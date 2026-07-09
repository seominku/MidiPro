// =============================================================
// MidiPro - audio/RtAudioEngine.cpp
// =============================================================

#include "audio/RtAudioEngine.h"

#include "midi/MidiConstants.h"
#include "midi2/Ump.h"

#include "RtAudio.h"

#include <iostream>

namespace midipro::audio {

namespace {
constexpr uint8_t kCcAllNotesOff = 123;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcTimbre = 74; // MPE 표준 음색(밝기) 컨트롤
} // namespace

RtAudioEngine::RtAudioEngine() {
    try {
        m_audio = std::make_unique<RtAudio>();
        m_deviceId = m_audio->getDefaultOutputDevice(); // 처음엔 시스템 기본 출력
    } catch (...) {
        std::cerr << "[RtAudioEngine] RtAudio 초기화 실패\n";
    }
    m_synth.prepare(m_sampleRate);
}

RtAudioEngine::~RtAudioEngine() {
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
    m_monoBuffer.assign(4096, 0.0f);
    m_planarL.assign(4096, 0.0f);
    m_planarR.assign(4096, 0.0f);

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

    m_open.store(true, std::memory_order_release);
    return true;
}

void RtAudioEngine::closePort() {
    if (!m_audio) return;
    if (m_open.load(std::memory_order_acquire)) {
        if (m_audio->isStreamRunning()) m_audio->stopStream();
        if (m_audio->isStreamOpen()) m_audio->closeStream();
        m_open.store(false, std::memory_order_release);
    }
}

bool RtAudioEngine::isOpen() const {
    return m_open.load(std::memory_order_acquire);
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
        if (bytes[1] == kCcAllNotesOff || bytes[1] == kCcAllSoundOff) {
            e.type = EngineEvent::Type::AllNotesOff;
            return pushEvent(e);
        }
        if (bytes[1] == kCcTimbre) { // CC74 = MPE 음색(밝기)
            e.type = EngineEvent::Type::Timbre;
            e.value = (float)bytes[2] / 127.0f;
            return pushEvent(e);
        }
        return true; // 나머지 CC는 무시
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
        if (m.index == kCcAllNotesOff || m.index == kCcAllSoundOff) {
            e.type = EngineEvent::Type::AllNotesOff;
            return pushEvent(e);
        }
        if (m.index == kCcTimbre) {
            e.type = EngineEvent::Type::Timbre;
            e.value = midi2::cc32ToFloat(m.data32);
            return pushEvent(e);
        }
        return true;
    default:
        return true;
    }
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

void RtAudioEngine::processCallback(float* output, unsigned frames) {
    if (frames > m_monoBuffer.size()) frames = (unsigned)m_monoBuffer.size();
    const bool useInstrument = m_instrument.isLoaded();

    // MPE 모드에 따라 피치벤드 범위 갱신 (멤버 채널 기본 ±48, 일반 ±2)
    m_synth.setPitchBendRange(m_mpeEnabled.load(std::memory_order_relaxed) ? 48.0f : 2.0f);

    // 1) 큐 명령을 순서대로 적용 (락프리 pop). 노트/표현은 채널과 함께
    //    악기(VST 또는 내장 신스)로, 파라미터는 내장 신스로 전달.
    EngineEvent e;
    while (m_queue.pop(e)) {
        switch (e.type) {
        case EngineEvent::Type::NoteOn:
            if (useInstrument) m_instrument.addNoteOn(e.channel, e.note, e.velocity);
            else m_synth.noteOnFloat(e.channel, e.note, e.value); // 고해상도 벨로시티(0~1)
            break;
        case EngineEvent::Type::NoteOff:
            if (useInstrument) m_instrument.addNoteOff(e.channel, e.note);
            else m_synth.noteOff(e.channel, e.note);
            break;
        case EngineEvent::Type::PitchBend:
            if (useInstrument) m_instrument.addPitchBend(e.channel, e.value);
            else m_synth.setPitchBend(e.channel, e.value);
            break;
        case EngineEvent::Type::PerNotePitchBend:
            // MIDI 2.0 노트별 벤딩은 내장 신스에서 (채널,노트) 보이스에 적용.
            // (VST 노트별 벤딩은 note-expression이 필요 -> 추후)
            if (!useInstrument) m_synth.setPerNotePitchBend(e.channel, e.note, e.value);
            break;
        case EngineEvent::Type::Pressure:
            if (useInstrument) m_instrument.addPressure(e.channel, e.value);
            else m_synth.setPressure(e.channel, e.value);
            break;
        case EngineEvent::Type::Timbre:
            if (useInstrument) m_instrument.addTimbre(e.channel, e.value);
            else m_synth.setTimbre(e.channel, e.value);
            break;
        case EngineEvent::Type::AllNotesOff:
            m_synth.allNotesOff();
            break;
        case EngineEvent::Type::SetParams:
            m_synth.setParams(e.params);
            break;
        }
    }

    // 2) 스테레오 planar 버퍼 생성
    float* planar[2] = {m_planarL.data(), m_planarR.data()};
    if (useInstrument) {
        m_instrument.process(planar, 2, (int)frames); // VSTi가 오디오 생성
    } else {
        m_synth.render(m_monoBuffer.data(), (int)frames); // 내장 신스 mono
        for (unsigned i = 0; i < frames; ++i) {
            m_planarL[i] = m_monoBuffer[i];
            m_planarR[i] = m_monoBuffer[i];
        }
        m_activeVoices.store(m_synth.activeVoiceCount(), std::memory_order_relaxed);
    }

    // 3) 이펙트 VST가 있으면 출력을 제자리 후처리
    if (m_effect.isLoaded()) {
        m_effect.process(planar, 2, (int)frames, planar);
    }

    // 4) 스테레오 인터리브로 출력
    for (unsigned i = 0; i < frames; ++i) {
        output[i * 2 + 0] = m_planarL[i];
        output[i * 2 + 1] = m_planarR[i];
    }
}

// ---- VST 로드/해제: 스트림을 잠깐 멈춰 오디오 스레드와 겹치지 않게 한다 ----
bool RtAudioEngine::loadInstrument(const std::string& path, int classIndex, std::string& err) {
    const bool wasOpen = isOpen();
    if (wasOpen) closePort();
    bool ok = m_instrument.loadModule(path, err) &&
              m_instrument.instantiate(classIndex, m_sampleRate, (int)m_bufferFrames, err);
    if (!ok) m_instrument.unload();
    if (wasOpen) openPort(0);
    return ok;
}

bool RtAudioEngine::loadEffect(const std::string& path, int classIndex, std::string& err) {
    const bool wasOpen = isOpen();
    if (wasOpen) closePort();
    bool ok = m_effect.loadModule(path, err) &&
              m_effect.instantiate(classIndex, m_sampleRate, (int)m_bufferFrames, err);
    if (!ok) m_effect.unload();
    if (wasOpen) openPort(0);
    return ok;
}

void RtAudioEngine::clearInstrument() {
    const bool wasOpen = isOpen();
    if (wasOpen) closePort();
    m_instrument.unload();
    if (wasOpen) openPort(0);
}

void RtAudioEngine::clearEffect() {
    const bool wasOpen = isOpen();
    if (wasOpen) closePort();
    m_effect.unload();
    if (wasOpen) openPort(0);
}

} // namespace midipro::audio
