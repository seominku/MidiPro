// =============================================================
// MidiPro - audio/RtAudioEngine.cpp
// =============================================================

#include "audio/RtAudioEngine.h"

#include "midi/MidiConstants.h"
#include "midi2/Ump.h"

#include "RtAudio.h"

#include <excpt.h> // __except 상수 (ASIO 드라이버 크래시 방어)
#include <objbase.h> // CoInitializeEx (ASIO 드라이버 로드에 COM STA 필요)
#include <algorithm>
#include <chrono> // 콜백 부하 측정 (성능 창)
#include <iostream>

namespace midipro::audio {

namespace {

// ASIO 드라이버는 CoCreateInstance로 로드되므로 호출 스레드가 COM STA여야
// 한다. 초기화 안 돼 있으면 로드가 조용히 실패해 장치 목록이 비어버린다.
// 이미 초기화돼 있으면(S_FALSE/RPC_E_CHANGED_MODE) 그대로 둔다.
void ensureComSTA() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }

// ASIO 드라이버 로드/초기화는 서드파티 드라이버 코드라 잘못된 드라이버가
// 설치돼 있으면 액세스 위반으로 앱을 통째로 죽일 수 있다. SEH로 감싸
// 실패로 처리하고 앱은 계속 살아 있게 한다. (아래 함수들엔 소멸자 있는
// 지역 객체를 두지 않아 /EHsc에서도 __try 사용 가능)
void asioProbeInner(RtAudio* asio, std::vector<unsigned>& ids,
                    std::vector<std::string>& names) {
    for (unsigned id : asio->getDeviceIds()) {
        RtAudio::DeviceInfo info = asio->getDeviceInfo(id);
        ids.push_back(id);
        names.push_back(info.name);
    }
}
bool asioProbeGuarded(RtAudio* asio, std::vector<unsigned>& ids,
                      std::vector<std::string>& names) {
    __try {
        asioProbeInner(asio, ids, names);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool asioOpenGuarded(RtAudio* asio, RtAudio::StreamParameters* op, RtAudio::StreamParameters* ip,
                     unsigned sr, unsigned* frames, RtAudioCallback cb, void* ud,
                     RtAudio::StreamOptions* opt, RtAudioErrorType* err) {
    __try {
        *err = asio->openStream(op, ip, RTAUDIO_FLOAT32, sr, frames, cb, ud, opt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

namespace {
constexpr uint8_t kCcAllNotesOff = 123;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcTimbre = 74; // MPE 표준 음색(밝기) 컨트롤
} // namespace

RtAudioEngine::RtAudioEngine() {
    // WASAPI로 명시 생성한다. 기본 생성자는 컴파일된 API를 순회하며 장치를
    // 탐색하는데, ASIO가 먼저 시도되면 시작 시 ASIO 드라이버를 로드해 크래시할
    // 수 있다. ASIO 인스턴스는 사용자가 실제로 쓸 때만 지연 생성한다(ensureAsio).
    try {
        m_audio = std::make_unique<RtAudio>(RtAudio::WINDOWS_WASAPI);
        m_deviceId = m_audio->getDefaultOutputDevice(); // 처음엔 시스템 기본 출력
        m_inAudio = std::make_unique<RtAudio>(RtAudio::WINDOWS_WASAPI); // 입력(캡처)용
    } catch (...) {
        std::cerr << "[RtAudioEngine] RtAudio 초기화 실패\n";
    }
    m_synth.prepare(m_sampleRate);
}

RtAudioEngine::~RtAudioEngine() {
    if (m_asio) {
        if (m_asio->isStreamRunning()) m_asio->stopStream();
        if (m_asio->isStreamOpen()) m_asio->closeStream();
    }
    stopInput();
    closePort();
}

std::vector<std::string> RtAudioEngine::listPorts() {
    // 신스는 논리적 출력 하나로 노출한다 (하드웨어 포트와 동일 취급).
    return {"내장 신디사이저 (RtAudio)"};
}

std::vector<std::string> RtAudioEngine::listOutputDevices() {
    // 출력 채널이 있는 오디오 장치만 나열한다 (마이크/입력 전용 제외).
    std::vector<std::string> names;
    m_outIds.clear();
    if (!m_audio) return names;
    for (unsigned id : m_audio->getDeviceIds()) {
        RtAudio::DeviceInfo info = m_audio->getDeviceInfo(id);
        if (info.outputChannels == 0) continue;
        m_outIds.push_back(id);
        std::string label = info.name;
        if (info.isDefaultOutput) label += "  (시스템 기본)";
        names.push_back(label);
    }
    return names;
}

int RtAudioEngine::outputDevice() const {
    for (int i = 0; i < (int)m_outIds.size(); ++i)
        if (m_outIds[i] == m_deviceId) return i;
    return -1;
}

void RtAudioEngine::setOutputDevice(int index) {
    if (index < 0 || index >= (int)m_outIds.size()) return;
    const unsigned newId = m_outIds[index];
    if (newId == m_deviceId) return;
    const bool wasOpen = m_open.load(std::memory_order_acquire);
    if (wasOpen) closePort();
    m_deviceId = newId;
    if (wasOpen) openPort(0); // 새 장치로 스트림 재시작
}

bool RtAudioEngine::openPort(unsigned /*index*/) {
    if (!m_audio) return false;
    // ASIO가 출력을 담당 중이면 WASAPI를 겹쳐 열지 않는다 (이중 콜백 방지).
    if (m_asioOn.load(std::memory_order_acquire)) return true;
    if (m_open.load(std::memory_order_acquire)) return true;
    if (m_audio->getDeviceCount() < 1) {
        std::cerr << "[RtAudioEngine] 오디오 출력 장치가 없습니다\n";
        return false;
    }

    // 선택된 장치가 유효하지 않으면 시스템 기본으로 되돌린다.
    unsigned deviceId = m_deviceId;
    if (deviceId == 0) deviceId = m_audio->getDefaultOutputDevice();
    RtAudio::DeviceInfo info = m_audio->getDeviceInfo(deviceId);
    if (info.outputChannels == 0) {
        deviceId = m_audio->getDefaultOutputDevice();
        info = m_audio->getDeviceInfo(deviceId);
    }

    RtAudio::StreamParameters params;
    params.deviceId = deviceId;
    params.nChannels = 2; // 스테레오 (mono 렌더를 L/R 복제)
    params.firstChannel = 0;

    // 장치가 선호하는 샘플레이트를 쓴다. WASAPI 공유 모드에서 장치
    // 믹스 포맷과 어긋나면 스트림이 안 열리거나 리샘플링이 끼는 것을 피한다.
    // (예: 포커스라이트 44100, 다수 온보드 48000)
    if (info.preferredSampleRate > 0) m_sampleRate = (double)info.preferredSampleRate;

    unsigned frames = m_bufferFrames;

    // 콜백에서 쓸 버퍼를 미리 크게 잡아둔다 (재할당 방지, Rule 3).
    allocateWorkBuffers();

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;

    RtAudioErrorType err =
        m_audio->openStream(&params, nullptr, RTAUDIO_FLOAT32, (unsigned)m_sampleRate, &frames,
                            &RtAudioEngine::rtCallback, this, &options);
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] openStream 실패: " << m_audio->getErrorText() << "\n";
        return false;
    }

    m_bufferFrames = frames;
    m_synth.prepare(m_sampleRate);

    err = m_audio->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] startStream 실패: " << m_audio->getErrorText() << "\n";
        m_audio->closeStream();
        return false;
    }

    // 출력 지연(초) = 한 버퍼 + 장치 보고 지연. 화면 재생 헤드 보정에 쓴다.
    const long reported = (long)m_audio->getStreamLatency();
    const double latFrames = (double)m_bufferFrames + (reported > 0 ? (double)reported : 0.0);
    m_latencySec.store(m_sampleRate > 0 ? latFrames / m_sampleRate : 0.0,
                       std::memory_order_relaxed);

    m_open.store(true, std::memory_order_release);
    return true;
}

void RtAudioEngine::closePort() {
    if (!m_audio) return;
    if (m_open.load(std::memory_order_acquire)) {
        if (m_audio->isStreamRunning()) m_audio->stopStream();
        if (m_audio->isStreamOpen()) m_audio->closeStream();
        m_open.store(false, std::memory_order_release);
        m_latencySec.store(0.0, std::memory_order_relaxed);
    }
}

bool RtAudioEngine::isOpen() const {
    // ASIO 듀플렉스가 돌고 있으면 그 스트림이 출력을 담당 중이므로 "열려 있음".
    // 이걸 빼면 재생 시작 시 GUI가 WASAPI를 추가로 열어 두 오디오 콜백이
    // 같은 렌더 버퍼를 동시에 써서 심한 지지직 잡음이 난다.
    return m_open.load(std::memory_order_acquire) || m_asioOn.load(std::memory_order_acquire);
}

bool RtAudioEngine::pushEvent(const EngineEvent& e) {
    // 제어 스레드끼리는 뮤텍스로 직렬화한다. 오디오 스레드(소비자)는
    // 이 뮤텍스를 절대 잡지 않으므로 실시간성은 유지된다.
    std::lock_guard<std::mutex> lock(m_producerMutex);
    if (!m_queue.push(e)) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool RtAudioEngine::send(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return false;
    const uint8_t status = bytes[0];
    const uint8_t kind = status & midi::kStatusMask;
    const uint8_t channel = status & midi::kChannelMask;

    EngineEvent e;
    e.channel = channel;
    if (kind == midi::kStatusNoteOn && bytes.size() >= 3) {
        if (bytes[2] == 0) {
            e.type = EngineEvent::Type::NoteOff;
            e.note = bytes[1];
        } else {
            e.type = EngineEvent::Type::NoteOn;
            e.note = bytes[1];
            e.velocity = bytes[2];
            e.value = (float)bytes[2] / 127.0f;
        }
        return pushEvent(e);
    }
    if (kind == midi::kStatusNoteOff && bytes.size() >= 2) {
        e.type = EngineEvent::Type::NoteOff;
        e.note = bytes[1];
        return pushEvent(e);
    }
    if (kind == midi::kStatusPitchBend && bytes.size() >= 3) {
        // 14비트 벤드(LSB, MSB) -> -1~1 정규화 (8192=중앙)
        const int raw = ((int)bytes[2] << 7 | bytes[1]) - midi::kPitchBendCenter;
        e.type = EngineEvent::Type::PitchBend;
        e.value = (float)raw / 8192.0f;
        return pushEvent(e);
    }
    if (kind == midi::kStatusChannelAftertouch && bytes.size() >= 2) {
        e.type = EngineEvent::Type::Pressure;
        e.value = (float)bytes[1] / 127.0f;
        return pushEvent(e);
    }
    if (kind == midi::kStatusControlChange && bytes.size() >= 3) {
        if (bytes[1] == kCcAllSoundOff) { // 120: 즉시 무음
            e.type = EngineEvent::Type::AllSoundOff;
            return pushEvent(e);
        }
        if (bytes[1] == kCcAllNotesOff) { // 123: 릴리스
            e.type = EngineEvent::Type::AllNotesOff;
            return pushEvent(e);
        }
        if (bytes[1] == kCcTimbre) { // CC74 = MPE 음색(밝기)
            e.type = EngineEvent::Type::Timbre;
            e.value = (float)bytes[2] / 127.0f;
            return pushEvent(e);
        }
        // 나머지 CC(모듈레이션/서스테인/익스프레션 등)는 VST 악기에 전달한다
        e.type = EngineEvent::Type::ControlChange;
        e.note = bytes[1]; // CC 번호
        e.value = (float)bytes[2] / 127.0f;
        return pushEvent(e);
    }
    // Program Change 등은 현재 신스에서 음색 고정이라 무시한다.
    return true;
}

bool RtAudioEngine::sendUmp(const uint32_t* words, int count) {
    if (!words || count < 1) return false;
    // MIDI 2.0 채널 보이스는 2워드
    if (count < 2 || midi2::umpWordCount(words[0]) != 2) return true; // 다른 UMP는 무시
    const midi2::Cv2Message m = midi2::parseMidi2Cv(words[0], words[1]);

    EngineEvent e;
    e.channel = m.channel;
    e.note = m.note;
    switch (m.type) {
    case midi2::Cv2Type::NoteOn: {
        const float vel = midi2::vel16ToFloat(m.velocity16);
        if (vel <= 0.0f) {
            e.type = EngineEvent::Type::NoteOff;
        } else {
            e.type = EngineEvent::Type::NoteOn;
            e.value = vel;                                 // 고해상도 벨로시티(0~1)
            e.velocity = (uint8_t)(vel * 127.0f + 0.5f);   // VST용 7비트
        }
        return pushEvent(e);
    }
    case midi2::Cv2Type::NoteOff:
        e.type = EngineEvent::Type::NoteOff;
        return pushEvent(e);
    case midi2::Cv2Type::PerNotePitchBend:
        e.type = EngineEvent::Type::PerNotePitchBend;
        e.value = midi2::pitch32ToNorm(m.data32); // -1~1
        return pushEvent(e);
    case midi2::Cv2Type::ChannelPitchBend:
        e.type = EngineEvent::Type::PitchBend;
        e.value = midi2::pitch32ToNorm(m.data32);
        return pushEvent(e);
    case midi2::Cv2Type::ChannelPressure:
        e.type = EngineEvent::Type::Pressure;
        e.value = midi2::cc32ToFloat(m.data32);
        return pushEvent(e);
    case midi2::Cv2Type::ControlChange:
        if (m.index == kCcAllSoundOff) {
            e.type = EngineEvent::Type::AllSoundOff;
            return pushEvent(e);
        }
        if (m.index == kCcAllNotesOff) {
            e.type = EngineEvent::Type::AllNotesOff;
            return pushEvent(e);
        }
        if (m.index == kCcTimbre) {
            e.type = EngineEvent::Type::Timbre;
            e.value = midi2::cc32ToFloat(m.data32);
            return pushEvent(e);
        }
        e.type = EngineEvent::Type::ControlChange;
        e.note = (uint8_t)m.index;
        e.value = midi2::cc32ToFloat(m.data32);
        return pushEvent(e);
    default:
        return true;
    }
}

// 콜백에서 쓰는 모든 작업 버퍼를 넉넉히 확보한다 (오디오 스레드에서 할당 금지).
void RtAudioEngine::allocateWorkBuffers() {
    constexpr std::size_t kMaxFrames = 4096;
    m_monoBuffer.assign(kMaxFrames, 0.0f);
    m_planarL.assign(kMaxFrames, 0.0f);
    m_planarR.assign(kMaxFrames, 0.0f);
    m_scratchL.assign(kMaxFrames, 0.0f);
    m_scratchR.assign(kMaxFrames, 0.0f);
    for (int b = 0; b < kBuses; ++b) {
        m_busL[b].assign(kMaxFrames, 0.0f);
        m_busR[b].assign(kMaxFrames, 0.0f);
    }
    m_retL.assign(kMaxFrames, 0.0f); // 센드/리턴 버스
    m_retR.assign(kMaxFrames, 0.0f);
    m_monBuf.assign(kMaxFrames, 0.0f); // ASIO 모니터 입력
    m_inTap.assign(kTapSize, 0.0f);    // 연습 모드 입력 탭
    m_returnReverb.setParam(2, 1.0f); // 리턴은 웻 전용 (드라이는 원래 버스로 나간다)
}

void RtAudioEngine::setChannelMix(int channel, float gain, float pan) {
    if (channel < 0 || channel > 15) return;
    // 값이 바뀔 때만 큐에 넣는다 (매 프레임 16채널 flooding 방지)
    if (m_lastChGain[channel] == gain && m_lastChPan[channel] == pan) return;
    m_lastChGain[channel] = gain;
    m_lastChPan[channel] = pan;

    EngineEvent e;
    e.type = EngineEvent::Type::ChannelMix;
    e.channel = (uint8_t)channel;
    e.value = gain;
    e.value2 = pan;
    pushEvent(e);
}

void RtAudioEngine::setParams(const SynthParams& params) {
    EngineEvent e;
    e.type = EngineEvent::Type::SetParams;
    e.params = params;
    pushEvent(e);
}

int RtAudioEngine::rtCallback(void* output, void* /*input*/, unsigned frames,
                              double /*streamTime*/, unsigned /*status*/, void* userData) {
    auto* self = static_cast<RtAudioEngine*>(userData);
    self->processCallback(static_cast<float*>(output), frames);
    return 0;
}

// 신스/VST/클립을 렌더해 m_planarL/R을 채운다 (입력 모니터/인터리브는 호출자가).
// WASAPI 출력 콜백과 ASIO 듀플렉스 콜백이 공용으로 쓴다.
void RtAudioEngine::renderBlock(unsigned frames) {
    const bool useInstrument = m_instrument.isLoaded();

    // MPE 모드에 따라 피치벤드 범위 갱신 (멤버 채널 기본 ±48, 일반 ±2)
    m_synth.setPitchBendRange(m_mpeEnabled.load(std::memory_order_relaxed) ? 48.0f : 2.0f);

    // 1) 큐 명령을 순서대로 적용 (락프리 pop). 노트/표현은 채널과 함께
    //    악기(VST 또는 내장 신스)로, 파라미터는 내장 신스로 전달.
    // 채널별 라우팅: 그 채널의 트랙 악기 > 전역 VSTi > 내장 신스 순.
    EngineEvent e;
    while (m_queue.pop(e)) {
        vst::Vst3Host* th = trackInstAudioHost(e.channel);
        switch (e.type) {
        case EngineEvent::Type::NoteOn:
            // 메트로놈/카운트인/강조 클릭(채널 10, 지정 노트)에 샘플이 설정돼
            // 있으면 신스 대신 원샷 샘플을 재생한다. 강조를 먼저 매칭한다.
            if (e.channel == 9) {
                std::shared_ptr<const AudioClip> smp;
                static constexpr int kOrder[kClickKinds] = {kClickCountInAccent, kClickAccent,
                                                            kClickCountIn, kClickMetro};
                for (int kind : kOrder) {
                    if (e.note != m_clickNote[(std::size_t)kind].load(std::memory_order_relaxed))
                        continue;
                    smp = std::atomic_load(&m_clickSample[(std::size_t)kind]);
                    // 사용자 샘플이 없으면 합성 "삑" — 어느 쪽이든 마스터 직행이라
                    // 드럼 트랙(채널 10)의 뮤트/볼륨/FX에 클릭이 묻히지 않는다
                    if (!smp) smp = std::atomic_load(&m_clickBeep[(std::size_t)kind]);
                    break;
                }
                if (smp && smp->sampleRate > 0) {
                    triggerClick(std::move(smp), e.value); // e.value = 벨로시티 0~1
                    break;
                }
                // 드럼 샘플이 배정된 노트면 WAV 원샷으로 (트랙 버스를 탄다)
                if (auto ds = std::atomic_load(&m_drumSample[e.note & 0x7F]);
                    ds && ds->sampleRate > 0) {
                    triggerDrumSample(e.channel & 0x0F, std::move(ds), e.value);
                    break;
                }
            }
            if (th) th->addNoteOn(e.channel, e.note, e.velocity);
            else if (useInstrument) m_instrument.addNoteOn(e.channel, e.note, e.velocity);
            else m_synth.noteOnFloat(e.channel, e.note, e.value); // 고해상도 벨로시티(0~1)
            break;
        case EngineEvent::Type::NoteOff:
            if (th) th->addNoteOff(e.channel, e.note);
            else if (useInstrument) m_instrument.addNoteOff(e.channel, e.note);
            else m_synth.noteOff(e.channel, e.note);
            break;
        case EngineEvent::Type::PitchBend:
            if (th) th->addPitchBend(e.channel, e.value);
            else if (useInstrument) m_instrument.addPitchBend(e.channel, e.value);
            else m_synth.setPitchBend(e.channel, e.value);
            break;
        case EngineEvent::Type::PerNotePitchBend:
            // MIDI 2.0 노트별 벤딩은 내장 신스에서 (채널,노트) 보이스에 적용.
            // (VST 노트별 벤딩은 note-expression이 필요 -> 추후)
            if (!useInstrument && !th) m_synth.setPerNotePitchBend(e.channel, e.note, e.value);
            break;
        case EngineEvent::Type::Pressure:
            if (th) th->addPressure(e.channel, e.value);
            else if (useInstrument) m_instrument.addPressure(e.channel, e.value);
            else m_synth.setPressure(e.channel, e.value);
            break;
        case EngineEvent::Type::Timbre:
            if (th) th->addTimbre(e.channel, e.value);
            else if (useInstrument) m_instrument.addTimbre(e.channel, e.value);
            else m_synth.setTimbre(e.channel, e.value);
            break;
        case EngineEvent::Type::ControlChange:
            // 일반 CC는 VST 악기의 IMidiMapping 파라미터로 전달한다.
            // (내장 신스는 CC74 외의 CC를 지원하지 않아 조용히 무시)
            if (th) th->addControlChange(e.channel, e.note, e.value);
            else if (useInstrument) m_instrument.addControlChange(e.channel, e.note, e.value);
            break;
        case EngineEvent::Type::AllNotesOff:
            m_synth.allNotesOff();
            // VSTi(전역/트랙 악기)도 눌린 노트를 전부 놓게 한다 (스턱 노트 방지)
            if (useInstrument) m_instrument.addAllNotesOff();
            for (int b = 0; b < kBuses; ++b)
                if (auto* h = trackInstAudioHost((uint8_t)b)) h->addAllNotesOff();
            break;
        case EngineEvent::Type::AllSoundOff:
            m_synth.allSoundOff(); // 즉시 무음 (정지/시크 잔음 제거)
            // VST에는 표준 "즉시 무음"이 없어 note-off로 릴리스시킨다
            if (useInstrument) m_instrument.addAllNotesOff();
            for (int b = 0; b < kBuses; ++b)
                if (auto* h = trackInstAudioHost((uint8_t)b)) h->addAllNotesOff();
            break;
        case EngineEvent::Type::SetParams:
            m_synth.setParams(e.params);
            break;
        case EngineEvent::Type::ChannelMix: {
            // 트랙(채널) 볼륨/팬. 내장 신스는 내부에서, VSTi 버스는 아래 배열로 적용.
            m_synth.setChannelMix(e.channel, e.value, e.value2);
            const int ch = e.channel & 0x0F;
            m_busGainL[ch] = e.value * (e.value2 <= 0.0f ? 1.0f : 1.0f - e.value2);
            m_busGainR[ch] = e.value * (e.value2 >= 0.0f ? 1.0f : 1.0f + e.value2);
            break;
        }
        }
    }

    // 2) 마스터/버스 버퍼를 비운다
    std::fill(m_planarL.begin(), m_planarL.begin() + frames, 0.0f);
    std::fill(m_planarR.begin(), m_planarR.begin() + frames, 0.0f);
    for (int b = 0; b < kBuses; ++b) {
        std::fill(m_busL[b].begin(), m_busL[b].begin() + frames, 0.0f);
        std::fill(m_busR[b].begin(), m_busR[b].begin() + frames, 0.0f);
    }

    // 3) 트랙별 악기: 각자 스크래치에 렌더한 뒤 자기 버스에 트랙 볼륨/팬을 곱해
    //    더한다 (그 버스의 이펙트 체인을 그대로 탄다).
    float* planar[2] = {m_planarL.data(), m_planarR.data()};
    float* scratch[2] = {m_scratchL.data(), m_scratchR.data()};
    for (int b = 0; b < kBuses; ++b) {
        auto& ti = m_trackInst[b];
        if (!ti || !ti->host.isLoaded()) continue;
        if (m_busFrozen[b].load(std::memory_order_relaxed)) continue; // 프리즈: CPU 절약
        std::fill(m_scratchL.begin(), m_scratchL.begin() + frames, 0.0f);
        std::fill(m_scratchR.begin(), m_scratchR.begin() + frames, 0.0f);
        ti->host.process(scratch, 2, (int)frames);
        const float gl = m_busGainL[b], gr = m_busGainR[b];
        for (unsigned i = 0; i < frames; ++i) {
            m_busL[b][i] += m_scratchL[i] * gl;
            m_busR[b][i] += m_scratchR[i] * gr;
        }
    }

    // 4) 전역 악기 또는 내장 신스. 전역 VSTi는 지정 버스(트랙 볼륨/팬 적용) 또는
    //    마스터 직행. 내장 신스는 채널별 버스로 렌더한다 (트랙 악기가 있는 채널은
    //    이벤트가 그쪽으로 라우팅돼 보이스가 없다).
    if (useInstrument) {
        const int ib = m_instrumentBus.load(std::memory_order_relaxed);
        if (ib >= 0 && ib < kBuses) {
            std::fill(m_scratchL.begin(), m_scratchL.begin() + frames, 0.0f);
            std::fill(m_scratchR.begin(), m_scratchR.begin() + frames, 0.0f);
            m_instrument.process(scratch, 2, (int)frames);
            const float gl = m_busGainL[ib], gr = m_busGainR[ib];
            for (unsigned i = 0; i < frames; ++i) {
                m_busL[ib][i] += m_scratchL[i] * gl;
                m_busR[ib][i] += m_scratchR[i] * gr;
            }
        } else {
            m_instrument.process(planar, 2, (int)frames);
        }
    } else {
        float* bl[kBuses];
        float* br[kBuses];
        for (int b = 0; b < kBuses; ++b) {
            bl[b] = m_busL[b].data();
            br[b] = m_busR[b].data();
        }
        m_synth.renderBuses(bl, br, (int)frames);
        m_activeVoices.store(m_synth.activeVoiceCount(), std::memory_order_relaxed);
    }

    // 4) 임포트/녹음한 오디오 클립을 각 트랙 버스에 더한다
    mixAudioClips(frames);

    // 5) 버스별 트랙 이펙트 체인 -> 마스터로 합산
    mixDrumVoices(frames); // 드럼 샘플 -> 버스 (체인 전이라 EQ/FX가 걸린다)
    // ASIO 모니터 입력 -> 담당 트랙 버스 (체인 앞): 모니터에 그 트랙 FX가 걸린다.
    // (asioProcess가 m_monBuf/m_monCount를 채워두면 여기서 소비한다)
    if (m_monCount > 0) {
        const int mb = m_monitorBus.load(std::memory_order_relaxed);
        if (mb >= 0 && mb < kBuses) {
            const float mg = m_monitorGain.load(std::memory_order_relaxed);
            const unsigned n = frames < m_monCount ? frames : m_monCount;
            float* bl = m_busL[mb].data();
            float* br = m_busR[mb].data();
            for (unsigned i = 0; i < n; ++i) {
                bl[i] += m_monBuf[i] * mg;
                br[i] += m_monBuf[i] * mg;
            }
        }
        m_monCount = 0;
    }
    processTrackChains(frames);
    processSendReturn(frames); // 센드/리턴 (공용 리버브) — 체인 뒤, 마스터 합산 전
    sumBusesToMaster(frames);

    // 5-2) 메트로놈/카운트인 클릭 샘플 (트랙 체인을 타지 않고 마스터로 직행)
    mixClickVoices(frames);

    // 6) 마스터 이펙트 VST (바이패스면 건너뜀)
    if (m_effect.isLoaded() && !m_effectBypass.load(std::memory_order_relaxed)) {
        m_effect.process(planar, 2, (int)frames, planar);
    }
}

// 버스마다 트랙 이펙트 체인을 순서대로 제자리 처리한다 (오디오 스레드).
// 체인 벡터는 스트림이 멈춘 동안에만 바뀌므로 여기선 읽기만 한다.
void RtAudioEngine::processTrackChains(unsigned frames) {
    // 프리즈 베이크 중: 악기 출력만(FX 전 단계) 뽑아야 하므로 체인을 쉰다.
    // 구운 클립은 재생 때 체인을 "한 번" 타서 프리즈 전과 소리가 같아진다.
    if (m_offlineSkipChains.load(std::memory_order_relaxed)) return;
    for (int b = 0; b < kBuses; ++b) {
        auto& chain = m_trackFx[b];
        if (chain.empty()) continue;
        float* bp[2] = {m_busL[b].data(), m_busR[b].data()};
        for (auto& fx : chain) {
            if (!fx) continue;
            if (!fx->enabled.load(std::memory_order_relaxed)) continue; // 바이패스
            if (fx->builtin) {
                // 내장 컴프레서 + 사이드체인: 키 버스 신호로 게인을 계산한다.
                // (키 버스 번호가 이 버스보다 크면 그 버스의 체인 "앞" 신호가 키다)
                const int sc = fx->sidechain.load(std::memory_order_relaxed);
                if (fx->builtin->type() == BuiltinFx::kCompressor && sc >= 0 &&
                    sc < kBuses && sc != b)
                    fx->builtin->processSidechain(bp, m_busL[sc].data(), m_busR[sc].data(),
                                                  (int)frames, m_sampleRate);
                else
                    fx->builtin->process(bp, (int)frames, m_sampleRate);
                continue;
            }
            if (!fx->host.isLoaded()) continue;
            fx->host.process(bp, 2, (int)frames, bp);
        }
    }
}

// 센드/리턴: 각 버스 출력(포스트 체인)을 센드 양만큼 리턴 버퍼로 모아
// 공용 리버브를 걸고 마스터(planar)에 더한다. 리버브 꼬리가 남을 수 있어
// 센드가 모두 0이어도 매 블록 처리한다 (프리즈 베이크 중에는 쉰다).
void RtAudioEngine::processSendReturn(unsigned frames) {
    if (m_offlineSkipChains.load(std::memory_order_relaxed)) return;
    if (m_retL.size() < frames) return; // 버퍼 미할당 (initialize 전)
    std::fill(m_retL.begin(), m_retL.begin() + frames, 0.0f);
    std::fill(m_retR.begin(), m_retR.begin() + frames, 0.0f);
    for (int b = 0; b < kBuses; ++b) {
        const float s = m_busSend[b].load(std::memory_order_relaxed);
        if (s <= 0.001f) continue;
        const float* bl = m_busL[b].data();
        const float* br = m_busR[b].data();
        for (unsigned i = 0; i < frames; ++i) {
            m_retL[i] += bl[i] * s;
            m_retR[i] += br[i] * s;
        }
    }
    float* rp[2] = {m_retL.data(), m_retR.data()};
    m_returnReverb.process(rp, (int)frames, m_sampleRate); // 믹스=1 (웻 전용)
    const float lvl = m_returnLevel.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < frames; ++i) {
        m_planarL[i] += m_retL[i] * lvl;
        m_planarR[i] += m_retR[i] * lvl;
    }
}

// 원자 max-hold (오디오 스레드, 락프리). GUI가 exchange(0)로 걷어갈 때까지
// 최대치를 유지하므로 프레임 사이의 순간 피크를 놓치지 않는다.
static void atomicStoreMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void RtAudioEngine::sumBusesToMaster(unsigned frames) {
    for (int b = 0; b < kBuses; ++b) {
        const float* bl = m_busL[b].data();
        const float* br = m_busR[b].data();
        float peak = 0.0f; // 버스 레벨 미터용 (포스트 FX·페이더)
        for (unsigned i = 0; i < frames; ++i) {
            m_planarL[i] += bl[i];
            m_planarR[i] += br[i];
            const float al = bl[i] < 0 ? -bl[i] : bl[i];
            const float ar = br[i] < 0 ? -br[i] : br[i];
            const float m = al > ar ? al : ar;
            if (m > peak) peak = m;
        }
        if (peak > 0.0f) atomicStoreMax(m_busPeak[b], peak);
    }
}

void RtAudioEngine::processCallback(float* output, unsigned frames) {
    const auto perfT0 = std::chrono::steady_clock::now(); // 부하 측정 시작
    if (frames > m_monoBuffer.size()) frames = (unsigned)m_monoBuffer.size();
    renderBlock(frames);

    // 입력 모니터링(WASAPI): 캡처 링에서 꺼내 출력에 더한다(실시간 듣기).
    const float mg = m_monitorGain.load(std::memory_order_relaxed);
    if (m_monitor.load(std::memory_order_relaxed)) {
        // 백로그가 아주 크게 쌓였을 때만(약 100ms 이상) 최신 쪽으로 건너뛴다.
        // 낮은 임계치로 자주 버리면 소리가 뚝뚝 끊겨(뭉개져) 들리므로 완화한다.
        const std::size_t kHiWater = (std::size_t)(m_sampleRate * 0.1); // ~100ms
        if (m_inRing.size() > kHiWater) {
            const std::size_t keep = (std::size_t)frames * 2;
            while (m_inRing.size() > keep) {
                float d;
                if (!m_inRing.pop(d)) break;
            }
        }
        for (unsigned i = 0; i < frames; ++i) {
            float s;
            if (!m_inRing.pop(s)) break; // 링이 비면 남은 프레임은 무음
            m_planarL[i] += s * mg;
            m_planarR[i] += s * mg;
        }
    }

    // 마스터 볼륨/팬 적용 후 스테레오 인터리브 출력 (소프트 클립으로 과다 합산 방지)
    const float master = m_masterGain.load(std::memory_order_relaxed);
    const float mpan = m_masterPan.load(std::memory_order_relaxed);
    const float mgl = master * (mpan <= 0.0f ? 1.0f : 1.0f - mpan);
    const float mgr = master * (mpan >= 0.0f ? 1.0f : 1.0f + mpan);
    // 볼륨/팬을 planar에 먼저 적용하고, 리미터를 최종 단계로 건다 (내보내기 포함)
    for (unsigned i = 0; i < frames; ++i) {
        m_planarL[i] *= mgl;
        m_planarR[i] *= mgr;
    }
    if (m_limiterOn.load(std::memory_order_relaxed)) {
        float* mp[2] = {m_planarL.data(), m_planarR.data()};
        m_masterLimiter.process(mp, (int)frames, m_sampleRate);
    }
    float pkL = 0.0f, pkR = 0.0f; // 클램프 "직전" 피크 (>1 = 클리핑)
    for (unsigned i = 0; i < frames; ++i) {
        float l = m_planarL[i], r = m_planarR[i];
        const float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
        l = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        r = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        output[i * 2 + 0] = l;
        output[i * 2 + 1] = r;
        captureMasterFrame(l, r); // WAV 내보내기: 들리는 그대로 캡처
    }
    atomicStoreMax(m_masterPeakL, pkL);
    atomicStoreMax(m_masterPeakR, pkR);

    // 부하 = 처리 시간 / 버퍼가 벌어주는 시간 (지수 평활로 떨림 완화)
    if (m_sampleRate > 0.0 && frames > 0) {
        const double used =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - perfT0).count();
        const float inst = (float)(used / ((double)frames / m_sampleRate));
        m_audioLoad.store(m_audioLoad.load(std::memory_order_relaxed) * 0.9f + inst * 0.1f,
                          std::memory_order_relaxed);
    }
}

// ---- ASIO 듀플렉스 콜백: 한 콜백에서 입력을 바로 출력에 섞어 저지연 모니터 ----
int RtAudioEngine::asioCallback(void* output, void* input, unsigned frames,
                                double /*streamTime*/, unsigned /*status*/, void* userData) {
    static_cast<RtAudioEngine*>(userData)->asioProcess(static_cast<float*>(output),
                                                       static_cast<const float*>(input), frames);
    return 0;
}

void RtAudioEngine::asioProcess(float* output, const float* input, unsigned frames) {
    const auto perfT0 = std::chrono::steady_clock::now(); // 부하 측정 시작
    if (frames > m_monoBuffer.size()) frames = (unsigned)m_monoBuffer.size();

    // 1) 입력을 먼저 읽는다 (녹음은 드라이 원본 그대로, 모니터 신호는 버퍼로).
    //    렌더보다 앞서는 이유: 모니터를 담당 트랙 버스에 "체인 앞"으로 넣어
    //    그 트랙의 EQ/FX가 걸린 소리를 들으며 연주할 수 있게 하기 위해서다.
    const int ch = m_asioInChannels > 0 ? m_asioInChannels : 1;
    const bool rec = m_recording.load(std::memory_order_acquire);
    const bool mon = m_monitor.load(std::memory_order_relaxed);
    const float mg = m_monitorGain.load(std::memory_order_relaxed);
    const int monBus = m_monitorBus.load(std::memory_order_relaxed);
    const bool busRoute = mon && monBus >= 0 && monBus < kBuses;
    float peak = 0.0f;
    for (unsigned i = 0; i < frames; ++i) {
        float s = 0.0f;
        if (input) {
            if (m_inMode == 1) s = input[(std::size_t)i * ch + 0];               // 입력1
            else if (m_inMode == 2) s = input[(std::size_t)i * ch + (ch >= 2 ? 1 : 0)]; // 입력2
            else { for (int c = 0; c < ch; ++c) s += input[(std::size_t)i * ch + c]; s /= (float)ch; }
        }
        const float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        if (rec) recordSample(s); // 녹음은 언제나 드라이
        if (mon) m_monBuf[i] = s;
        tapPush(s); // 연습 모드 피치 분석용
    }
    const float prev = m_inLevel.load(std::memory_order_relaxed);
    m_inLevel.store(peak > prev ? peak : prev * 0.85f, std::memory_order_relaxed);

    m_monCount = busRoute ? frames : 0; // renderBlock이 버스에 섞는다
    renderBlock(frames);                // 신스/VST/클립 (+ 모니터 버스 라우팅)

    // 버스 라우팅이 아니면 (담당 트랙 없음) 예전처럼 마스터 직행으로 모니터
    if (mon && !busRoute)
        for (unsigned i = 0; i < frames; ++i) {
            m_planarL[i] += m_monBuf[i] * mg;
            m_planarR[i] += m_monBuf[i] * mg;
        }

    const float master = m_masterGain.load(std::memory_order_relaxed);
    const float mpan = m_masterPan.load(std::memory_order_relaxed);
    const float mgl = master * (mpan <= 0.0f ? 1.0f : 1.0f - mpan);
    const float mgr = master * (mpan >= 0.0f ? 1.0f : 1.0f + mpan);
    // 볼륨/팬을 planar에 먼저 적용하고, 리미터를 최종 단계로 건다 (내보내기 포함)
    for (unsigned i = 0; i < frames; ++i) {
        m_planarL[i] *= mgl;
        m_planarR[i] *= mgr;
    }
    if (m_limiterOn.load(std::memory_order_relaxed)) {
        float* mp[2] = {m_planarL.data(), m_planarR.data()};
        m_masterLimiter.process(mp, (int)frames, m_sampleRate);
    }
    float pkL = 0.0f, pkR = 0.0f; // 클램프 "직전" 피크 (>1 = 클리핑)
    for (unsigned i = 0; i < frames; ++i) {
        float l = m_planarL[i], r = m_planarR[i];
        const float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
        l = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        r = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        output[i * 2 + 0] = l;
        output[i * 2 + 1] = r;
        captureMasterFrame(l, r); // WAV 내보내기: 들리는 그대로 캡처
    }
    atomicStoreMax(m_masterPeakL, pkL);
    atomicStoreMax(m_masterPeakR, pkR);

    if (m_sampleRate > 0.0 && frames > 0) { // 콜백 부하 (성능 창)
        const double used =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - perfT0).count();
        const float inst = (float)(used / ((double)frames / m_sampleRate));
        m_audioLoad.store(m_audioLoad.load(std::memory_order_relaxed) * 0.9f + inst * 0.1f,
                          std::memory_order_relaxed);
    }
}

