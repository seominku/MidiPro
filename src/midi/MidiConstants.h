#pragma once
// =============================================================
// MidiPro - midi/MidiConstants.h
// MIDI 프로토콜 상수 정의.
//
// 왜 상수로 두는가 (Rule 5):
//   MIDI는 0x90, 0xB0 같은 매직 넘버투성이 도메인이라
//   숫자를 그대로 쓰면 코드를 읽을 수 없다. 모든 상태 바이트와
//   자주 쓰는 CC/GM 번호에 이름을 붙여 여기서만 정의한다.
// =============================================================

#include <cstdint>

namespace midipro::midi {

// ---- 채널 메시지 상태 바이트 (상위 니블) ----
inline constexpr uint8_t kStatusNoteOff           = 0x80;
inline constexpr uint8_t kStatusNoteOn            = 0x90;
inline constexpr uint8_t kStatusPolyAftertouch    = 0xA0;
inline constexpr uint8_t kStatusControlChange     = 0xB0;
inline constexpr uint8_t kStatusProgramChange     = 0xC0;
inline constexpr uint8_t kStatusChannelAftertouch = 0xD0;
inline constexpr uint8_t kStatusPitchBend         = 0xE0;

// ---- 시스템 메시지 상태 바이트 ----
inline constexpr uint8_t kStatusSysExStart    = 0xF0;
inline constexpr uint8_t kStatusClock         = 0xF8;
inline constexpr uint8_t kStatusStart         = 0xFA;
inline constexpr uint8_t kStatusContinue      = 0xFB;
inline constexpr uint8_t kStatusStop          = 0xFC;
inline constexpr uint8_t kStatusActiveSensing = 0xFE;
inline constexpr uint8_t kStatusSystemReset   = 0xFF;

// ---- 비트 마스크 ----
inline constexpr uint8_t kChannelMask = 0x0F; // 상태 바이트의 하위 니블 = 채널
inline constexpr uint8_t kStatusMask  = 0xF0; // 상위 니블 = 메시지 종류
inline constexpr uint8_t kDataMask    = 0x7F; // 데이터 바이트는 7비트

// ---- 피치 벤드 ----
// 14비트 값(0~16383)에서 8192가 벤드 없음(중앙)
inline constexpr int kPitchBendCenter = 8192;
inline constexpr int kPitchBendMin    = -8192;
inline constexpr int kPitchBendMax    = 8191;

// ---- 자주 쓰는 컨트롤 체인지(CC) 번호 ----
inline constexpr uint8_t kCcModWheel   = 1;
inline constexpr uint8_t kCcVolume     = 7;
inline constexpr uint8_t kCcPan        = 10;
inline constexpr uint8_t kCcExpression = 11;
inline constexpr uint8_t kCcSustain    = 64;

// ---- GM(General MIDI) 프로그램 번호 (기타 계열 위주) ----
inline constexpr uint8_t kGmAcousticGrandPiano = 0;
inline constexpr uint8_t kGmNylonGuitar        = 24;
inline constexpr uint8_t kGmSteelGuitar        = 25;
inline constexpr uint8_t kGmJazzGuitar         = 26;
inline constexpr uint8_t kGmCleanGuitar        = 27;
inline constexpr uint8_t kGmMutedGuitar        = 28;
inline constexpr uint8_t kGmOverdrivenGuitar   = 29;
inline constexpr uint8_t kGmDistortionGuitar   = 30;

// ---- 노트 번호 (기타 표준 튜닝) ----
inline constexpr uint8_t kNoteE2 = 40; // 6번줄
inline constexpr uint8_t kNoteA2 = 45; // 5번줄
inline constexpr uint8_t kNoteD3 = 50; // 4번줄
inline constexpr uint8_t kNoteG3 = 55; // 3번줄
inline constexpr uint8_t kNoteB3 = 59; // 2번줄
inline constexpr uint8_t kNoteE3 = 52;
inline constexpr uint8_t kNoteE4 = 64; // 1번줄
inline constexpr uint8_t kNoteC4 = 60; // 가운데 도

} // namespace midipro::midi
