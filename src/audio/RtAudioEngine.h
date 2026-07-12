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

#include "audio/AudioClip.h"
#include "audio/BuiltinFx.h"
#include "audio/IAudioClips.h"
#include "audio/IAudioInput.h"
#include "audio/IMidi2Input.h"
#include "audio/ISynthControl.h"
#include "audio/IVstHostControl.h"
#include "audio/Synth.h"
#include "core/SpscQueue.h"
#include "midi/IMidiDevice.h"
#include "vst/Vst3Host.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RtAudio;

namespace midipro::audio {

class RtAudioEngine final : public midi::IMidiOutput,
                            public ISynthControl,
                            public IVstHostControl,
                            public IMidi2Input,
                            public IAudioClips,
                            public IAudioInput {
public:
    RtAudioEngine();
    ~RtAudioEngine() override;

    // ---- IMidiOutput ----
    std::vector<std::string> listPorts() override; // {"내장 신디사이저"}
    bool openPort(unsigned index) override;         // 오디오 스트림 시작
    void closePort() override;                      // 스트림 정지
    bool isOpen() const override;
    bool send(const std::vector<uint8_t>& bytes) override; // MIDI 1.0 이벤트 큐잉
    double outputLatencySeconds() const override {         // 오디오 버퍼/장치 지연
        return m_latencySec.load(std::memory_order_relaxed);
    }

    // ---- IMidi2Input ----
    bool sendUmp(const uint32_t* words, int count) override; // MIDI 2.0 UMP 큐잉

    // ---- IAudioClips (오디오 클립 재생) ----
    void setAudioMix(std::shared_ptr<const std::vector<AudioMixClip>> clips) override;
    void startAudio(int64_t startFrame) override;
    void stopAudio() override;
    void seekAudio(int64_t frame) override;
    double engineSampleRate() const override { return m_sampleRate; }
    double inputLatencySeconds() const override {
        return m_inLatencySec.load(std::memory_order_relaxed);
    }
    uint32_t inputTapWritePos() const override {
        return m_inTapPos.load(std::memory_order_acquire);
    }
    bool readInputTapRange(uint32_t from, float* dst, int n) override {
        if (m_inTap.empty() || n <= 0 || n > (int)kTapSize) return false;
        const uint32_t p = m_inTapPos.load(std::memory_order_acquire);
        if ((uint32_t)(p - from) > kTapSize) return false;      // 미래이거나 너무 오래됨
        if (p - from < (uint32_t)n) return false;               // 아직 덜 쓰였다
        if (p - from > kTapSize - 512) return false;            // 곧 덮인다 — 신뢰 불가
        for (int i = 0; i < n; ++i)
            dst[i] = m_inTap[(from + (uint32_t)i) & (kTapSize - 1)];
        return true;
    }
    // 연습 모드: 최근 입력 n개를 dst로 복사 (링에서 최신 구간)
    int readInputTap(float* dst, int n) override {
        if (m_inTap.empty() || n <= 0) return 0;
        if (n > (int)kTapSize) n = (int)kTapSize;
        const uint32_t p = m_inTapPos.load(std::memory_order_acquire);
        if (p < (uint32_t)n) return 0; // 아직 데이터가 모자란다
        for (int i = 0; i < n; ++i)
            dst[i] = m_inTap[(p - (uint32_t)n + (uint32_t)i) & (kTapSize - 1)];
        return n;
    }
    void setMasterGain(float gain) override {
        m_masterGain.store(gain, std::memory_order_relaxed);
    }
    void setMasterPan(float pan) override {
        m_masterPan.store(pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan),
                          std::memory_order_relaxed);
    }
    void setClickPitches(uint8_t metroNote, uint8_t countInNote, uint8_t accentNote,
                         uint8_t countInAccentNote) override {
        m_clickNote[kClickMetro].store(metroNote, std::memory_order_relaxed);
        m_clickNote[kClickCountIn].store(countInNote, std::memory_order_relaxed);
        m_clickNote[kClickAccent].store(accentNote, std::memory_order_relaxed);
        m_clickNote[kClickCountInAccent].store(countInAccentNote, std::memory_order_relaxed);
        // 신스 대체용 "삑" 클립도 준비한다 (GUI 스레드 — 할당 허용).
        // 클릭이 버스(트랙 볼륨/뮤트/FX)를 타지 않고 마스터로 직행하게 한다.
        ensureClickBeep(kClickMetro, metroNote);
        ensureClickBeep(kClickCountIn, countInNote);
        ensureClickBeep(kClickAccent, accentNote);
        ensureClickBeep(kClickCountInAccent, countInAccentNote);
    }
    void setClickSample(int kind, std::shared_ptr<const AudioClip> clip) override {
        if (kind < 0 || kind >= kClickKinds) return;
        std::atomic_store(&m_clickSample[kind],
                          std::shared_ptr<const AudioClip>(std::move(clip)));
    }
    void setDrumSample(uint8_t note, std::shared_ptr<const AudioClip> clip) override {
        std::atomic_store(&m_drumSample[note & 0x7F],
                          std::shared_ptr<const AudioClip>(std::move(clip)));
    }
    void beginOfflineRender(int64_t startFrame) override;
    bool queueMidi(uint8_t status, uint8_t data1, uint8_t data2) override {
        return send({status, data1, data2});
    }
    // 오프라인 렌더 공통 주의: VST는 스트림 버퍼 크기(m_bufferFrames)로
    // setupProcessing 돼 있어, 그보다 큰 블록을 한 번에 주면 Vst3Host가
    // maxBlock까지만 처리하고 나머지가 무음으로 남는다(뚝뚝 끊김).
    // 그래서 요청 블록을 안전 크기로 쪼개서 렌더한다.
    void renderOfflineBlock(float* interleavedOut, unsigned frames) override {
        // 스트림이 멈춰 있으므로 GUI 스레드가 콜백 본체를 직접 호출해도 안전하다
        const unsigned step = m_bufferFrames > 0 ? (unsigned)m_bufferFrames : 512u;
        for (unsigned off = 0; off < frames;) {
            const unsigned n = frames - off < step ? frames - off : step;
            processCallback(interleavedOut + (std::size_t)off * 2, n);
            off += n;
        }
    }
    void renderOfflineBlockBus(int bus, float* interleavedOut, unsigned frames,
                               bool preFx) override {
        bus = bus < 0 ? 0 : (bus >= kBuses ? kBuses - 1 : bus);
        // preFx: FX "전" 악기 출력만 (프리즈 — 체인은 재생 때 걸린다)
        m_offlineSkipChains.store(preFx, std::memory_order_relaxed);
        const unsigned step = m_bufferFrames > 0 ? (unsigned)m_bufferFrames : 512u;
        for (unsigned off = 0; off < frames;) {
            unsigned n = frames - off < step ? frames - off : step;
            if (n > (unsigned)m_monoBuffer.size()) n = (unsigned)m_monoBuffer.size();
            renderBlock(n);
            for (unsigned i = 0; i < n; ++i) {
                interleavedOut[(std::size_t)(off + i) * 2 + 0] = m_busL[bus][i];
                interleavedOut[(std::size_t)(off + i) * 2 + 1] = m_busR[bus][i];
            }
            off += n;
        }
        m_offlineSkipChains.store(false, std::memory_order_relaxed);
    }
    void renderOfflineBlockReturn(float* interleavedOut, unsigned frames) override {
        // 리턴 버스(공용 리버브) 출력만 뽑는다 — 리버브 스템 내보내기용
        const unsigned step = m_bufferFrames > 0 ? (unsigned)m_bufferFrames : 512u;
        const float lvl = m_returnLevel.load(std::memory_order_relaxed);
        for (unsigned off = 0; off < frames;) {
            unsigned n = frames - off < step ? frames - off : step;
            if (n > (unsigned)m_monoBuffer.size()) n = (unsigned)m_monoBuffer.size();
            renderBlock(n); // processSendReturn이 m_retL/R을 채운다
            for (unsigned i = 0; i < n; ++i) {
                interleavedOut[(std::size_t)(off + i) * 2 + 0] = m_retL[i] * lvl;
                interleavedOut[(std::size_t)(off + i) * 2 + 1] = m_retR[i] * lvl;
            }
            off += n;
        }
    }
    void endOfflineRender() override;
    void setBusFrozen(int bus, bool frozen) override {
        if (bus >= 0 && bus < kBuses)
            m_busFrozen[bus].store(frozen, std::memory_order_relaxed);
    }
    float audioLoad() const override { return m_audioLoad.load(std::memory_order_relaxed); }
    unsigned engineBufferFrames() const override { return (unsigned)m_bufferFrames; }
    float pollBusPeak(int bus) override {
        if (bus < 0 || bus >= kBuses) return 0.0f;
        return m_busPeak[bus].exchange(0.0f, std::memory_order_relaxed);
    }
    void pollMasterPeak(float& outL, float& outR) override {
        outL = m_masterPeakL.exchange(0.0f, std::memory_order_relaxed);
        outR = m_masterPeakR.exchange(0.0f, std::memory_order_relaxed);
    }
    void startMasterCapture(double maxSeconds) override;
    std::shared_ptr<AudioClip> stopMasterCapture() override;
    bool masterCapturing() const override {
        return m_capturing.load(std::memory_order_acquire);
    }
    void setMasterLimiter(bool on) override {
        m_limiterOn.store(on, std::memory_order_relaxed);
    }
    bool masterLimiterOn() const override {
        return m_limiterOn.load(std::memory_order_relaxed);
    }
    BuiltinFx* masterLimiter() override { return &m_masterLimiter; }
    void setBusSend(int bus, float level) override {
        if (bus >= 0 && bus < kBuses)
            m_busSend[bus].store(level < 0.0f ? 0.0f : level, std::memory_order_relaxed);
    }
    void setMonitorBus(int bus) override {
        m_monitorBus.store(bus < 0 || bus >= kBuses ? -1 : bus, std::memory_order_relaxed);
    }
    void setReturnLevel(float level) override {
        m_returnLevel.store(level < 0.0f ? 0.0f : level, std::memory_order_relaxed);
    }
    float returnLevel() const override {
        return m_returnLevel.load(std::memory_order_relaxed);
    }
    BuiltinFx* returnReverb() override { return &m_returnReverb; }

