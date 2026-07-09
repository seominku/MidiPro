#pragma once
// =============================================================
// MidiPro - midi/MidiOutputRouter.h
// 여러 IMidiOutput 대상 중 하나로 전송을 넘겨주는 라우터.
//
// 왜 필요한가 (Rule 1, 8):
//   Player와 GUI는 "출력이 하드웨어 MIDI인지 내장 신스인지"를
//   몰라도 되게 한다. 둘 다 이 라우터(IMidiOutput)에만 의존하고,
//   실제 대상 선택은 라우터가 담당한다. 새 출력이 생겨도(예: 파일
//   렌더링) 상위 코드는 안 바뀐다.
//
//   라우터 자체는 IMidiOutput 인터페이스에만 의존하고 구체 타입
//   (RtMidi/RtAudio)은 모른다. 대상 등록은 조립 지점에서 한다.
// =============================================================

#include "midi/IMidiDevice.h"

#include <string>
#include <vector>

namespace midipro::midi {

class MidiOutputRouter final : public IMidiOutput {
public:
    struct Target {
        std::string label;
        IMidiOutput* output;
    };

    void addTarget(const std::string& label, IMidiOutput* output) {
        m_targets.push_back({label, output});
    }

    const std::vector<Target>& targets() const { return m_targets; }
    int activeTarget() const { return m_active; }
    void setActiveTarget(int index) {
        if (index >= 0 && index < (int)m_targets.size()) m_active = index;
    }

    IMidiOutput* active() {
        if (m_active < 0 || m_active >= (int)m_targets.size()) return nullptr;
        return m_targets[m_active].output;
    }

    // ---- IMidiOutput: 활성 대상으로 위임 ----
    std::vector<std::string> listPorts() override {
        IMidiOutput* out = active();
        return out ? out->listPorts() : std::vector<std::string>{};
    }
    bool openPort(unsigned index) override {
        IMidiOutput* out = active();
        return out ? out->openPort(index) : false;
    }
    void closePort() override {
        if (IMidiOutput* out = active()) out->closePort();
    }
    bool isOpen() const override {
        if (m_active < 0 || m_active >= (int)m_targets.size()) return false;
        return m_targets[m_active].output->isOpen();
    }
    bool send(const std::vector<uint8_t>& bytes) override {
        IMidiOutput* out = active();
        return out ? out->send(bytes) : false;
    }

private:
    std::vector<Target> m_targets;
    int m_active = 0;
};

} // namespace midipro::midi
