// =============================================================
// MidiPro - sequencer/Player.cpp
// =============================================================

#include "sequencer/Player.h"

#include "midi/MidiConstants.h"
#include "sequencer/TimeBase.h"

#include <algorithm>
#include <chrono>

namespace midipro::seq {

using clock = std::chrono::steady_clock;

Player::Player(midi::IMidiOutput& output) : m_output(output) {}

Player::~Player() {
    stop();
}

void Player::play(const Song& song, uint32_t startTick, bool keepAlive, uint32_t preRollTicks) {
    stop(); // 진행 중이던 재생을 먼저 정리

    // 스냅샷 구성: 뮤트 안 된 트랙의 이벤트를 하나의 평탄한 목록으로
    // 모아 틱 순으로 정렬한다. 이 할당은 재생 시작 시 한 번뿐이다.
    m_snapshot.clear();
    for (const auto& track : song.tracks) {
        if (track.muted) continue;
        if (track.frozen) continue; // 프리즈: 구운 오디오가 대신 소리낸다
        for (const auto& e : track.events) {
            m_snapshot.push_back({e.tick, e.status, e.data1, e.data2, e.isThreeBytes()});
        }
    }
    std::stable_sort(m_snapshot.begin(), m_snapshot.end(),
                     [](const FlatEvent& a, const FlatEvent& b) { return a.tick < b.tick; });

    m_ppqn = song.ppqn;
    m_tempoSnap = song.tempoChanges; // 재생 중 불변 스냅샷 (틱 오름차순)
    m_preRoll.store(preRollTicks, std::memory_order_relaxed);
    m_keepAlive.store(keepAlive, std::memory_order_relaxed);
    m_bpm.store(song.bpm, std::memory_order_relaxed);
    m_startTick.store(startTick, std::memory_order_relaxed);
    m_currentTick.store(startTick, std::memory_order_relaxed);
    m_countingIn.store(preRollTicks > 0, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_playing.store(true, std::memory_order_release);

    m_thread = std::thread(&Player::run, this);
}

void Player::stop() {
    if (m_thread.joinable()) {
        m_stopRequested.store(true, std::memory_order_release);
        m_thread.join();
    }
    m_playing.store(false, std::memory_order_release);
}

void Player::run() {
    // 시작 틱 이전 이벤트는 건너뛰되, Program Change는 음색 유지를 위해
    // 지금 즉시 전송한다. (구간 재생·루프 시 악기가 바뀌지 않는 문제 방지)
    auto seekTo = [this](uint32_t tick) {
        std::size_t idx = 0;
        for (; idx < m_snapshot.size() && m_snapshot[idx].tick < tick; ++idx) {
            const auto& e = m_snapshot[idx];
            if ((e.status & midi::kStatusMask) == midi::kStatusProgramChange)
                m_output.send({e.status, e.data1});
        }
        return idx;
    };

    const int64_t startTick = (int64_t)m_startTick.load(std::memory_order_relaxed);
    const int64_t preRoll = (int64_t)m_preRoll.load(std::memory_order_relaxed);
    const int64_t originTick = startTick - preRoll; // elapsed=0 시점의 유효 틱(카운트인이면 음수)
    // 카운트인이 끝나는 지점. 시작 시엔 startTick이고, 루프 되감김 카운트인이
    // 켜져 있으면 매 바퀴 loopStart로 갱신된다 (effTick < preRollEnd = 카운트인 중).
    int64_t preRollEnd = startTick;
    std::size_t index = seekTo((uint32_t)(startTick < 0 ? 0 : startTick));

    // 시간을 누적식으로 잰다. BPM이 바뀌면 "이후 델타"에만 적용되므로
    // 재생 중 템포를 바꿔도 위치가 튀지 않고 부드럽게 반영된다.
    double accumTicks = (double)originTick;
    auto lastClock = clock::now();

    // 메트로놈 상태. 클릭 간격은 박자 단위(4=4분음표, 8=8분음표)에 따라 달라진다.
    auto floorDiv = [](int64_t a, int64_t b) { return (a >= 0 ? a / b : -((-a + b - 1) / b)); };
    const auto clickTicksNow = [this]() {
        const int unit = m_metroUnit.load(std::memory_order_relaxed);
        const int64_t base = m_ppqn > 0 ? m_ppqn : 480; // 4분음표 틱
        return unit == 8 ? std::max<int64_t>(1, base / 2) : base;
    };
    // 어떤 첫 클릭 인덱스와도 충돌하지 않는 초기값 (충돌하면 첫 클릭을 건너뛴다)
    int64_t lastBeat = INT64_MIN / 2;
    bool metroPending = false;
    int64_t metroOffTick = 0;
    uint8_t metroNote = 0;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // 지난 프레임 이후 경과 시간만큼만 현재 BPM으로 틱을 누적한다.
        const auto nowClock = clock::now();
        const double deltaSec = std::chrono::duration<double>(nowClock - lastClock).count();
        lastClock = nowClock;
        // 유효 BPM = 기본 템포(슬라이더) 또는 현재 틱 이전의 마지막 템포 지점.
        // ramp 지점이면 다음 지점까지 선형 보간 (점점 빠르게/느리게).
        double effBpm = m_bpm.load(std::memory_order_relaxed);
        for (std::size_t ti = 0; ti < m_tempoSnap.size(); ++ti) {
            const auto& tc = m_tempoSnap[ti];
            if ((double)tc.tick > accumTicks) break;
            if (tc.ramp && ti + 1 < m_tempoSnap.size() &&
                (double)m_tempoSnap[ti + 1].tick > (double)tc.tick &&
                accumTicks < (double)m_tempoSnap[ti + 1].tick) {
                const auto& nx = m_tempoSnap[ti + 1];
                const double f = (accumTicks - (double)tc.tick) /
                                 ((double)nx.tick - (double)tc.tick);
                effBpm = tc.bpm + (nx.bpm - tc.bpm) * f;
            } else {
                effBpm = tc.bpm;
            }
            if (effBpm < 1.0) effBpm = 1.0;
        }
        accumTicks += secondsToTicks(deltaSec, effBpm, m_ppqn);
        const int64_t effTick = (int64_t)accumTicks;

        // 메트로놈: 박자 설정(4/4, 3/4, 6/8)에 따라 클릭(채널 10).
        //  - 카운트인: 클릭을 "시작점 기준 상대 격자"로 낸다. 시작점이 마디
        //    중간이어도 정확히 N번 클릭 후 시작하고, 마지막 클릭만 강조된다.
        //  - 재생 중: 절대 마디 그리드(틱 0 기준)의 첫 박만 강조.
        const int64_t clickTicks = clickTicksNow();
        const bool countingIn = effTick < preRollEnd;
        m_countingIn.store(countingIn, std::memory_order_release);
        // 카운트인 전용 모드면 시작점 이후 클릭을 틱 기준으로 정확히 끊는다.
        const bool metroOn = m_metronome.load(std::memory_order_relaxed) &&
                             (countingIn || !m_metroCountInOnly.load(std::memory_order_relaxed));
        if (metroOn) {
            // 카운트인은 시작점 기준(rel = -N..-1), 재생은 절대 그리드 기준.
            const int64_t beat = countingIn ? floorDiv(effTick - preRollEnd, clickTicks)
                                            : floorDiv(effTick, clickTicks);
            if (beat != lastBeat) {
                lastBeat = beat;
                bool accent;
                if (countingIn) {
                    accent = (beat == -1); // 시작 직전(마지막) 클릭만 강조
                } else {
                    const int sigBeats =
                        std::max(1, m_metroBeats.load(std::memory_order_relaxed));
                    accent = (((beat % sigBeats) + sigBeats) % sigBeats) == 0;
                }
                if (accent)
                    metroNote = countingIn
                                    ? m_countInAccentNote.load(std::memory_order_relaxed)
                                    : m_accentNote.load(std::memory_order_relaxed);
                else
                    metroNote = countingIn ? m_countInNote.load(std::memory_order_relaxed)
                                           : m_metroNote.load(std::memory_order_relaxed);
                // 클릭 음량: 벨로시티 스케일 (0이면 이번 클릭을 아예 안 보낸다)
                const float cvol = countingIn ? m_countInVol.load(std::memory_order_relaxed)
                                              : m_metroVol.load(std::memory_order_relaxed);
                if (cvol > 0.001f) {
                    const int base = accent ? 112 : 80; // 강세는 세기로도
                    const int v = (int)((float)base * cvol);
                    const uint8_t vel = (uint8_t)(v < 1 ? 1 : (v > 127 ? 127 : v));
                    m_output.send({(uint8_t)(midi::kStatusNoteOn | 9), metroNote, vel});
                    metroPending = true;
                    metroOffTick = effTick + clickTicks / 8;
                }
            }
        }
        // 예약된 클릭 Note Off는 메트로놈이 도중에 꺼져도 반드시 보낸다.
        // (카운트인이 클릭 직후 끝나면 플래그가 꺼지는데, 이 처리가 if 안에
        //  있으면 Note Off가 누락돼 클릭 음이 계속 울린다)
        if (metroPending && effTick >= metroOffTick) {
            m_output.send({(uint8_t)(midi::kStatusNoteOff | 9), metroNote, 0});
            metroPending = false;
        }

        // 루프: 끝에 도달하면 노트를 끄고 구간 시작으로 되돌린다 (유효 틱>=0에서만)
        const int64_t loopStart = (int64_t)m_loopStart.load(std::memory_order_relaxed);
        const int64_t loopEnd = (int64_t)m_loopEnd.load(std::memory_order_relaxed);
        const bool looping = m_loopEnabled.load(std::memory_order_acquire) && loopEnd > loopStart;
        if (looping && effTick >= 0 && effTick >= loopEnd) {
            // 되감기 전에 예약된 클릭 Note Off를 먼저 보낸다. 틱 기준이 뒤로
            // 점프하므로 그대로 두면 다음 바퀴 그 지점까지 클릭이 눌려 있는다.
            if (metroPending) {
                m_output.send({(uint8_t)(midi::kStatusNoteOff | 9), metroNote, 0});
                metroPending = false;
            }
            allNotesOff();
            // 루프 카운트인: 켜져 있으면 loopStart 앞에 프리롤을 넣어 매 바퀴
            // 클릭 후 다시 시작한다. 이벤트는 seekTo가 loopStart 이전을 건너뛰어
            // 카운트인 동안 아무것도 안 나간다.
            const int64_t lpre = (int64_t)m_loopPreRoll.load(std::memory_order_relaxed);
            accumTicks = (double)(loopStart - lpre);
            preRollEnd = loopStart;
            index = seekTo((uint32_t)loopStart);
            lastClock = clock::now();
            // 카운트인이면 상대 격자 첫 클릭(-N)이 바로 나가도록 센티널로 리셋
            lastBeat = lpre > 0 ? INT64_MIN / 2 : floorDiv(loopStart, clickTicksNow()) - 1;
            // GUI가 이번 프레임에 "되감김 + 카운트인 중"을 함께 보도록 즉시 반영
            m_countingIn.store(lpre > 0, std::memory_order_release);
            m_currentTick.store((uint32_t)loopStart, std::memory_order_relaxed);
            continue;
        }

        // 카운트인 중에는 플레이헤드를 시작점(또는 루프 시작)에 고정해 보고한다
        m_currentTick.store(countingIn ? (uint32_t)(preRollEnd < 0 ? 0 : preRollEnd)
                                       : (uint32_t)(effTick < 0 ? 0 : effTick),
                            std::memory_order_relaxed);

        // 도달한 이벤트를 모두 전송 (유효 틱 기준)
        while (index < m_snapshot.size() && (int64_t)m_snapshot[index].tick <= effTick) {
            const auto& e = m_snapshot[index];
            if (e.threeBytes)
                m_output.send({e.status, e.data1, e.data2});
            else
                m_output.send({e.status, e.data1});
            ++index;
        }

        // 곡 끝: 루프/keepAlive면 계속, 아니면 멈춘다
        if (index >= m_snapshot.size() && !m_keepAlive.load(std::memory_order_relaxed) && !looping)
            break;

        // 왜 1ms 슬립인가: 다음 이벤트까지 바쁜 대기로 CPU를 태우지
        // 않으면서도, 사람이 못 느낄 만큼의 타이밍 오차만 남긴다.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    allSoundOff(); // 정지 시 잔음까지 즉시 끊는다
    m_countingIn.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
}

void Player::allNotesOff() {
    // 모든 채널에 All Notes Off(CC 123) — 릴리스로 부드럽게 끈다 (루프 경계용).
    constexpr uint8_t kCcAllNotesOff = 123;
    for (uint8_t ch = 0; ch < 16; ++ch)
        m_output.send({(uint8_t)(midi::kStatusControlChange | ch), kCcAllNotesOff, 0});
}

void Player::allSoundOff() {
    // All Sound Off(120) + All Notes Off(123) — 즉시 무음으로 잔음을 끊는다.
    for (uint8_t ch = 0; ch < 16; ++ch) {
        m_output.send({(uint8_t)(midi::kStatusControlChange | ch), 120, 0});
        m_output.send({(uint8_t)(midi::kStatusControlChange | ch), 123, 0});
    }
}

} // namespace midipro::seq