    // ---- ISynthControl ----
    void setParams(const SynthParams& params) override; // GUI 스레드에서 호출
    void setChannelMix(int channel, float gain, float pan) override;
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
    void setEffectBypass(bool bypass) override {
        m_effectBypass.store(bypass, std::memory_order_relaxed);
    }
    bool effectBypassed() const override {
        return m_effectBypass.load(std::memory_order_relaxed);
    }
    void setInstrumentBus(int bus) override {
        m_instrumentBus.store(bus < -1 || bus >= kBuses ? -1 : bus, std::memory_order_relaxed);
    }
    int instrumentBus() const override { return m_instrumentBus.load(std::memory_order_relaxed); }
    bool loadTrackEffect(int channel, const std::string& path, int classIndex,
                         std::string& err) override;
    void removeTrackEffect(int channel, int index) override;
    void moveTrackEffect(int channel, int from, int to) override;
    void clearTrackEffects(int channel) override;
    int trackEffectCount(int channel) const override;
    std::string trackEffectName(int channel, int index) const override;
    bool trackEffectEnabled(int channel, int index) const override;
    void setTrackEffectEnabled(int channel, int index, bool on) override;
    vst::Vst3Host* trackEffectHost(int channel, int index) override;
    bool addBuiltinTrackEffect(int channel, int type) override;
    BuiltinFx* trackEffectBuiltin(int channel, int index) override;
    void setTrackEffectSidechain(int channel, int index, int bus) override;
    int trackEffectSidechain(int channel, int index) const override;
    bool pluginHasEffectClass(const std::string& path) override;
    bool pluginHasInstrumentClass(const std::string& path) override;
    bool loadTrackInstrument(int channel, const std::string& path, int classIndex,
                             std::string& err) override;
    void clearTrackInstrument(int channel) override;
    bool trackInstrumentActive(int channel) const override;
    std::string trackInstrumentName(int channel) const override;
    vst::Vst3Host* trackInstrumentHost(int channel) override;

