#pragma once
// =============================================================
// MidiPro - audio/RtAudioEngine.h
// RtAudio 기반 내장 신디사이저 출력.
//
// 두 인터페이스를 겸한다:
//   - IMidiOutput: Player/GUI가 하드웨어 포트와 똑같이 신스로
//     MIDI를 보낼 수 있게 한다 (openPort=스트림 시작, send=이벤트 큐잉).
//   - 추가로 setParams(): 음색 파라미터를 GUI에서 조절.
//
// 스레딩 (Rule 3):
//   오디오 콜백(실시간 스레드)은 큐에서 이벤트를 pop(락프리)해
//   Synth에 적용하고 렌더만 한다. 할당/락/로깅 없음. 여러 제어
//   스레드(GUI, 재생)가 send/setParams로 넣는 것은 프로듀서
//   뮤텍스로 직렬화하지만, 소비자(오디오)는 절대 락하지 않는다.
//
// RtAudio 헤더를 이 헤더에 include하지 않으려고 전방 선언만 둔다.
// =============================================================

#include "audio/IMidi2Input.h"
#include "audio/ISynthControl.h"
#include "audio/IVstHostControl.h"
#include "audio/Synth.h"
#include "core/SpscQueue.h"
#include "midi/IMidiDevice.h"
#include "vst/Vst3Host.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class RtAudio;

namespace midipro::audio {

class RtAudioEngine final : public midi::IMidiOutput,
                            public ISynthControl,
                            public IVstHostControl,
                            public IMidi2Input {
public:
    RtAudioEngine();
    ~RtAudioEngine() override;

    // ---- IMidiOutput ----
    std::vector<std::string> listPorts() override; // {"내장 신디사이저"}
    bool openPort(unsigned index) override;         // 오디오 스트림 시작
    void closePort() override;                      // 스트림 정지
    bool isOpen() const override;
    bool send(const std::vector<uint8_t>& bytes) override; // MIDI 1.0 이벤트 큐잉

    // ---- IMidi2Input ----
    bool sendUmp(const uint32_t* words, int count) override; // MIDI 2.0 UMP 큐잉

    // ---- ISynthControl ----
    void setParams(const SynthParams& params) override; // GUI 스레드에서 호출
    int activeVoiceCount() const override { return m_activeVoices.load(std::memory_order_relaxed); }
    std::vector<std::string> listOutputDevices() override;
    int outputDevice() const override;
    void setOutputDevice(int index) override;
    double currentSampleRate() const override { return m_sampleRate; }

    // MPE 모드: 내장 신스의 피치벤드 범위를 멤버 채널 기본(±48)/일반(±2)로.
    void setMpeMode(bool enabled) override {
        m_mpeEnabled.store(enabled, std::memory_order_relaxed);
    }
    bool mpeMode() const override { return m_mpeEnabled.load(std::memory_order_relaxed); }

    // ---- IVstHostControl (GUI 스레드 전용; 내부에서 스트림을 잠깐 멈춰 안전하게 로드) ----
    vst::Vst3Host& instrumentHost() override { return m_instrument; }
    vst::Vst3Host& effectHost() override { return m_effect; }
    bool loadInstrument(const std::string& path, int classIndex, std::string& err) override;
    bool loadEffect(const std::string& path, int classIndex, std::string& err) override;
    void clearInstrument() override;
    void clearEffect() override;
    bool instrumentActive() const override { return m_instrument.isLoaded(); }
    bool effectActive() const override { return m_effect.isLoaded(); }

private:
    // 오디오 스레드에 전달되는 명령
    struct EngineEvent {
        enum class Type : uint8_t {
            NoteOn, NoteOff, AllNotesOff, SetParams,
            PitchBend, Pressure, Timbre, PerNotePitchBend
        };
        Type type = Type::NoteOn;
        uint8_t channel = 0;
        uint8_t note = 0;
        uint8_t velocity = 0; // VST용 7비트 벨로시티
        float value = 0.0f;   // NoteOn 벨로시티(0~1) / 벤드(-1~1) / 압력·음색(0~1)
        SynthParams params;
    };

    static int rtCallback(void* output, void* input, unsigned frames, double streamTime,
                          unsigned status, void* userData);
    void processCallback(float* output, unsigned frames);
    bool pushEvent(const EngineEvent& e); // 프로듀서 뮤텍스로 직렬화

    static constexpr std::size_t kQueueCapacity = 2048;

    std::unique_ptr<RtAudio> m_audio;
    Synth m_synth;
    core::SpscQueue<EngineEvent, kQueueCapacity> m_queue;
    std::mutex m_producerMutex; // 제어 스레드 간 push 직렬화 (오디오 스레드는 미사용)

    double m_sampleRate = 44100.0;
    unsigned m_bufferFrames = 512;
    unsigned m_deviceId = 0;         // 0=기본 출력. 사용자가 고른 오디오 장치 id
    std::vector<unsigned> m_outIds;  // 마지막 listOutputDevices() 결과의 장치 id(인덱스 대응)
    std::vector<float> m_monoBuffer; // 콜백용 사전 할당 버퍼 (내장 신스)

    // VST3 호스트 + 스테레오 planar 작업 버퍼 (사전 할당, Rule 3)
    vst::Vst3Host m_instrument; // 악기 VSTi (내장 신스 대체)
    vst::Vst3Host m_effect;     // 이펙트 (출력 후처리)
    std::vector<float> m_planarL;
    std::vector<float> m_planarR;
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_mpeEnabled{false};
    std::atomic<int> m_activeVoices{0};
    std::atomic<std::size_t> m_dropped{0};
};

} // namespace midipro::audio