// ---- 메트로놈/카운트인 클릭 원샷 재생 (오디오 스레드) ----
// 합성 클릭 "삑" 클립 생성 (GUI 스레드). 음이 바뀔 때만 다시 만든다.
// 짧은 사인 + 빠른 지수 감쇠 — 기존 신스 클릭과 비슷한 톤.
void RtAudioEngine::ensureClickBeep(int kind, uint8_t note) {
    if (kind < 0 || kind >= kClickKinds) return;
    if (m_clickBeepNote[(std::size_t)kind].load(std::memory_order_relaxed) == (int)note)
        return;
    constexpr int kSr = 44100;
    const double hz = 440.0 * std::pow(2.0, ((double)note - 69.0) / 12.0);
    auto clip = std::make_shared<AudioClip>();
    clip->channels = 1;
    clip->sampleRate = kSr;
    const int n = kSr / 12; // ~83ms
    clip->pcm.resize((std::size_t)n);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / kSr;
        const double env = std::exp(-t / 0.025); // 25ms 감쇠
        clip->pcm[(std::size_t)i] = (float)(std::sin(2.0 * 3.14159265358979 * hz * t) * env * 0.7);
    }
    clip->trimLen = n;
    std::atomic_store(&m_clickBeep[(std::size_t)kind],
                      std::shared_ptr<const AudioClip>(std::move(clip)));
    m_clickBeepNote[(std::size_t)kind].store((int)note, std::memory_order_relaxed);
}