    // ---- IAudioInput (마이크/인터페이스 캡처) ----
    std::vector<std::string> listInputDevices() override;
    int inputDevice() const override;
    void setInputDevice(int index) override;
    void setInputChannelMode(int mode) override;
    int inputChannelMode() const override { return m_inMode; }
    void setBufferFrames(unsigned frames) override;
    unsigned bufferFrames() const override { return m_bufferFrames; }
    bool startInput() override;
    void stopInput() override;
    bool inputActive() const override { return m_inOpen.load(std::memory_order_acquire); }
    void setMonitor(bool on) override { m_monitor.store(on, std::memory_order_relaxed); }
    bool monitorOn() const override { return m_monitor.load(std::memory_order_relaxed); }
    void setMonitorGain(float gain) override {
        m_monitorGain.store(gain, std::memory_order_relaxed);
    }
    void startRecording() override;
    void pumpRecording() override;
    std::shared_ptr<AudioClip> stopRecording() override;
    bool isRecording() const override { return m_recording.load(std::memory_order_acquire); }
    float inputLevel() const override { return m_inLevel.load(std::memory_order_relaxed); }
    bool asioAvailable() const override { return true; } // ASIO 컴파일됨(장치는 검색 시 로드)
    std::vector<std::string> listAsioDevices() override;
    bool startAsio(int deviceIndex, int channelMode) override;
    void stopAsio() override;
    bool asioActive() const override { return m_asioOn.load(std::memory_order_acquire); }

private:
    // 오디오 스레드에 전달되는 명령
    struct EngineEvent {
        enum class Type : uint8_t {
            NoteOn, NoteOff, AllNotesOff, AllSoundOff, SetParams,
            PitchBend, Pressure, Timbre, PerNotePitchBend, ChannelMix,
            ControlChange // 일반 CC (note = CC 번호, value = 0~1) -> VST로 전달
        };
        Type type = Type::NoteOn;
        uint8_t channel = 0;
        uint8_t note = 0;
        uint8_t velocity = 0; // VST용 7비트 벨로시티
        float value = 0.0f;   // NoteOn 벨로시티(0~1) / 벤드(-1~1) / 압력·음색(0~1) / 채널 게인
        float value2 = 0.0f;  // ChannelMix: 팬(-1~1)
        SynthParams params;
    };

