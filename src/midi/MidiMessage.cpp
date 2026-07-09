// =============================================================
// MidiPro - midi/MidiMessage.cpp
// =============================================================

#include "midi/MidiMessage.h"
#include "midi/MidiConstants.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace midipro::midi {

// ---------------------------------------------------------
// 파서
// ---------------------------------------------------------
MidiMessage MidiMessage::parse(const uint8_t* bytes, std::size_t size) {
    MidiMessage m;
    if (bytes == nullptr || size == 0) {
        return m;
    }

    // 원본 바이트 보관 (표시용, 상한까지만)
    m.m_totalSize = size;
    m.m_rawSize = std::min(size, kMaxRawBytes);
    std::copy(bytes, bytes + m.m_rawSize, m.m_raw);

    const uint8_t status = bytes[0];

    // 시스템 메시지 (채널 없음)
    if (status >= kStatusSysExStart) {
        switch (status) {
        case kStatusSysExStart:    m.m_type = MessageType::SysEx;         break;
        case kStatusClock:         m.m_type = MessageType::Clock;         break;
        case kStatusStart:         m.m_type = MessageType::Start;         break;
        case kStatusContinue:      m.m_type = MessageType::Continue;      break;
        case kStatusStop:          m.m_type = MessageType::Stop;          break;
        case kStatusActiveSensing: m.m_type = MessageType::ActiveSensing; break;
        case kStatusSystemReset:   m.m_type = MessageType::SystemReset;   break;
        default:                   m.m_type = MessageType::Unknown;       break;
        }
        return m;
    }

    // 채널 메시지
    m.m_channel = status & kChannelMask;
    if (size > 1) m.m_data1 = bytes[1];
    if (size > 2) m.m_data2 = bytes[2];

    switch (status & kStatusMask) {
    case kStatusNoteOff:
        m.m_type = MessageType::NoteOff;
        break;
    case kStatusNoteOn:
        // 왜: MIDI 관례상 velocity 0인 Note On은 Note Off로 취급된다.
        // (러닝 스테이터스 최적화를 위해 많은 장비가 이렇게 보낸다)
        m.m_type = (m.m_data2 == 0) ? MessageType::NoteOff : MessageType::NoteOn;
        break;
    case kStatusPolyAftertouch:
        m.m_type = MessageType::PolyAftertouch;
        break;
    case kStatusControlChange:
        m.m_type = MessageType::ControlChange;
        break;
    case kStatusProgramChange:
        m.m_type = MessageType::ProgramChange;
        break;
    case kStatusChannelAftertouch:
        m.m_type = MessageType::ChannelAftertouch;
        break;
    case kStatusPitchBend:
        m.m_type = MessageType::PitchBend;
        // 14비트 리틀엔디언(LSB 먼저) 조합 후 중앙값 기준으로 변환
        m.m_pitchBend = ((int)m.m_data2 << 7 | m.m_data1) - kPitchBendCenter;
        break;
    default:
        m.m_type = MessageType::Unknown;
        break;
    }
    return m;
}

MidiMessage MidiMessage::parse(const std::vector<uint8_t>& bytes) {
    return parse(bytes.data(), bytes.size());
}

// ---------------------------------------------------------
// 생성기
// ---------------------------------------------------------
std::vector<uint8_t> MidiMessage::makeNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    return {(uint8_t)(kStatusNoteOn | (channel & kChannelMask)),
            (uint8_t)(note & kDataMask),
            (uint8_t)(velocity & kDataMask)};
}

std::vector<uint8_t> MidiMessage::makeNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    return {(uint8_t)(kStatusNoteOff | (channel & kChannelMask)),
            (uint8_t)(note & kDataMask),
            (uint8_t)(velocity & kDataMask)};
}

std::vector<uint8_t> MidiMessage::makeControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
    return {(uint8_t)(kStatusControlChange | (channel & kChannelMask)),
            (uint8_t)(cc & kDataMask),
            (uint8_t)(value & kDataMask)};
}

std::vector<uint8_t> MidiMessage::makeProgramChange(uint8_t channel, uint8_t program) {
    return {(uint8_t)(kStatusProgramChange | (channel & kChannelMask)),
            (uint8_t)(program & kDataMask)};
}

std::vector<uint8_t> MidiMessage::makePitchBend(uint8_t channel, int bend) {
    int value = bend + kPitchBendCenter;
    value = std::clamp(value, 0, kPitchBendCenter + kPitchBendMax);
    return {(uint8_t)(kStatusPitchBend | (channel & kChannelMask)),
            (uint8_t)(value & kDataMask),
            (uint8_t)((value >> 7) & kDataMask)};
}

// ---------------------------------------------------------
// 표시
// ---------------------------------------------------------
std::string MidiMessage::noteName(uint8_t note) {
    static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    // 왜 -1인가: MIDI 표준에서 노트 0 = C-1 (가운데 도 60 = C4)
    const int octave = (note / 12) - 1;
    std::ostringstream os;
    os << kNames[note % 12] << octave;
    return os.str();
}

std::string MidiMessage::toString() const {
    std::ostringstream os;
    const int displayChannel = m_channel + 1; // 사용자에게는 1~16으로 표시

    switch (m_type) {
    case MessageType::NoteOn:
        os << "Note On   ch=" << displayChannel << " note=" << (int)m_data1 << " ("
           << noteName(m_data1) << ") vel=" << (int)m_data2;
        break;
    case MessageType::NoteOff:
        os << "Note Off  ch=" << displayChannel << " note=" << (int)m_data1 << " ("
           << noteName(m_data1) << ") vel=" << (int)m_data2;
        break;
    case MessageType::ControlChange:
        os << "CC        ch=" << displayChannel << " cc=" << (int)m_data1
           << " value=" << (int)m_data2;
        break;
    case MessageType::ProgramChange:
        os << "Program   ch=" << displayChannel << " program=" << (int)m_data1;
        break;
    case MessageType::PitchBend:
        os << "PitchBend ch=" << displayChannel << " bend=" << m_pitchBend;
        break;
    case MessageType::PolyAftertouch:
        os << "PolyAT    ch=" << displayChannel << " note=" << (int)m_data1
           << " pressure=" << (int)m_data2;
        break;
    case MessageType::ChannelAftertouch:
        os << "ChanAT    ch=" << displayChannel << " pressure=" << (int)m_data1;
        break;
    case MessageType::SysEx:         os << "SysEx (" << m_totalSize << " bytes)"; break;
    case MessageType::Clock:         os << "Clock";         break;
    case MessageType::Start:         os << "Start";         break;
    case MessageType::Continue:      os << "Continue";      break;
    case MessageType::Stop:          os << "Stop";          break;
    case MessageType::ActiveSensing: os << "ActiveSensing"; break;
    case MessageType::SystemReset:   os << "SystemReset";   break;
    default:                         os << "Unknown";       break;
    }

    // 원본 바이트 hex 덤프
    os << "  [";
    for (std::size_t i = 0; i < m_rawSize; ++i) {
        if (i) os << " ";
        os << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)m_raw[i]
           << std::dec << std::setfill(' ');
    }
    if (m_totalSize > m_rawSize) os << " ...";
    os << "]";
    return os.str();
}

} // namespace midipro::midi