void RtAudioEngine::triggerClick(std::shared_ptr<const AudioClip> smp, float gain) {
    // 빈 슬롯을 찾고, 없으면 가장 진행된(가장 오래된) 슬롯을 뺏는다.
    ClickVoice* slot = nullptr;
    for (auto& v : m_clickVoices)
        if (!v.clip) {
            slot = &v;
            break;
        }
    if (!slot) {
        slot = &m_clickVoices[0];
        for (auto& v : m_clickVoices)
            if (v.pos > slot->pos) slot = &v;
    }
    slot->step = m_sampleRate > 0 ? (double)smp->sampleRate / m_sampleRate : 1.0;
    slot->clip = std::move(smp);
    slot->pos = 0.0;
    slot->gain = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
}

void RtAudioEngine::mixClickVoices(unsigned frames) {
    for (auto& v : m_clickVoices) {
        if (!v.clip) continue;
        const double end = (double)v.clip->frames();
        for (unsigned i = 0; i < frames; ++i) {
            if (v.pos >= end) {
                v.clip.reset();
                break;
            }
            float l, r;
            v.clip->sampleAt(v.pos, l, r);
            m_planarL[i] += l * v.gain;
            m_planarR[i] += r * v.gain;
            v.pos += v.step;
        }
    }
}

