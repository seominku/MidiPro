#pragma once
// =============================================================
// MidiPro - midi/MidiMessage.h
// 파싱된 MIDI 메시지 값 타입 + 파서/생성기.
//
// 왜 동적 할당이 없는 값 타입인가 (Rule 3):
//   이 타입은 MIDI 입력 콜백(실시간성 스레드)에서 만들어져
//   락프리 큐로 복사돼 넘어간다. std::vector/std::string 멤버가
//   있으면 복사마다 할당이 일어나므로 고정 크기 배열만 쓴다.
//
// 순수 로직(파싱/생성/이름 변환)만 담고 있어 장치나 오디오 없이
// 유닛 테스트가 가능하다 (Rule 6). 구현은 MidiMessage.cpp 참고.
// =============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace midipro::midi {

enum class MessageType {
    NoteOff,
    NoteOn,
    PolyAftertouch,
    ControlChange,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    SysEx,
    Clock,
    Start,
    Continue,
    Stop,
    ActiveSensing,
    SystemReset,
    Unknown,
};

class MidiMessage {
public:
    // 로그 표시용으로 보관하는 원본 바이트 상한.
    // SysEx는 수 KB가 될 수 있으므로 앞부분만 저장하고 전체 길이는
    // totalSize()로 따로 알려준다. (실시간 경로에서 가변 길이 금지)
    static constexpr std::size_t kMaxRawBytes = 16;

    MidiMessage() = default;

    // ---------- 파서 ----------
    static MidiMessage parse(const uint8_t* bytes, std::size_t size);
    static MidiMessage parse(const std::vector<uint8_t>& bytes);

    // ---------- 생성기 (전송용 raw 바이트) ----------
    // 반환형이 vector인 이유: 전송은 메인 스레드에서만 하므로
    // 할당이 허용된다. 실시간 경로에서는 호출하지 말 것.
    static std::vector<uint8_t> makeNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static std::vector<uint8_t> makeNoteOff(uint8_t channel, uint8_t note, uint8_t velocity = 0);
    static std::vector<uint8_t> makeControlChange(uint8_t channel, uint8_t cc, uint8_t value);
    static std::vector<uint8_t> makeProgramChange(uint8_t channel, uint8_t program);
    static std::vector<uint8_t> makePitchBend(uint8_t channel, int bend /* -8192 ~ 8191 */);

    // ---------- 조회 ----------
    MessageType type() const { return m_type; }
    uint8_t channel() const { return m_channel; }   // 0~15 (표시할 때는 +1)
    uint8_t data1() const { return m_data1; }       // 노트/CC/프로그램 번호
    uint8_t data2() const { return m_data2; }       // 벨로시티/CC 값
    int pitchBend() const { return m_pitchBend; }   // PitchBend일 때만 유효
    const uint8_t* raw() const { return m_raw; }
    std::size_t rawSize() const { return m_rawSize; }     // 보관된 바이트 수 (<= kMaxRawBytes)
    std::size_t totalSize() const { return m_totalSize; } // 원본 메시지 전체 길이

    // ---------- 표시 (메인 스레드 전용, 할당 있음) ----------
    std::string toString() const;
    static std::string noteName(uint8_t note); // 예: 60 -> "C4"

private:
    MessageType m_type = MessageType::Unknown;
    uint8_t m_channel = 0;
    uint8_t m_data1 = 0;
    uint8_t m_data2 = 0;
    int m_pitchBend = 0;
    uint8_t m_raw[kMaxRawBytes] = {};
    std::size_t m_rawSize = 0;
    std::size_t m_totalSize = 0;
};

} // namespace midipro::midi
