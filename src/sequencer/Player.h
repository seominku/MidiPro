#pragma once
// =============================================================
// MidiPro - sequencer/Player.h
// 재생 엔진: Song을 시간에 맞춰 IMidiOutput으로 내보낸다.
//
// 스레딩 모델 (Rule 3):
//   재생은 전용 스레드에서 돈다. GUI 스레드와의 통신은 아토믹
//   변수(재생 상태/현재 틱/BPM)로만 하고, 곡 데이터는 재생
//   시작 시점의 스냅샷을 복사해 쓴다. 재생 중 GUI가 곡을
//   수정해도 스냅샷은 안 바뀌므로 락이 필요 없다.
//
//   전용 스레드는 오디오 콜백만큼 엄격한 실시간은 아니지만,
//   같은 원칙(락/할당 최소화)을 따른다. 스냅샷 벡터는 시작 시
//   한 번만 할당하고 재생 루프에서는 인덱스만 전진시킨다.
//
// 계층 규칙 (Rule 1):
//   구체 장치가 아니라 IMidiOutput 인터페이스에만 의존한다.
// =============================================================

#include "midi/IMidiDevice.h"
#include "sequencer/MidiEvent.h"
#include "sequencer/Song.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace midipro::seq {

class Player {
public:
    explicit Player(midi::IMidiOutput& output);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // 현재 곡 상태를 스냅샷으로 복사해 재생을 시작한다.
    // startTick부터 재생하며, 뮤트된 트랙은 스냅샷에서 제외한다.
    // keepAlive=true면 곡이 끝나거나 비어 있어도 트랜스포트 클럭을
    // 계속 돌린다(녹음 중 클럭 확보용). false면 마지막 이벤트에서 멈춘다.
    // preRollTicks>0이면 startTick 이전으로 그만큼 카운트인(빈 프리롤)한다.
    void play(const Song& song, uint32_t startTick = 0, bool keepAlive = false,
              uint32_t preRollTicks = 0);

    // 메트로놈: 켜면 재생/녹음 중 박마다 클릭음(채널 10)을 낸다.
    void setMetronome(bool enabled) { m_metronome.store(enabled, std::memory_order_relaxed); }
    // 카운트인 전용 모드: 시작점(startTick) 이후에는 클릭을 내지 않는다.
    // (사용자가 메트로놈을 껐는데 카운트인만 켠 경우 — GUI 타이머로 끄면
    //  마지막 강조 뒤에 시작 박 클릭이 한 번 더 새어 나온다)
    void setMetronomeCountInOnly(bool onlyCountIn) {
        m_metroCountInOnly.store(onlyCountIn, std::memory_order_relaxed);
    }
    // 클릭 음: 일반/강조를 메트로놈과 카운트인 각각 따로 지정한다.
    void setClickNotes(uint8_t metroNote, uint8_t countInNote, uint8_t metroAccentNote,
                       uint8_t countInAccentNote) {
        m_metroNote.store(metroNote, std::memory_order_relaxed);
        m_countInNote.store(countInNote, std::memory_order_relaxed);
        m_accentNote.store(metroAccentNote, std::memory_order_relaxed);
        m_countInAccentNote.store(countInAccentNote, std::memory_order_relaxed);
    }

    // 클릭 음량: 메트로놈/카운트인 각각 (0=무음, 1=기본, 1.5=크게).
    // 벨로시티를 스케일하므로 신스 음이든 샘플이든 똑같이 적용된다.
    void setClickVolumes(float metroVol, float countInVol) {
        m_metroVol.store(metroVol < 0.0f ? 0.0f : metroVol, std::memory_order_relaxed);
        m_countInVol.store(countInVol < 0.0f ? 0.0f : countInVol, std::memory_order_relaxed);
    }