    static int rtCallback(void* output, void* input, unsigned frames, double streamTime,
                          unsigned status, void* userData);
    void processCallback(float* output, unsigned frames);
    void renderBlock(unsigned frames); // 신스/VST/클립 -> m_planarL/R (공용)
    bool pushEvent(const EngineEvent& e); // 프로듀서 뮤텍스로 직렬화

    // 입력(캡처) 콜백: 마이크 프레임을 mono로 섞어 링/녹음 버퍼에 넣는다.
    static int inCallback(void* output, void* input, unsigned frames, double streamTime,
                          unsigned status, void* userData);
    void inputCapture(const float* input, unsigned frames);

    // ASIO 듀플렉스 콜백: 렌더 + 입력을 바로 출력에 섞어 저지연 모니터.
    static int asioCallback(void* output, void* input, unsigned frames, double streamTime,
                            unsigned status, void* userData);
    void asioProcess(float* output, const float* input, unsigned frames);
    bool ensureAsio(); // ASIO 인스턴스를 필요할 때만 생성(시작 시 드라이버 로드 회피)

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

    // 마스터 리미터: 볼륨/팬 적용 뒤 최종 단계에서 피크를 눌러 클리핑을 막는다
    BuiltinFx m_masterLimiter{BuiltinFx::kLimiter};
    std::atomic<bool> m_limiterOn{true};

    // ASIO 모니터 입력 라우팅: 담당 트랙 버스에 체인 "앞"에서 섞어 모니터에
    // 그 트랙의 FX가 걸리게 한다 (-1 = 예전처럼 마스터 직행). 녹음은 항상 드라이.
    std::atomic<int> m_monitorBus{-1};
    std::vector<float> m_monBuf; // 이번 콜백의 모니터 입력 (모노, 사전 할당)
    unsigned m_monCount = 0;     // renderBlock이 소비 (오디오 스레드 전용)

    // 센드/리턴 버스: 트랙들이 공용 리버브 하나를 나눠 쓴다 (Send 노브)
    std::atomic<float> m_busSend[16] = {};
    BuiltinFx m_returnReverb{BuiltinFx::kReverb};
    std::atomic<float> m_returnLevel{1.0f};
    std::vector<float> m_retL, m_retR;

    // VST3 호스트 + 스테레오 planar 작업 버퍼 (사전 할당, Rule 3)
    vst::Vst3Host m_instrument; // 악기 VSTi (내장 신스 대체)
    vst::Vst3Host m_effect;     // 마스터 이펙트 (최종 출력 후처리)
    std::vector<float> m_planarL;
    std::vector<float> m_planarR;

