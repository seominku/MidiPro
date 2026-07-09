#pragma once
// =============================================================
// MidiPro - audio/IVstHostControl.h
// VST3 호스트 제어 인터페이스 (GUI가 의존하는 얇은 경계, Rule 1).
//
// RtAudioEngine이 구현한다. GUI는 이 인터페이스로만 플러그인을
// 불러오거나 해제하고, 클래스 열거/에디터는 Vst3Host를 통해 한다.
// =============================================================

#include "vst/Vst3Host.h"

#include <string>

namespace midipro::audio {

class IVstHostControl {
public:
    virtual ~IVstHostControl() = default;

    // 스트림을 잠깐 멈추고 안전하게 로드/해제한다 (엔진이 처리).
    virtual bool loadInstrument(const std::string& path, int classIndex, std::string& err) = 0;
    virtual bool loadEffect(const std::string& path, int classIndex, std::string& err) = 0;
    virtual void clearInstrument() = 0;
    virtual void clearEffect() = 0;
    virtual bool instrumentActive() const = 0;
    virtual bool effectActive() const = 0;

    // 클래스 목록/에디터 접근용 (GUI 스레드)
    virtual vst::Vst3Host& instrumentHost() = 0;
    virtual vst::Vst3Host& effectHost() = 0;
};

} // namespace midipro::audio