// ---- 드럼 샘플 원샷 (오디오 스레드) ----
void RtAudioEngine::triggerDrumSample(int bus, std::shared_ptr<const AudioClip> smp,
                                      float gain) {
    if (bus < 0 || bus >= kBuses) bus = 9;
    DrumSampleVoice* slot = nullptr;
    for (auto& v : m_drumVoices)
        if (!v.clip) {
            slot = &v;
            break;
        }
    if (!slot) { // 빈 슬롯이 없으면 가장 진행된 것을 뺏는다
        slot = &m_drumVoices[0];
        for (auto& v : m_drumVoices)
            if (v.pos > slot->pos) slot = &v;
    }
    slot->step = m_sampleRate > 0 ? (double)smp->sampleRate / m_sampleRate : 1.0;
    slot->clip = std::move(smp);
    slot->pos = 0.0;
    gain = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
    slot->gain = gain * gain; // 벨로시티 제곱 커브 (강세가 또렷하게)
    slot->bus = bus;
}

void RtAudioEngine::mixDrumVoices(unsigned frames) {
    for (auto& v : m_drumVoices) {
        if (!v.clip) continue;
        const double end = (double)v.clip->frames();
        float* bl = m_busL[v.bus].data();
        float* br = m_busR[v.bus].data();
        for (unsigned i = 0; i < frames; ++i) {
            if (v.pos >= end) {
                v.clip.reset();
                break;
            }
            float l, r;
            v.clip->sampleAt(v.pos, l, r);
            bl[i] += l * v.gain;
            br[i] += r * v.gain;
            v.pos += v.step;
        }
    }
}