    // ---- 트랙 버스 (트랙 = MIDI 채널). 각 버스에 그 트랙의 신스 보이스 +
    // 오디오 클립을 모으고, 트랙별 이펙트 체인을 건 뒤 마스터로 합친다. ----
    static constexpr int kBuses = 16;
    std::array<std::vector<float>, kBuses> m_busL;
    std::array<std::vector<float>, kBuses> m_busR;
    void allocateWorkBuffers(); // 콜백용 버퍼 사전 할당 (Rule 3)
    void processTrackChains(unsigned frames); // 버스별 이펙트 체인 적용
    void processSendReturn(unsigned frames);  // 센드/리턴 (공용 리버브)
    void sumBusesToMaster(unsigned frames);

    // 트랙 이펙트 슬롯. 스트림이 멈춘 동안에만 벡터를 바꾼다(오디오 스레드는 읽기만).
    // builtin이 있으면 내장 이펙트 슬롯 (host는 안 쓴다), 없으면 VST 슬롯.
    struct TrackEffect {
        vst::Vst3Host host;
        std::unique_ptr<BuiltinFx> builtin;
        std::atomic<bool> enabled{true};
        std::atomic<int> sidechain{-1}; // 내장 컴프레서 전용: 키 버스 (-1=자기 입력)
        std::string path;
        int classIndex = 0;
    };
    std::array<std::vector<std::unique_ptr<TrackEffect>>, kBuses> m_trackFx;

    // 트랙별 악기 슬롯 (버스당 1개). 이펙트와 같은 규칙: 스트림 정지 중에만 교체.
    struct TrackInstrument {
        vst::Vst3Host host;
        std::string path;
        int classIndex = 0;
    };
    std::array<std::unique_ptr<TrackInstrument>, kBuses> m_trackInst;
    std::vector<float> m_scratchL, m_scratchR; // 악기 렌더용 스크래치 (사전 할당)
    // 오디오 스레드: 이 채널을 담당하는 트랙 악기 (없으면 null)
    vst::Vst3Host* trackInstAudioHost(uint8_t channel) {
        auto& ti = m_trackInst[channel & 0x0F];
        return (ti && ti->host.isLoaded()) ? &ti->host : nullptr;
    }
    // 플러그인 로드/해제를 위해 실행 중인 스트림을 잠시 멈췄다 정확히 되돌린다.
    struct StreamSuspend {
        bool wasWasapi = false;
        bool wasAsio = false;
        int asioDevice = 0;
        int asioChannelMode = 0;
    };

    StreamSuspend suspendStreams();
    void resumeStreams(const StreamSuspend& s);
    StreamSuspend m_offlineSuspend; // 오프라인 렌더 동안 멈춘 스트림 상태
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_mpeEnabled{false};
    std::atomic<bool> m_effectBypass{false}; // 전역 이펙트 실시간 on/off
    std::atomic<float> m_masterGain{1.0f};   // 최종 출력 마스터 볼륨
    std::atomic<float> m_masterPan{0.0f};    // 최종 출력 마스터 팬
    std::atomic<float> m_monitorGain{1.0f};  // 모니터 입력 볼륨(모니터 트랙 볼륨)
    std::atomic<double> m_latencySec{0.0}; // 스트림 출력 지연(초)

    // 오디오 클립 재생 (스냅샷은 shared_ptr 원자 교체, 위치는 아토믹)
    std::shared_ptr<const std::vector<AudioMixClip>> m_mixClips;
    std::atomic<int64_t> m_audioSample{0};
    std::atomic<bool> m_audioPlaying{false};
    void mixAudioClips(unsigned frames); // 콜백에서 planar 버퍼에 클립을 더한다
    std::atomic<int> m_activeVoices{0};
    std::atomic<std::size_t> m_dropped{0};

