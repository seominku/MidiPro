#pragma once
// =============================================================
// MidiPro - midi/IMidiDevice.h
// MIDI 장치 추상 인터페이스.
//
// 왜 인터페이스인가 (Rule 1, 6, 8):
//   - 상위 계층(시퀀서, GUI)이 RtMidi 같은 구체 라이브러리를
//     직접 참조하지 않게 하는 모듈 경계. 라이브러리를 교체해도
//     이 파일 위쪽 코드는 바뀌지 않는다.
//   - 테스트에서는 Mock 구현으로 대체해 하드웨어 없이 검증한다.
//
// 왜 콜백이 아니라 poll 방식인가 (Rule 3):
//   구현체 내부의 실시간 콜백 스레드를 인터페이스 밖으로 노출하지
//   않기 위해서다. 수신 메시지는 구현체가 락프리 큐에 쌓고,
//   소비자(메인 스레드)는 poll()로 하나씩 꺼내 간다.
// =============================================================

#include "midi/MidiMessage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midipro::midi {

class IMidiInput {
public:
    virtual ~IMidiInput() = default;

    virtual std::vector<std::string> listPorts() = 0;
    virtual bool openPort(unsigned index) = 0;
    virtual void closePort() = 0;
    virtual bool isOpen() const = 0;

    // 수신된 메시지를 하나 꺼낸다. 없으면 false.
    virtual bool poll(MidiMessage& out) = 0;

    // 큐가 가득 차서 버려진 메시지 수 (실시간 스레드는 대기 대신 드롭한다)
    virtual std::size_t droppedCount() const = 0;
};

class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    virtual std::vector<std::string> listPorts() = 0;
    virtual bool openPort(unsigned index) = 0;
    virtual void closePort() = 0;
    virtual bool isOpen() const = 0;

    virtual bool send(const std::vector<uint8_t>& bytes) = 0;
};

} // namespace midipro::midi