// ---- 오디오 클립 믹싱 (오디오 스레드) ----
void RtAudioEngine::mixAudioClips(unsigned frames) {
    if (!m_audioPlaying.load(std::memory_order_acquire)) return;
    auto clips = std::atomic_load(&m_mixClips); // shared_ptr 원자 로드 (수명 보장)
    if (!clips || clips->empty()) {
        m_audioSample.fetch_add((int64_t)frames, std::memory_order_relaxed);
        return;
    }
    const int64_t base = m_audioSample.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < frames; ++i) {
        const int64_t pos = base + (int64_t)i;
        for (const auto& c : *clips) {
            if (c.muted || !c.clip) continue;
            const int64_t rel = pos - c.startFrame;
            if (rel < 0) continue;
            const double elapsedSrc = (double)rel * c.srcPerEngine;
            if (elapsedSrc >= (double)c.lengthSrcFrames) continue; // 클립 끝(공백 포함)
            const double srcFrame = (double)c.sourceOffset + elapsedSrc;
            float l, r;
            c.clip->sampleAt(srcFrame, l, r); // 소스 범위 밖이면 0(무음)
            // 페이드 인/아웃: 시작·끝에서 게인을 램프시켜 클릭음을 없앤다
            float fg = 1.0f;
            if (c.fadeInFrames > 0 && rel < c.fadeInFrames)
                fg = (float)rel / (float)c.fadeInFrames;
            if (c.fadeOutFrames > 0) {
                const int64_t remain = c.lengthEngFrames - rel;
                if (remain < c.fadeOutFrames)
                    fg *= remain > 0 ? (float)remain / (float)c.fadeOutFrames : 0.0f;
            }
            // 클립은 그 트랙의 버스로 간다 -> 트랙 이펙트 체인이 걸린다
            const int b = (c.bus < 0 || c.bus >= kBuses) ? 0 : c.bus;
            m_busL[b][i] += l * c.gainL * fg;
            m_busR[b][i] += r * c.gainR * fg;
        }
    }
    m_audioSample.store(base + (int64_t)frames, std::memory_order_relaxed);
}

void RtAudioEngine::setAudioMix(std::shared_ptr<const std::vector<AudioMixClip>> clips) {
    std::atomic_store(&m_mixClips, clips);
}
void RtAudioEngine::startAudio(int64_t startFrame) {
    m_audioSample.store(startFrame, std::memory_order_relaxed);
    m_audioPlaying.store(true, std::memory_order_release);
}
void RtAudioEngine::stopAudio() { m_audioPlaying.store(false, std::memory_order_release); }
void RtAudioEngine::seekAudio(int64_t frame) {
    m_audioSample.store(frame, std::memory_order_relaxed);
}

// ---- 오디오 입력(캡처) ----
std::vector<std::string> RtAudioEngine::listInputDevices() {
    std::vector<std::string> names;
    m_inIds.clear();
    if (!m_inAudio) return names;
    for (unsigned id : m_inAudio->getDeviceIds()) {
        RtAudio::DeviceInfo info = m_inAudio->getDeviceInfo(id);
        if (info.inputChannels == 0) continue; // 입력 채널이 있는 장치만
        m_inIds.push_back(id);
        std::string label = info.name;
        if (info.isDefaultInput) label += "  (시스템 기본)";
        names.push_back(label);
    }
    return names;
}

