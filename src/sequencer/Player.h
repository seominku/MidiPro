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
    void stop();          // 정지 + 모든 노트 오프
    bool isPlaying() const { return m_playing.load(std::memory_order_acquire); }

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
    void allNotesOff();

    // 재생 스레드에서만 접근하는 스냅샷 (시작 시 채워지고 정지까지 불변)
    struct FlatEvent {
        uint32_t tick;
        uint8_t status, data1, data2;
        bool threeBytes;
    };

    midi::IMidiOutput& m_output;
    std::thread m_thread;
    std::vector<FlatEvent> m_snapshot; // tick 정렬된 전체 이벤트
    int m_ppqn = kDefaultPpqn;

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_keepAlive{false};
    std::atomic<bool> m_loopEnabled{false};
    std::atomic<uint32_t> m_loopStart{0};
    std::atomic<uint32_t> m_loopEnd{0};
    std::atomic<bool> m_metronome{false};
    std::atomic<uint32_t> m_preRoll{0};
    std::atomic<double> m_bpm{120.0};
    std::atomic<uint32_t> m_currentTick{0};
    std::atomic<uint32_t> m_startTick{0};
};

} // namespace midipro::seq
