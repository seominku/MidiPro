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
        for (const auto& e : track.events) {
            m_snapshot.push_back({e.tick, e.status, e.data1, e.data2, e.isThreeBytes()});
        }
    }
    std::stable_sort(m_snapshot.begin(), m_snapshot.end(),
                     [](const FlatEvent& a, const FlatEvent& b) { return a.tick < b.tick; });

    m_ppqn = song.ppqn;
    m_preRoll.store(preRollTicks, std::memory_order_relaxed);
    m_keepAlive.store(keepAlive, std::memory_order_relaxed);
    m_bpm.store(song.bpm, std::memory_order_relaxed);
    m_startTick.store(startTick, std::memory_order_relaxed);
    m_currentTick.store(startTick, std::memory_order_relaxed);
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
    int64_t baseTick = startTick;                 // 곡 이벤트 기준 (틱)
    int64_t originTick = startTick - preRoll;      // elapsed=0 시점의 유효 틱(카운트인이면 음수)
    std::size_t index = seekTo((uint32_t)baseTick);
    auto wallStart = clock::now();

    // 메트로놈 상태
    const int64_t beatTicks = m_ppqn > 0 ? m_ppqn : 480;
    auto floorDiv = [](int64_t a, int64_t b) { return (a >= 0 ? a / b : -((-a + b - 1) / b)); };
    int64_t lastBeat = floorDiv(originTick, beatTicks) - 1;
    bool metroPending = false;
    int64_t metroOffTick = 0;
    uint8_t metroNote = 0;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // 경과 실시간 -> 경과 틱으로 환산 (BPM은 재생 중 변경 가능)
        const double bpm = m_bpm.load(std::memory_order_relaxed);
        const double elapsedSec =
            std::chrono::duration<double>(clock::now() - wallStart).count();
        const int64_t effTick = originTick + (int64_t)secondsToTicks(elapsedSec, bpm, m_ppqn);

        // 메트로놈: 유효 틱 기준 박마다 클릭(채널 10). 카운트인(음수)에도 울린다.
        if (m_metronome.load(std::memory_order_relaxed)) {
            const int64_t beat = floorDiv(effTick, beatTicks);
            if (beat != lastBeat) {
                lastBeat = beat;
                const int bb = (int)(((beat % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar);
                metroNote = (bb == 0) ? 76 : 77;                   // 다운비트 강세
                const uint8_t vel = (bb == 0) ? 112 : 80;
                m_output.send({(uint8_t)(midi::kStatusNoteOn | 9), metroNote, vel});
                metroPending = true;
                metroOffTick = effTick + beatTicks / 8;
            }
            if (metroPending && effTick >= metroOffTick) {
                m_output.send({(uint8_t)(midi::kStatusNoteOff | 9), metroNote, 0});
                metroPending = false;
            }
        }

        // 루프: 끝에 도달하면 노트를 끄고 구간 시작으로 되돌린다 (유효 틱>=0에서만)
        const int64_t loopStart = (int64_t)m_loopStart.load(std::memory_order_relaxed);
        const int64_t loopEnd = (int64_t)m_loopEnd.load(std::memory_order_relaxed);
        const bool looping = m_loopEnabled.load(std::memory_order_acquire) && loopEnd > loopStart;
        if (looping && effTick >= 0 && effTick >= loopEnd) {
            allNotesOff();
            baseTick = loopStart;
            originTick = loopStart;
            index = seekTo((uint32_t)loopStart);
            wallStart = clock::now();
            lastBeat = floorDiv(loopStart, beatTicks) - 1;
            m_currentTick.store((uint32_t)loopStart, std::memory_order_relaxed);
            continue;
        }

        // 카운트인 중(음수)에는 0으로 보고, 곡 이벤트는 보내지 않는다
        m_currentTick.store(effTick < 0 ? 0u : (uint32_t)effTick, std::memory_order_relaxed);

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

    allNotesOff();
    m_playing.store(false, std::memory_order_release);
}

void Player::allNotesOff() {
    // 정지 시 울리던 노트가 계속 나지 않도록 모든 채널에
    // All Notes Off(CC 123)를 보낸다.
    constexpr uint8_t kCcAllNotesOff = 123;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        m_output.send({(uint8_t)(midi::kStatusControlChange | ch), kCcAllNotesOff, 0});
    }
}

} // namespace midipro::seq
