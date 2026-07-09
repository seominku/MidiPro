#pragma once
// =============================================================
// MidiPro - audio/IAudioEngine.h
// 오디오 엔진 확장 지점 (Phase 3 예정).
//
// 왜 지금 만들어두는가 (Rule 8):
//   Phase 3에서 신디사이저(RtAudio + 오실레이터/ADSR)가 들어올
//   자리를 인터페이스로 미리 비워둔다. 시퀀서(Phase 2)는 이
//   인터페이스에만 의존하게 설계해, 신스가 나중에 붙어도
//   시퀀서 코드를 갈아엎지 않게 한다.
//
// 주의: noteOn/noteOff는 오디오 콜백이 아닌 제어 스레드에서
//   호출되는 API다. 구현체는 내부에서 락프리 방식으로 오디오
//   스레드에 전달해야 한다 (Rule 3).
// =============================================================

#include <cstdint>

namespace midipro::audio {

class IAudioEngine {
public:
    virtual ~IAudioEngine() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    virtual void noteOn(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
    virtual void noteOff(uint8_t channel, uint8_t note) = 0;
    virtual void allNotesOff() = 0;
};

} // namespace midipro::audio
