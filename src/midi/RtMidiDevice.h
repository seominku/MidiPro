#pragma once
// =============================================================
// MidiPro - midi/RtMidiDevice.h
// IMidiInput / IMidiOutput의 RtMidi 구현체.
//
// 이 헤더는 조립 지점(main)에서만 include한다. 다른 계층은
// IMidiDevice.h의 인터페이스만 봐야 한다 (Rule 1).
// =============================================================

#include "core/SpscQueue.h"
#include "midi/IMidiDevice.h"

#include <atomic>
#include <memory>

// 전방 선언: RtMidi.h를 이 헤더에 include하지 않기 위해서.
// (이 헤더를 include하는 쪽이 RtMidi 헤더 경로를 몰라도 되게 한다)
class RtMidiIn;
class RtMidiOut;

namespace midipro::midi {

class RtMidiInput final : public IMidiInput {
public:
    RtMidiInput();
    ~RtMidiInput() override;

    std::vector<std::string> listPorts() override;
    bool openPort(unsigned index) override;
    void closePort() override;
    bool isOpen() const override;
    bool poll(MidiMessage& out) override;
    std::size_t droppedCount() const override;

private:
    // RtMidi가 드라이버 스레드에서 호출하는 콜백.
    // 여기서는 파싱 + 락프리 큐 push만 한다 (할당/락/로깅 금지, Rule 3).
    static void rtCallback(double timestamp, std::vector<uint8_t>* bytes, void* userData);

    static constexpr std::size_t kQueueCapacity = 1024;

    std::unique_ptr<RtMidiIn> m_rtIn;
    core::SpscQueue<MidiMessage, kQueueCapacity> m_queue;
    std::atomic<std::size_t> m_dropped{0};
    bool m_open = false;
};

class RtMidiOutput final : public IMidiOutput {
public:
    RtMidiOutput();
    ~RtMidiOutput() override;

    std::vector<std::string> listPorts() override;
    bool openPort(unsigned index) override;
    void closePort() override;
    bool isOpen() const override;
    bool send(const std::vector<uint8_t>& bytes) override;

private:
    std::unique_ptr<RtMidiOut> m_rtOut;
    bool m_open = false;
};

} // namespace midipro::midi