int RtAudioEngine::inputDevice() const {
    for (int i = 0; i < (int)m_inIds.size(); ++i)
        if (m_inIds[i] == m_inDeviceId) return i;
    return -1;
}

void RtAudioEngine::setInputDevice(int index) {
    if (index < 0 || index >= (int)m_inIds.size()) return;
    const unsigned id = m_inIds[index];
    if (id == m_inDeviceId) return;
    const bool was = m_inOpen.load(std::memory_order_acquire);
    if (was) stopInput();
    m_inDeviceId = id;
    if (was) startInput(); // 새 장치로 재시작
}

void RtAudioEngine::setInputChannelMode(int mode) {
    if (mode < 0 || mode > 2 || mode == m_inMode) return;
    m_inMode = mode;
    const bool was = m_inOpen.load(std::memory_order_acquire);
    if (was) { stopInput(); startInput(); } // 새 채널 구성으로 재시작
}

void RtAudioEngine::setBufferFrames(unsigned frames) {
    // 2의 거듭제곱 32~2048로 제한. 작을수록 지연이 줄지만 끊김 위험이 커진다.
    if (frames < 32) frames = 32;
    if (frames > 2048) frames = 2048;
    if (frames == m_bufferFrames) return;
    const bool outWas = m_open.load(std::memory_order_acquire);
    const bool inWas = m_inOpen.load(std::memory_order_acquire);
    if (inWas) stopInput();
    if (outWas) closePort();
    m_bufferFrames = frames;
    if (outWas) openPort(0);  // 출력 스트림을 새 버퍼로 재시작
    if (inWas) startInput();  // 입력 스트림도 재시작
}

bool RtAudioEngine::startInput() {
    if (!m_inAudio) return false;
    if (m_inOpen.load(std::memory_order_acquire)) return true;

    unsigned dev = m_inDeviceId ? m_inDeviceId : m_inAudio->getDefaultInputDevice();
    RtAudio::DeviceInfo info = m_inAudio->getDeviceInfo(dev);
    if (info.inputChannels == 0) {
        dev = m_inAudio->getDefaultInputDevice();
        info = m_inAudio->getDeviceInfo(dev);
        if (info.inputChannels == 0) {
            std::cerr << "[RtAudioEngine] 입력 장치가 없습니다\n";
            return false;
        }
    }

    // 채널 모드에 따라 캡처할 채널을 고른다.
    //   0 = 1+2 합침(가능하면 2채널 캡처 후 평균), 1 = 입력1, 2 = 입력2
    const unsigned devIn = info.inputChannels;
    unsigned first = 0, count = 1;
    if (m_inMode == 0) { first = 0; count = devIn >= 2 ? 2u : 1u; }
    else if (m_inMode == 1) { first = 0; count = 1; }
    else { first = devIn >= 2 ? 1u : 0u; count = 1; } // 입력2

    RtAudio::StreamParameters p;
    p.deviceId = dev;
    p.nChannels = count;
    p.firstChannel = first;
    m_inChannels = (int)count;

    unsigned frames = m_bufferFrames;
    RtAudio::StreamOptions opt;
    opt.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;

    // 출력 스트림과 같은 샘플레이트로 요청해 링/녹음 처리를 단순화한다.
    RtAudioErrorType err =
        m_inAudio->openStream(nullptr, &p, RTAUDIO_FLOAT32, (unsigned)m_sampleRate, &frames,
                              &RtAudioEngine::inCallback, this, &opt);
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] 입력 openStream 실패: " << m_inAudio->getErrorText() << "\n";
        return false;
    }
    err = m_inAudio->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] 입력 startStream 실패: " << m_inAudio->getErrorText() << "\n";
        m_inAudio->closeStream();
        return false;
    }
    m_inDeviceId = dev;
    // 장치 버퍼 지연 (연습 판정에서 타격 시각을 이만큼 앞으로 당긴다).
    // RtAudio WASAPI는 GetStreamLatency의 100ns 단위 값을 프레임 수처럼
    // 돌려주는 버그가 있어 수 초로 튈 수 있다 — 그럴듯한 범위만 믿는다.
    const long lat = (long)m_inAudio->getStreamLatency();
    double sec = m_sampleRate > 0 ? (double)lat / m_sampleRate : 0.0;
    if (!(sec >= 0.0 && sec <= 0.35)) sec = 0.0;
    m_inLatencySec.store(sec, std::memory_order_relaxed);
    m_inOpen.store(true, std::memory_order_release);
    return true;
}

void RtAudioEngine::stopInput() {
    if (!m_inAudio) return;
    if (m_inOpen.load(std::memory_order_acquire)) {
        if (m_inAudio->isStreamRunning()) m_inAudio->stopStream();
        if (m_inAudio->isStreamOpen()) m_inAudio->closeStream();
        m_inOpen.store(false, std::memory_order_release);
    }
    m_inLevel.store(0.0f, std::memory_order_relaxed);
}

int RtAudioEngine::inCallback(void* /*output*/, void* input, unsigned frames,
                              double /*streamTime*/, unsigned /*status*/, void* userData) {
    static_cast<RtAudioEngine*>(userData)->inputCapture(static_cast<const float*>(input), frames);
    return 0;
}

void RtAudioEngine::inputCapture(const float* input, unsigned frames) {
    if (!input) return;
    const int ch = m_inChannels > 0 ? m_inChannels : 1;
    const bool rec = m_recording.load(std::memory_order_acquire);
    float peak = 0.0f;
    for (unsigned i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c) s += input[(std::size_t)i * ch + c];
        s /= (float)ch; // mono 다운믹스
        const float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        // 녹음 중이면 사전 할당된 청크에 기록(재할당 없음, Rule 3)
        if (rec) recordSample(s);
        m_inRing.push(s); // 모니터용 (넘치면 자동 드롭)
        tapPush(s);       // 연습 모드 피치 분석용
    }
    // 레벨 미터: 피크는 즉시 반영, 감쇠는 완만히
    const float prev = m_inLevel.load(std::memory_order_relaxed);
    m_inLevel.store(peak > prev ? peak : prev * 0.85f, std::memory_order_relaxed);
}

// 제어 스레드: index번 청크가 없으면 할당해 원자 포인터로 공개한다.
void RtAudioEngine::ensureRecChunk(std::size_t index) {
    if (index >= kRecMaxChunks) return;
    if (m_recChunks[index].load(std::memory_order_relaxed) != nullptr) return;
    auto chunk = std::make_unique<std::vector<float>>(kRecChunkFrames, 0.0f);
    float* raw = chunk->data();
    m_recChunkStore.push_back(std::move(chunk));
    m_recChunks[index].store(raw, std::memory_order_release);
}

void RtAudioEngine::startRecording() {
    if (m_recording.load(std::memory_order_acquire)) return;
    m_recChunkStore.clear();
    for (auto& p : m_recChunks) p.store(nullptr, std::memory_order_relaxed);
    // 첫 두 청크(약 1분)를 미리 확보. 이후는 pumpRecording이 앞서서 채운다.
    ensureRecChunk(0);
    ensureRecChunk(1);
    m_recCount.store(0, std::memory_order_release);
    m_recording.store(true, std::memory_order_release);
}

void RtAudioEngine::pumpRecording() {
    if (!m_recording.load(std::memory_order_acquire)) return;
    // 현재 쓰는 청크 + 앞으로 2개를 항상 준비해 둔다 (청크당 30초라 여유 충분).
    const std::size_t cur = m_recCount.load(std::memory_order_acquire) / kRecChunkFrames;
    ensureRecChunk(cur + 1);
    ensureRecChunk(cur + 2);
}

std::shared_ptr<AudioClip> RtAudioEngine::stopRecording() {
    if (!m_recording.load(std::memory_order_acquire)) return nullptr;
    m_recording.store(false, std::memory_order_release);
    const std::size_t n = m_recCount.load(std::memory_order_acquire);

    std::shared_ptr<AudioClip> clip;
    if (n > 0) {
        clip = std::make_shared<AudioClip>();
        clip->name = "녹음";
        clip->channels = 1;
        clip->sampleRate = (int)m_sampleRate;
        clip->pcm.resize(n);
        // 청크들을 순서대로 이어 붙인다
        std::size_t copied = 0;
        for (std::size_t k = 0; copied < n && k < kRecMaxChunks; ++k) {
            const float* buf = m_recChunks[k].load(std::memory_order_acquire);
            if (!buf) break;
            const std::size_t take = std::min(n - copied, kRecChunkFrames);
            std::copy(buf, buf + take, clip->pcm.begin() + (std::ptrdiff_t)copied);
            copied += take;
        }
        clip->trimStart = 0;
        clip->trimLen = (int64_t)clip->frames();
        clip->buildPeaks();
    }
    for (auto& p : m_recChunks) p.store(nullptr, std::memory_order_relaxed);
    m_recChunkStore.clear();
    return clip;
}

// ---- 오프라인 렌더 (내보내기) ----
void RtAudioEngine::beginOfflineRender(int64_t startFrame) {
    m_offlineSuspend = suspendStreams(); // 오디오 스레드가 없도록 스트림 정지
    allocateWorkBuffers();               // ASIO만 쓰던 경우에도 렌더 버퍼 확보
    m_synth.prepare(m_sampleRate);       // 이전 재생 잔음 제거 (보이스 리셋)
    m_audioSample.store(startFrame, std::memory_order_relaxed);
    m_audioPlaying.store(true, std::memory_order_release);
}