    // 채널 믹스 중복 전송 방지 (제어 스레드 전용 캐시). 매 프레임 큐를 채우지 않는다.
    float m_lastChGain[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float m_lastChPan[16] = {0};

    // VSTi 출력 버스 (-1 = 마스터 직행). 버스로 보내면 그 트랙의 볼륨/팬을
    // 아래 배열(오디오 스레드 전용, ChannelMix 이벤트로 갱신)로 곱해 준다.
    // (내장 신스는 자체적으로 채널 게인을 적용하므로 이 배열을 쓰지 않는다)
    std::atomic<int> m_instrumentBus{-1};
    float m_busGainL[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float m_busGainR[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

    // ---- 오디오 입력(캡처) ----
    std::unique_ptr<RtAudio> m_inAudio;      // 별도 입력 스트림
    unsigned m_inDeviceId = 0;               // 선택된 입력 장치 id(0=기본)
    std::vector<unsigned> m_inIds;           // listInputDevices() 결과 id(인덱스 대응)
    int m_inChannels = 1;                    // 캡처 채널 수(1 또는 2 -> 모노 다운믹스)
    int m_inMode = 0;                        // 0=1+2, 1=입력1, 2=입력2
    std::atomic<bool> m_inOpen{false};
    std::atomic<bool> m_monitor{false};      // 입력->출력 실시간 통과
    std::atomic<float> m_inLevel{0.0f};      // 최근 입력 피크(0~1)

    // 입력 장치 버퍼 지연 (연습 판정 보정용). 스트림 열 때 갱신.
    std::atomic<double> m_inLatencySec{0.0};

    // 연습 모드 입력 탭: 콜백이 최근 입력(모노)을 링에 쓰고 GUI가 피치 분석에 읽는다.
    // SPSC — 쓰기는 오디오 스레드, 읽기는 GUI. 순간 찢김은 분석용이라 무해.
    // 악보 주도 청취가 이벤트당 최대 ~0.9초 조각을 소급해 읽으므로 넉넉하게.
    static constexpr uint32_t kTapSize = 131072; // 2의 거듭제곱 (~2.7초 @48k)
    std::vector<float> m_inTap;
    std::atomic<uint32_t> m_inTapPos{0};
    inline void tapPush(float s) {
        if (m_inTap.empty()) return;
        const uint32_t p = m_inTapPos.load(std::memory_order_relaxed);
        m_inTap[p & (kTapSize - 1)] = s;
        m_inTapPos.store(p + 1, std::memory_order_release);
    }

    // 트랙 프리즈: 이 버스의 VST "악기" 처리를 건너뛴다 (GUI가 설정).
    // FX 체인은 계속 돌아 프리즈 전후 소리 경로가 동일하다.
    std::array<std::atomic<bool>, kBuses> m_busFrozen{};
    // 프리즈 베이크 중 FX 체인 생략 (GUI 스레드가 스트림 정지 상태에서만 토글)
    std::atomic<bool> m_offlineSkipChains{false};

    // 오디오 콜백 부하 (처리 시간/버퍼 시간, 지수 평활). 성능 창 표시용.
    std::atomic<float> m_audioLoad{0.0f};

    // 레벨 미터: 오디오 스레드가 max-hold로 쌓고 GUI가 exchange(0)로 걷어간다.
    // 버스는 포스트 FX·페이더, 마스터는 소프트 클립 직전(>1 = 클리핑).
    std::array<std::atomic<float>, kBuses> m_busPeak{};
    std::atomic<float> m_masterPeakL{0.0f};
    std::atomic<float> m_masterPeakR{0.0f};
    // mono 링: 입력 콜백(생산자) -> 출력 콜백(소비자, 모니터). 2^15 = 32768.
    core::SpscQueue<float, 32768> m_inRing;
    // 녹음 버퍼: 30초 청크의 원자 포인터 목록. GUI(pumpRecording)가 앞서서
    // 할당해 채워 두고, 오디오 스레드는 포인터를 읽어 쓰기만 한다(할당 없음).
    // 청크가 커서(30초) GUI가 프레임마다 돌면 고갈될 일이 없다. 상한 2시간.
    static constexpr std::size_t kRecChunkFrames = 48000u * 30u;
    static constexpr std::size_t kRecMaxChunks = 240;
    std::array<std::atomic<float*>, kRecMaxChunks> m_recChunks{};
    std::vector<std::unique_ptr<std::vector<float>>> m_recChunkStore; // 소유(제어 스레드)
    std::atomic<std::size_t> m_recCount{0};
    std::atomic<bool> m_recording{false};
    // 오디오 스레드: 샘플 하나를 현재 청크에 기록 (청크 없으면 조용히 버림)
    void recordSample(float s) {
        const std::size_t n = m_recCount.load(std::memory_order_relaxed);
        const std::size_t chunk = n / kRecChunkFrames;
        if (chunk >= kRecMaxChunks) return;
        float* buf = m_recChunks[chunk].load(std::memory_order_acquire);
        if (!buf) return;
        buf[n % kRecChunkFrames] = s;
        m_recCount.store(n + 1, std::memory_order_release);
    }
    void ensureRecChunk(std::size_t index); // 제어 스레드: index번 청크 할당 보장

    // ---- 클릭 소리 4종 (인덱스 = kClick* 상수) ----
    std::array<std::atomic<uint8_t>, kClickKinds> m_clickNote{
        std::atomic<uint8_t>{77}, std::atomic<uint8_t>{84}, std::atomic<uint8_t>{88},
        std::atomic<uint8_t>{91}};
    std::array<std::shared_ptr<const AudioClip>, kClickKinds> m_clickSample; // null=신스 음
    // 원샷 재생 슬롯 (오디오 스레드 전용, 클릭이 겹쳐도 잘리지 않게 4개)
    struct ClickVoice {
        std::shared_ptr<const AudioClip> clip;
        double pos = 0.0;   // 소스 프레임 위치
        double step = 1.0;  // 엔진 1프레임당 소스 프레임
        float gain = 1.0f;
    };
    std::array<ClickVoice, 4> m_clickVoices;
    void triggerClick(std::shared_ptr<const AudioClip> smp, float gain); // 오디오 스레드
    void mixClickVoices(unsigned frames);                                // 오디오 스레드
    // 사용자 샘플이 없을 때 쓰는 합성 "삑" (음 높이별로 미리 렌더, GUI 스레드에서 생성)
    std::array<std::shared_ptr<const AudioClip>, kClickKinds> m_clickBeep;
    std::array<std::atomic<int>, kClickKinds> m_clickBeepNote{
        std::atomic<int>{-1}, std::atomic<int>{-1}, std::atomic<int>{-1}, std::atomic<int>{-1}};
    void ensureClickBeep(int kind, uint8_t note); // GUI 스레드 (음이 바뀔 때만 재생성)

    // ---- 드럼 샘플 (노트별 WAV 원샷, 채널 10). null = 내장 드럼 신스 ----
    // 클릭과 달리 트랙 버스로 섞여 볼륨/팬/EQ/FX 체인이 걸린다.
    std::array<std::shared_ptr<const AudioClip>, 128> m_drumSample;
    struct DrumSampleVoice {
        std::shared_ptr<const AudioClip> clip;
        double pos = 0.0;
        double step = 1.0;
        float gain = 1.0f;
        int bus = 9;
    };
    std::array<DrumSampleVoice, 16> m_drumVoices; // 겹침 대비 (오디오 스레드 전용)
    void triggerDrumSample(int bus, std::shared_ptr<const AudioClip> smp, float gain);
    void mixDrumVoices(unsigned frames); // 버스에 누적 (체인 전)

    // ---- 마스터 캡처 (WAV 내보내기). 입력 녹음과 같은 사전 할당 패턴. ----
    std::vector<float> m_capBuf; // 인터리브 스테레오 (L R L R ...)
    std::size_t m_capCap = 0;    // float 개수 (프레임 x 2)
    std::atomic<std::size_t> m_capCount{0};
    std::atomic<bool> m_capturing{false};
    // 최종 출력 한 프레임을 캡처 버퍼에 쌓는다 (두 콜백이 공용, 할당 없음).
    void captureMasterFrame(float l, float r) {
        if (!m_capturing.load(std::memory_order_relaxed)) return;
        const std::size_t n = m_capCount.load(std::memory_order_relaxed);
        if (n + 1 >= m_capCap) return;
        m_capBuf[n] = l;
        m_capBuf[n + 1] = r;
        m_capCount.store(n + 2, std::memory_order_release);
    }

    // ---- ASIO (저지연 듀플렉스) ----
    std::unique_ptr<RtAudio> m_asio;         // ASIO 전용 인스턴스(없으면 미지원)
    std::vector<unsigned> m_asioIds;         // listAsioDevices() 결과 id
    std::atomic<bool> m_asioOn{false};
    int m_asioInChannels = 2;
    int m_asioDeviceIndex = 0; // 마지막으로 연 ASIO 장치(플러그인 로드 후 복구용)
    // restoreOutput=true면 ASIO가 담당하던 출력을 WASAPI 스트림으로 되돌린다.
    // (플러그인 로드용 일시정지에서는 false — 샘플레이트가 바뀌면 안 되므로)
    void stopAsioInternal(bool restoreOutput);
};

} // namespace midipro::audio