    // 메트로놈 박자: beats/unit (예: 4/4, 3/4, 6/8). unit=8이면 8분음표 클릭.
    void setMetroSignature(int beats, int unit) {
        if (beats < 1) beats = 1;
        if (unit != 8) unit = 4;
        m_metroBeats.store(beats, std::memory_order_relaxed);
        m_metroUnit.store(unit, std::memory_order_relaxed);
    }
    // 재생 중에도 템포(BPM)를 실시간으로 바꾼다.
    void setBpm(double bpm) { m_bpm.store(bpm, std::memory_order_relaxed); }
    void stop();          // 정지 + 모든 노트 오프
    bool isPlaying() const { return m_playing.load(std::memory_order_acquire); }
    // 카운트인(프리롤) 진행 중인가. GUI가 녹음 시작 시점을 "플레이어 시계"
    // 기준으로 정확히 맞추는 데 쓴다 (벽시계 타이머는 어긋난다).
    bool isCountingIn() const { return m_countingIn.load(std::memory_order_acquire); }

    // 루프 되감김 카운트인: 틱>0이면 매 바퀴 되감을 때마다 루프 시작 전에
    // 그만큼 프리롤(클릭)한 뒤 다시 시작한다. 0이면 즉시 되감는다.
    void setLoopCountIn(uint32_t ticks) {
        m_loopPreRoll.store(ticks, std::memory_order_relaxed);
    }

    // 루프 구간 [startTick, endTick). 재생 중에도 켜고 끌 수 있다.
    void setLoop(bool enabled, uint32_t startTick, uint32_t endTick) {
        m_loopStart.store(startTick, std::memory_order_relaxed);
        m_loopEnd.store(endTick, std::memory_order_relaxed);
        m_loopEnabled.store(enabled, std::memory_order_release);
    }

    // GUI가 재생 헤드 위치를 읽어 피아노 롤에 표시하는 데 쓴다.
    uint32_t currentTick() const { return m_currentTick.load(std::memory_order_relaxed); }

private:
    void run(); // 재생 스레드 본체
    void allNotesOff();  // CC123 (릴리스) — 루프 경계 등
    void allSoundOff();  // CC120+CC123 (즉시 무음) — 정지 시

    // 재생 스레드에서만 접근하는 스냅샷 (시작 시 채워지고 정지까지 불변)
    struct FlatEvent {
        uint32_t tick;
        uint8_t status, data1, data2;
        bool threeBytes;
    };

    midi::IMidiOutput& m_output;
    std::thread m_thread;
    std::vector<FlatEvent> m_snapshot; // tick 정렬된 전체 이벤트
    // 템포 변경 지점 스냅샷 (재생 시작 시 복사, 재생 중 불변 -> 락 불필요).
    // 유효 BPM = 현재 틱 이전의 마지막 지점. 없으면 m_bpm(기본 템포).
    std::vector<TempoChange> m_tempoSnap;
    int m_ppqn = kDefaultPpqn;

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_keepAlive{false};
    std::atomic<bool> m_loopEnabled{false};
    std::atomic<uint32_t> m_loopStart{0};
    std::atomic<uint32_t> m_loopEnd{0};
    std::atomic<bool> m_metronome{false};
    std::atomic<bool> m_countingIn{false};       // 프리롤 진행 중 (재생 스레드가 갱신)
    std::atomic<bool> m_metroCountInOnly{false}; // 시작점 이후 클릭 금지
    std::atomic<uint8_t> m_metroNote{77};   // 메트로놈 클릭 음
    std::atomic<uint8_t> m_countInNote{84}; // 카운트인 클릭 음 (구분되게 다른 기본값)
    std::atomic<uint8_t> m_accentNote{88};        // 메트로놈 강조(다운비트) 클릭 음
    std::atomic<uint8_t> m_countInAccentNote{91}; // 카운트인 강조 클릭 음
    std::atomic<float> m_metroVol{1.0f};   // 메트로놈 클릭 음량
    std::atomic<float> m_countInVol{1.0f}; // 카운트인 클릭 음량
    std::atomic<int> m_metroBeats{4};       // 한 마디의 박 수 (4/4=4, 3/4=3, 6/8=6)
    std::atomic<int> m_metroUnit{4};        // 박 단위 (4=4분음표, 8=8분음표)
    std::atomic<uint32_t> m_preRoll{0};
    std::atomic<uint32_t> m_loopPreRoll{0}; // 루프 되감김마다 카운트인할 틱 (0=없음)
    std::atomic<double> m_bpm{120.0};
    std::atomic<uint32_t> m_currentTick{0};
    std::atomic<uint32_t> m_startTick{0};
};

} // namespace midipro::seq