void RtAudioEngine::endOfflineRender() {
    m_audioPlaying.store(false, std::memory_order_release);
    m_synth.allSoundOff(); // 잔여 보이스 정리 (스트림 정지 상태라 직접 호출 안전)
    resumeStreams(m_offlineSuspend);
    m_offlineSuspend = StreamSuspend{};
}

// ---- 마스터 캡처 (WAV 내보내기) ----
void RtAudioEngine::startMasterCapture(double maxSeconds) {
    if (m_capturing.load(std::memory_order_acquire)) return;
    if (maxSeconds < 1.0) maxSeconds = 1.0;
    if (maxSeconds > 3600.0) maxSeconds = 3600.0; // 안전 상한 1시간
    m_capCap = (std::size_t)(m_sampleRate * maxSeconds) * 2; // 스테레오 인터리브
    m_capBuf.assign(m_capCap, 0.0f); // 제어 스레드에서 미리 할당 (Rule 3)
    m_capCount.store(0, std::memory_order_release);
    m_capturing.store(true, std::memory_order_release);
}

std::shared_ptr<AudioClip> RtAudioEngine::stopMasterCapture() {
    if (!m_capturing.load(std::memory_order_acquire)) return nullptr;
    m_capturing.store(false, std::memory_order_release);
    const std::size_t n = m_capCount.load(std::memory_order_acquire);
    if (n < 2) {
        m_capBuf.clear();
        m_capBuf.shrink_to_fit();
        return nullptr;
    }
    auto clip = std::make_shared<AudioClip>();
    clip->name = "mixdown";
    clip->channels = 2;
    clip->sampleRate = (int)m_sampleRate;
    clip->pcm.assign(m_capBuf.begin(), m_capBuf.begin() + n);
    clip->trimStart = 0;
    clip->trimLen = (int64_t)clip->frames();
    m_capBuf.clear();
    m_capBuf.shrink_to_fit();
    return clip;
}

// ---- ASIO (저지연 듀플렉스) ----
bool RtAudioEngine::ensureAsio() {
    if (m_asio) return true;
    try {
        m_asio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
    } catch (...) {
        m_asio.reset();
    }
    return m_asio != nullptr;
}

std::vector<std::string> RtAudioEngine::listAsioDevices() {
    std::vector<std::string> names;
    m_asioIds.clear();
    if (!ensureAsio()) return names;
    ensureComSTA(); // ASIO 드라이버 로드에 COM STA 필요
    // 드라이버 로드가 크래시해도 앱이 죽지 않게 SEH로 감싼다.
    if (!asioProbeGuarded(m_asio.get(), m_asioIds, names)) {
        std::cerr << "[RtAudioEngine] ASIO 장치 탐색 중 오류(드라이버 문제)\n";
        m_asioIds.clear();
        names.clear();
    }
    return names;
}

bool RtAudioEngine::startAsio(int deviceIndex, int channelMode) {
    if (!ensureAsio()) return false;
    ensureComSTA();
    if (m_asioOn.load(std::memory_order_acquire)) stopAsioInternal(false);
    if (m_asioIds.empty()) listAsioDevices();
    if (m_asioIds.empty()) {
        std::cerr << "[RtAudioEngine] ASIO 드라이버가 없습니다\n";
        return false;
    }
    const unsigned dev = (deviceIndex >= 0 && deviceIndex < (int)m_asioIds.size())
                             ? m_asioIds[deviceIndex]
                             : m_asioIds[0];
    m_asioDeviceIndex = (deviceIndex >= 0 && deviceIndex < (int)m_asioIds.size()) ? deviceIndex : 0;
    RtAudio::DeviceInfo info = m_asio->getDeviceInfo(dev);
    if (info.outputChannels == 0) return false;
    m_inMode = channelMode;

    // WASAPI 출력/입력 스트림을 닫아 충돌·중복 출력 방지.
    // ASIO가 출력까지 담당하므로 WASAPI 출력/입력 스트림을 닫는다.
    // (다시 끌 때는 stopAsio()가 무조건 WASAPI 출력을 되살린다)
    if (isOpen()) closePort();
    stopInput();

    RtAudio::StreamParameters op, ip;
    op.deviceId = dev;
    op.nChannels = 2;
    op.firstChannel = 0;
    ip.deviceId = dev;
    ip.nChannels = info.inputChannels >= 2 ? 2u : 1u;
    ip.firstChannel = 0;
    m_asioInChannels = (int)ip.nChannels;

    // 버퍼는 자동: 0을 넘기면 RtAudio가 드라이버 권장 크기(preferSize)를 쓴다.
    unsigned frames = 0;
    const double sr = info.preferredSampleRate > 0 ? (double)info.preferredSampleRate : m_sampleRate;
    RtAudio::StreamOptions opt;
    opt.flags = RTAUDIO_SCHEDULE_REALTIME;

    // 콜백용 작업 버퍼 사전 할당. ASIO만 열 때는 openPort()를 거치지 않으므로
    // 여기서 확보해 두지 않으면 렌더 버퍼가 비어 소리가 나지 않는다 (Rule 3).
    allocateWorkBuffers();

    RtAudioErrorType err = RTAUDIO_NO_ERROR;
    // 드라이버 열기가 크래시해도 앱이 죽지 않게 SEH로 감싼다.
    if (!asioOpenGuarded(m_asio.get(), &op, &ip, (unsigned)sr, &frames,
                         &RtAudioEngine::asioCallback, this, &opt, &err) ||
        err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] ASIO openStream 실패\n";
        if (!isOpen()) openPort(0); // 출력이 닫힌 채 남지 않게 복구
        return false;
    }
    m_sampleRate = sr;
    m_synth.prepare(sr);
    m_bufferFrames = frames;

    err = m_asio->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[RtAudioEngine] ASIO startStream 실패: " << m_asio->getErrorText() << "\n";
        m_asio->closeStream();
        if (!isOpen()) openPort(0); // 출력이 닫힌 채 남지 않게 복구
        return false;
    }
    m_latencySec.store(sr > 0 ? (double)m_bufferFrames / sr : 0.0, std::memory_order_relaxed);
    m_monitor.store(true, std::memory_order_relaxed); // ASIO는 켜면 바로 모니터
    m_asioOn.store(true, std::memory_order_release);
    return true;
}

void RtAudioEngine::stopAsio() { stopAsioInternal(/*restoreOutput=*/true); }

void RtAudioEngine::stopAsioInternal(bool restoreOutput) {
    if (!m_asio) return;
    if (m_asioOn.load(std::memory_order_acquire)) {
        if (m_asio->isStreamRunning()) m_asio->stopStream();
        if (m_asio->isStreamOpen()) m_asio->closeStream();
        m_asioOn.store(false, std::memory_order_release);
    }
    m_monitor.store(false, std::memory_order_relaxed);
    m_inLevel.store(0.0f, std::memory_order_relaxed);
    // ASIO가 출력을 담당하고 있었으므로, 끄면 반드시 WASAPI 출력을 되살린다.
    // (예전엔 ASIO 시작 전에 WASAPI가 닫혀 있었으면 아무 스트림도 안 열려 무음이 됐다)
    if (restoreOutput && !isOpen()) openPort(0);
}

// ---- 트랙별 이펙트 체인 ----
// 체인 벡터는 오디오 스레드가 매 블록 읽으므로, 스트림을 멈춘 동안에만 바꾼다.
RtAudioEngine::StreamSuspend RtAudioEngine::suspendStreams() {
    StreamSuspend s;
    s.wasAsio = m_asioOn.load(std::memory_order_acquire);
    s.asioDevice = m_asioDeviceIndex;
    s.asioChannelMode = m_inMode;
    if (s.wasAsio) {
        // 출력을 WASAPI로 되살리면 m_sampleRate/m_bufferFrames가 WASAPI 값으로 바뀌어
        // 플러그인이 실제 스트림과 다른 샘플레이트로 준비된다. 복구 없이 멈춘다.
        stopAsioInternal(/*restoreOutput=*/false);
    } else {
        s.wasWasapi = isOpen();
        if (s.wasWasapi) closePort();
    }
    return s;
}

void RtAudioEngine::resumeStreams(const StreamSuspend& s) {
    if (s.wasAsio && startAsio(s.asioDevice, s.asioChannelMode)) return; // ASIO가 출력도 담당
    if (s.wasWasapi && !isOpen()) openPort(0);
}

bool RtAudioEngine::loadTrackEffect(int channel, const std::string& path, int classIndex,
                                    std::string& err) {
    if (channel < 0 || channel >= kBuses) {
        err = "잘못된 트랙 채널";
        return false;
    }
    const StreamSuspend s = suspendStreams();

    bool ok = false;
    auto fx = std::make_unique<TrackEffect>();
    fx->path = path;
    if (fx->host.loadModule(path, err)) {
        // 악기 클래스를 이펙트 슬롯에 끼우면 입력 버스가 없어(numInputs==0)
        // 출력 버퍼를 무음으로 덮어써 트랙이 통째로 안 들린다. 반드시 이펙트
        // 클래스를 골라야 한다. classIndex<0이면 첫 이펙트 클래스를 자동 선택.
        const auto& cls = fx->host.classes();
        int idx = classIndex;
        if (idx < 0 || idx >= (int)cls.size() || cls[(std::size_t)idx].isInstrument) {
            idx = -1;
            for (int c = 0; c < (int)cls.size(); ++c)
                if (cls[(std::size_t)c].isEffect) {
                    idx = c;
                    break;
                }
        }
        if (idx < 0) {
            err = "이 플러그인에는 오디오 이펙트 클래스가 없습니다 (악기 전용)";
        } else if (fx->host.instantiate(idx, m_sampleRate, (int)m_bufferFrames, err)) {
            // 오디오 입력 버스가 없으면 인서트 이펙트로 못 쓴다(출력을 무음으로 덮어씀).
            if (!fx->host.hasAudioInput()) {
                err = "오디오 입력이 없는 플러그인(악기)이라 트랙 이펙트로 쓸 수 없습니다";
                fx->host.unload();
            } else {
                fx->classIndex = idx;
                ok = true;
            }
        }
    }

    if (ok) m_trackFx[channel].push_back(std::move(fx));
    else if (fx) fx->host.unload();

    resumeStreams(s);
    return ok;
}

