// =============================================================
// MidiPro - midi2/Ump.cpp
// =============================================================

#include "midi2/Ump.h"

namespace midipro::midi2 {

namespace {
uint8_t messageType(uint32_t word0) { return (uint8_t)((word0 >> 28) & 0xF); }

// MIDI 2.0 CV word0 조립: [MT=4][group][status][channel][data16]
uint32_t cv2Word0(uint8_t group, uint8_t status, uint8_t channel, uint16_t data16) {
    return ((uint32_t)kMtMidi2ChannelVoice << 28) | ((uint32_t)(group & 0xF) << 24) |
           ((uint32_t)(status & 0xF) << 20) | ((uint32_t)(channel & 0xF) << 16) | data16;
}
} // namespace

int umpWordCount(uint32_t word0) {
    switch (messageType(word0)) {
    case 0x0: case 0x1: case 0x2: return 1; // Utility / System / MIDI1 CV
    case 0x3: case 0x4: return 2;           // Data(64) / MIDI2 CV
    case 0x5: return 4;                     // Data(128)
    default: return 1;
    }
}

Cv2Message parseMidi2Cv(uint32_t word0, uint32_t word1) {
    Cv2Message m;
    if (messageType(word0) != kMtMidi2ChannelVoice) return m; // Other
    m.group = (uint8_t)((word0 >> 24) & 0xF);
    const uint8_t status = (uint8_t)((word0 >> 20) & 0xF);
    m.channel = (uint8_t)((word0 >> 16) & 0xF);
    const uint8_t byteHi = (uint8_t)((word0 >> 8) & 0xFF); // note / cc index
    m.data32 = word1;

    switch (status) {
    case kCvNoteOn:
        m.type = Cv2Type::NoteOn;
        m.note = byteHi;
        m.velocity16 = (uint16_t)((word1 >> 16) & 0xFFFF);
        break;
    case kCvNoteOff:
        m.type = Cv2Type::NoteOff;
        m.note = byteHi;
        m.velocity16 = (uint16_t)((word1 >> 16) & 0xFFFF);
        break;
    case kCvPerNotePitchBend:
        m.type = Cv2Type::PerNotePitchBend;
        m.note = byteHi;
        break;
    case kCvChannelPitchBend:
        m.type = Cv2Type::ChannelPitchBend;
        break;
    case kCvControlChange:
        m.type = Cv2Type::ControlChange;
        m.index = byteHi;
        break;
    case kCvChannelPressure:
        m.type = Cv2Type::ChannelPressure;
        break;
    case kCvPolyPressure:
        m.type = Cv2Type::PolyPressure;
        m.note = byteHi;
        break;
    default:
        m.type = Cv2Type::Other;
        break;
    }
    return m;
}

UmpMessage makeNoteOn(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16) {
    UmpMessage u;
    u.count = 2;
    // data16 = [note][attributeType=0]
    u.words[0] = cv2Word0(group, kCvNoteOn, channel, (uint16_t)((note & 0x7F) << 8));
    u.words[1] = (uint32_t)velocity16 << 16; // [velocity16][attributeData=0]
    return u;
}

UmpMessage makeNoteOff(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16) {
    UmpMessage u;
    u.count = 2;
    u.words[0] = cv2Word0(group, kCvNoteOff, channel, (uint16_t)((note & 0x7F) << 8));
    u.words[1] = (uint32_t)velocity16 << 16;
    return u;
}

UmpMessage makePerNotePitchBend(uint8_t group, uint8_t channel, uint8_t note, uint32_t value32) {
    UmpMessage u;
    u.count = 2;
    u.words[0] = cv2Word0(group, kCvPerNotePitchBend, channel, (uint16_t)((note & 0x7F) << 8));
    u.words[1] = value32;
    return u;
}

UmpMessage makeChannelPitchBend(uint8_t group, uint8_t channel, uint32_t value32) {
    UmpMessage u;
    u.count = 2;
    u.words[0] = cv2Word0(group, kCvChannelPitchBend, channel, 0);
    u.words[1] = value32;
    return u;
}

UmpMessage makeControlChange(uint8_t group, uint8_t channel, uint8_t index, uint32_t value32) {
    UmpMessage u;
    u.count = 2;
    u.words[0] = cv2Word0(group, kCvControlChange, channel, (uint16_t)((index & 0x7F) << 8));
    u.words[1] = value32;
    return u;
}

UmpMessage makeChannelPressure(uint8_t group, uint8_t channel, uint32_t value32) {
    UmpMessage u;
    u.count = 2;
    u.words[0] = cv2Word0(group, kCvChannelPressure, channel, 0);
    u.words[1] = value32;
    return u;
}

// ---- 스케일 변환 ----
// 7->16비트: 단순 선형 (0->0, 127->65535). MIDI 2.0 스펙의 min-center-max
// 보간과 값이 정확히 같진 않지만 단조·경계 보존이라 실용상 충분하다.
uint16_t vel7to16(uint8_t v7) {
    return (uint16_t)((v7 & 0x7F) * 65535 / 127);
}

float vel16ToFloat(uint16_t v16) { return (float)v16 / 65535.0f; }

float pitch32ToNorm(uint32_t v32) {
    // 0x80000000 = 중앙(0). -1 ~ +1
    const double centered = (double)v32 - 2147483648.0;
    return (float)(centered / 2147483648.0);
}

uint32_t normToPitch32(float norm) {
    double n = norm;
    if (n < -1.0) n = -1.0;
    if (n > 1.0) n = 1.0;
    double v = (n * 0.5 + 0.5) * 4294967295.0;
    if (v < 0.0) v = 0.0;
    if (v > 4294967295.0) v = 4294967295.0;
    return (uint32_t)v;
}

float cc32ToFloat(uint32_t v32) { return (float)((double)v32 / 4294967295.0); }

uint32_t cc7to32(uint8_t v7) {
    return (uint32_t)((double)(v7 & 0x7F) / 127.0 * 4294967295.0);
}

} // namespace midipro::midi2
