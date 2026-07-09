// =============================================================
// MidiPro - midi/RtMidiDevice.cpp
// =============================================================

#include "midi/RtMidiDevice.h"

#include "RtMidi.h"

#include <iostream>

namespace midipro::midi {

// ---------------------------------------------------------
// RtMidiInput
// ---------------------------------------------------------
RtMidiInput::RtMidiInput() {
    try {
        m_rtIn = std::make_unique<RtMidiIn>(RtMidi::WINDOWS_MM, "MidiPro Input");
    } catch (RtMidiError& e) {
        std::cerr << "[RtMidiInput] 초기화 실패: " << e.getMessage() << "\n";
    }
}

RtMidiInput::~RtMidiInput() {
    closePort();
}

std::vector<std::string> RtMidiInput::listPorts() {
    std::vector<std::string> ports;
    if (!m_rtIn) return ports;
    const unsigned count = m_rtIn->getPortCount();
    for (unsigned i = 0; i < count; ++i) {
        try {
            ports.push_back(m_rtIn->getPortName(i));
        } catch (RtMidiError&) {
            ports.push_back("(이름 조회 실패)");
        }
    }
    return ports;
}

bool RtMidiInput::openPort(unsigned index) {
    if (!m_rtIn) return false;
    closePort();
    try {
        m_rtIn->openPort(index, "MidiPro In");
        // SysEx는 받되 Clock/ActiveSensing은 무시.
        // 왜: 일부 장비는 Clock을 초당 수백 개 보내 큐를 채워버린다.
        m_rtIn->ignoreTypes(/*sysex=*/false, /*time=*/true, /*sense=*/true);
        m_rtIn->setCallback(&RtMidiInput::rtCallback, this);
        m_open = true;
        return true;
    } catch (RtMidiError& e) {
        std::cerr << "[RtMidiInput] 포트 열기 실패: " << e.getMessage() << "\n";
        return false;
    }
}

void RtMidiInput::closePort() {
    if (m_rtIn && m_open) {
        m_rtIn->cancelCallback();
        m_rtIn->closePort();
        m_open = false;
    }
}

bool RtMidiInput::isOpen() const {
    return m_open;
}

bool RtMidiInput::poll(MidiMessage& out) {
    return m_queue.pop(out);
}

std::size_t RtMidiInput::droppedCount() const {
    return m_dropped.load(std::memory_order_relaxed);
}

void RtMidiInput::rtCallback(double /*timestamp*/, std::vector<uint8_t>* bytes, void* userData) {
    // 실시간성 스레드: 파싱(스택 위 고정 크기) + 락프리 push만 수행.
    // 로깅/할당/락은 금지 — 어기면 드라이버 콜백이 밀려 메시지가 유실된다.
    auto* self = static_cast<RtMidiInput*>(userData);
    if (self == nullptr || bytes == nullptr || bytes->empty()) return;

    const MidiMessage message = MidiMessage::parse(bytes->data(), bytes->size());
    if (!self->m_queue.push(message)) {
        // 큐가 가득: 대기하지 않고 버린 뒤 개수만 기록한다.
        self->m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------
// RtMidiOutput
// ---------------------------------------------------------
RtMidiOutput::RtMidiOutput() {
    try {
        m_rtOut = std::make_unique<RtMidiOut>(RtMidi::WINDOWS_MM, "MidiPro Output");
    } catch (RtMidiError& e) {
        std::cerr << "[RtMidiOutput] 초기화 실패: " << e.getMessage() << "\n";
    }
}

RtMidiOutput::~RtMidiOutput() {
    closePort();
}

std::vector<std::string> RtMidiOutput::listPorts() {
    std::vector<std::string> ports;
    if (!m_rtOut) return ports;
    const unsigned count = m_rtOut->getPortCount();
    for (unsigned i = 0; i < count; ++i) {
        try {
            ports.push_back(m_rtOut->getPortName(i));
        } catch (RtMidiError&) {
            ports.push_back("(이름 조회 실패)");
        }
    }
    return ports;
}

bool RtMidiOutput::openPort(unsigned index) {
    if (!m_rtOut) return false;
    closePort();
    try {
        m_rtOut->openPort(index, "MidiPro Out");
        m_open = true;
        return true;
    } catch (RtMidiError& e) {
        std::cerr << "[RtMidiOutput] 포트 열기 실패: " << e.getMessage() << "\n";
        return false;
    }
}

void RtMidiOutput::closePort() {
    if (m_rtOut && m_open) {
        m_rtOut->closePort();
        m_open = false;
    }
}

bool RtMidiOutput::isOpen() const {
    return m_open;
}

bool RtMidiOutput::send(const std::vector<uint8_t>& bytes) {
    if (!m_rtOut || !m_open) return false;
    try {
        m_rtOut->sendMessage(&bytes);
        return true;
    } catch (RtMidiError& e) {
        std::cerr << "[RtMidiOutput] 전송 실패: " << e.getMessage() << "\n";
        return false;
    }
}

} // namespace midipro::midi