void RtAudioEngine::removeTrackEffect(int channel, int index) {
    if (channel < 0 || channel >= kBuses) return;
    auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size()) return;
    const StreamSuspend s = suspendStreams();
    chain[index]->host.unload();
    chain.erase(chain.begin() + index);
    resumeStreams(s);
}

void RtAudioEngine::moveTrackEffect(int channel, int from, int to) {
    if (channel < 0 || channel >= kBuses) return;
    auto& chain = m_trackFx[channel];
    if (from < 0 || from >= (int)chain.size() || to < 0 || to >= (int)chain.size() ||
        from == to)
        return;
    // 오디오 스레드가 체인을 순회 중일 수 있으니 잠시 멈추고 바꾼다
    const StreamSuspend s = suspendStreams();
    auto tmp = std::move(chain[(std::size_t)from]);
    chain.erase(chain.begin() + from);
    chain.insert(chain.begin() + to, std::move(tmp));
    resumeStreams(s);
}

void RtAudioEngine::clearTrackEffects(int channel) {
    if (channel < 0 || channel >= kBuses) return;
    if (m_trackFx[channel].empty()) return;
    const StreamSuspend s = suspendStreams();
    for (auto& fx : m_trackFx[channel])
        if (fx) fx->host.unload();
    m_trackFx[channel].clear();
    resumeStreams(s);
}

int RtAudioEngine::trackEffectCount(int channel) const {
    if (channel < 0 || channel >= kBuses) return 0;
    return (int)m_trackFx[channel].size();
}

std::string RtAudioEngine::trackEffectName(int channel, int index) const {
    if (channel < 0 || channel >= kBuses) return {};
    const auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return {};
    if (chain[index]->builtin) return BuiltinFx::typeName(chain[index]->builtin->type());
    return chain[index]->host.activeName();
}

bool RtAudioEngine::trackEffectEnabled(int channel, int index) const {
    if (channel < 0 || channel >= kBuses) return false;
    const auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return false;
    return chain[index]->enabled.load(std::memory_order_relaxed);
}

void RtAudioEngine::setTrackEffectEnabled(int channel, int index, bool on) {
    if (channel < 0 || channel >= kBuses) return;
    auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return;
    chain[index]->enabled.store(on, std::memory_order_relaxed); // 실시간 바이패스
}

// 모듈만 열어 클래스 목록을 보고 닫는다 (인스턴스화하지 않으므로 가볍고 안전).
bool RtAudioEngine::pluginHasEffectClass(const std::string& path) {
    vst::Vst3Host probe;
    std::string err;
    if (!probe.loadModule(path, err)) return false;
    bool any = false;
    for (const auto& c : probe.classes())
        if (c.isEffect) {
            any = true;
            break;
        }
    probe.unload();
    return any;
}

vst::Vst3Host* RtAudioEngine::trackEffectHost(int channel, int index) {
    if (channel < 0 || channel >= kBuses) return nullptr;
    auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return nullptr;
    if (chain[index]->builtin) return nullptr; // 내장 이펙트 슬롯 (파라미터 창으로)
    return &chain[index]->host;
}

// 내장 이펙트를 체인 끝에 추가한다. VST 로드와 같은 규칙(스트림 잠시 정지).
bool RtAudioEngine::addBuiltinTrackEffect(int channel, int type) {
    if (channel < 0 || channel >= kBuses) return false;
    if (type < 0 || type >= BuiltinFx::kTypes) return false;
    auto fx = std::make_unique<TrackEffect>();
    fx->builtin = std::make_unique<BuiltinFx>(type);
    const StreamSuspend s = suspendStreams();
    m_trackFx[channel].push_back(std::move(fx));
    resumeStreams(s);
    return true;
}

BuiltinFx* RtAudioEngine::trackEffectBuiltin(int channel, int index) {
    if (channel < 0 || channel >= kBuses) return nullptr;
    auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return nullptr;
    return chain[index]->builtin.get();
}

void RtAudioEngine::setTrackEffectSidechain(int channel, int index, int bus) {
    if (channel < 0 || channel >= kBuses) return;
    auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return;
    chain[index]->sidechain.store(bus < 0 || bus >= kBuses ? -1 : bus,
                                  std::memory_order_relaxed);
}

int RtAudioEngine::trackEffectSidechain(int channel, int index) const {
    if (channel < 0 || channel >= kBuses) return -1;
    const auto& chain = m_trackFx[channel];
    if (index < 0 || index >= (int)chain.size() || !chain[index]) return -1;
    return chain[index]->sidechain.load(std::memory_order_relaxed);
}

// ---- 트랙별 악기 ----
bool RtAudioEngine::pluginHasInstrumentClass(const std::string& path) {
    vst::Vst3Host probe;
    std::string err;
    if (!probe.loadModule(path, err)) return false;
    bool any = false;
    for (const auto& c : probe.classes())
        if (c.isInstrument) {
            any = true;
            break;
        }
    probe.unload();
    return any;
}

bool RtAudioEngine::loadTrackInstrument(int channel, const std::string& path, int classIndex,
                                        std::string& err) {
    if (channel < 0 || channel >= kBuses) {
        err = "잘못된 트랙 채널";
        return false;
    }
    const StreamSuspend s = suspendStreams();

    // 기존 슬롯이 있으면 먼저 비운다 (슬롯은 트랙당 1개)
    if (m_trackInst[channel]) {
        m_trackInst[channel]->host.unload();
        m_trackInst[channel].reset();
    }

    bool ok = false;
    auto inst = std::make_unique<TrackInstrument>();
    inst->path = path;
    if (inst->host.loadModule(path, err)) {
        // classIndex<0이면 첫 악기 클래스를 자동 선택. 이펙트 클래스는 거부.
        const auto& cls = inst->host.classes();
        int idx = classIndex;
        if (idx < 0 || idx >= (int)cls.size() || !cls[(std::size_t)idx].isInstrument) {
            idx = -1;
            for (int c = 0; c < (int)cls.size(); ++c)
                if (cls[(std::size_t)c].isInstrument) {
                    idx = c;
                    break;
                }
        }
        if (idx < 0) {
            err = "이 플러그인에는 악기 클래스가 없습니다 (이펙트 전용)";
        } else if (inst->host.instantiate(idx, m_sampleRate, (int)m_bufferFrames, err)) {
            if (!inst->host.isInstrument()) {
                err = "노트 입력을 받지 않는 플러그인이라 악기로 쓸 수 없습니다";
                inst->host.unload();
            } else {
                inst->classIndex = idx;
                ok = true;
            }
        }
    }
    if (ok) m_trackInst[channel] = std::move(inst);
    else if (inst) inst->host.unload();

    resumeStreams(s);
    return ok;
}

void RtAudioEngine::clearTrackInstrument(int channel) {
    if (channel < 0 || channel >= kBuses || !m_trackInst[channel]) return;
    const StreamSuspend s = suspendStreams();
    m_trackInst[channel]->host.unload();
    m_trackInst[channel].reset();
    resumeStreams(s);
}

bool RtAudioEngine::trackInstrumentActive(int channel) const {
    if (channel < 0 || channel >= kBuses) return false;
    return m_trackInst[channel] && m_trackInst[channel]->host.isLoaded();
}

std::string RtAudioEngine::trackInstrumentName(int channel) const {
    if (!trackInstrumentActive(channel)) return {};
    return m_trackInst[channel]->host.activeName();
}

vst::Vst3Host* RtAudioEngine::trackInstrumentHost(int channel) {
    if (!trackInstrumentActive(channel)) return nullptr;
    return &m_trackInst[channel]->host;
}

// ---- VST 로드/해제: 스트림(WASAPI/ASIO 모두)을 잠깐 멈춰 오디오 스레드와 겹치지 않게 ----
bool RtAudioEngine::loadInstrument(const std::string& path, int classIndex, std::string& err) {
    const StreamSuspend s = suspendStreams();
    bool ok = m_instrument.loadModule(path, err) &&
              m_instrument.instantiate(classIndex, m_sampleRate, (int)m_bufferFrames, err);
    if (!ok) m_instrument.unload();
    resumeStreams(s);
    return ok;
}

bool RtAudioEngine::loadEffect(const std::string& path, int classIndex, std::string& err) {
    const StreamSuspend s = suspendStreams();
    bool ok = m_effect.loadModule(path, err) &&
              m_effect.instantiate(classIndex, m_sampleRate, (int)m_bufferFrames, err);
    if (!ok) m_effect.unload();
    resumeStreams(s);
    return ok;
}

void RtAudioEngine::clearInstrument() {
    const StreamSuspend s = suspendStreams();
    m_instrument.unload();
    resumeStreams(s);
}

void RtAudioEngine::clearEffect() {
    const StreamSuspend s = suspendStreams();
    m_effect.unload();
    resumeStreams(s);
}

} // namespace midipro::audio
