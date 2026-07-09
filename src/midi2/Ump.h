#pragma once
// =============================================================
// MidiPro - midi2/Ump.h
// MIDI 2.0 UMP(Universal MIDI Packet) 코덱.
//
// MIDI 2.0의 실질적 알맹이는 UMP 패킷과 MIDI 2.0 채널 보이스
// 메시지다: 16비트 벨로시티, 32비트 컨트롤러, 그리고 채널을 나누지
// 않고도 되는 "노트별 피치벤드/컨트롤러". 이 파일은 그 패킷을
// 만들고 해석하고, MIDI 1.0 7비트 값과 스케일 변환한다.
//
// 순수 로직이라 장치/오디오 없이 단독 테스트한다 (Rule 6). 하드웨어
// UMP 전송(Windows MIDI Services)은 별개 계층으로, 여기선 다루지 않는다.
// =============================================================

#include <cstdint>

namespace midipro::midi2 {

// UMP 메시지 타입(word0 상위 4비트)
inline constexpr uint8_t kMtMidi1ChannelVoice = 0x2; // 32비트(1워드)
inline constexpr uint8_t kMtMidi2ChannelVoice = 0x4; // 64비트(2워드)

// MIDI 2.0 채널 보이스 상태(opcode)
inline constexpr uint8_t kCvNoteOff          = 0x8;
inline constexpr uint8_t kCvNoteOn           = 0x9;
inline constexpr uint8_t kCvPolyPressure     = 0xA;
inline constexpr uint8_t kCvControlChange    = 0xB;
inline constexpr uint8_t kCvChannelPressure  = 0xD;
inline constexpr uint8_t kCvChannelPitchBend = 0xE;
inline constexpr uint8_t kCvPerNotePitchBend = 0x6;

// 만든 UMP 메시지 (최대 2워드까지 사용)
struct UmpMessage {
    uint32_t words[2] = {0, 0};
    int count = 0; // 워드 수
};

// word0로 전체 워드 수를 판정 (MT별 고정 길이)
int umpWordCount(uint32_t word0);

// ---- 파싱 결과 ----
enum class Cv2Type {
    NoteOn, NoteOff, PerNotePitchBend, ChannelPitchBend,
    ControlChange, ChannelPressure, PolyPressure, Other
};

struct Cv2Message {
    Cv2Type type = Cv2Type::Other;
    uint8_t group = 0;
    uint8_t channel = 0;   // 0~15
    uint8_t note = 0;      // 노트/폴리 관련일 때
    uint8_t index = 0;     // CC 번호 등
    uint16_t velocity16 = 0;
    uint32_t data32 = 0;   // 32비트 값 (피치벤드/CC/압력)
};

// MIDI 2.0 채널 보이스(MT=4) 2워드를 해석. MT가 다르면 type=Other.
Cv2Message parseMidi2Cv(uint32_t word0, uint32_t word1);

// ---- 빌더 (MIDI 2.0 채널 보이스) ----
UmpMessage makeNoteOn(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16);
UmpMessage makeNoteOff(uint8_t group, uint8_t channel, uint8_t note, uint16_t velocity16);
UmpMessage makePerNotePitchBend(uint8_t group, uint8_t channel, uint8_t note, uint32_t value32);
UmpMessage makeChannelPitchBend(uint8_t group, uint8_t channel, uint32_t value32);
UmpMessage makeControlChange(uint8_t group, uint8_t channel, uint8_t index, uint32_t value32);
UmpMessage makeChannelPressure(uint8_t group, uint8_t channel, uint32_t value32);

// ---- 스케일 변환 ----
uint16_t vel7to16(uint8_t v7);           // MIDI1.0 벨로시티 -> 16비트
float vel16ToFloat(uint16_t v16);        // 0~1
float pitch32ToNorm(uint32_t v32);       // -1~1 (0x80000000=중앙)
uint32_t normToPitch32(float norm);      // -1~1 -> 32비트
float cc32ToFloat(uint32_t v32);         // 0~1
uint32_t cc7to32(uint8_t v7);            // MIDI1.0 CC -> 32비트

} // namespace midipro::midi2
