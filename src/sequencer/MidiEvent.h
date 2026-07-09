#pragma once
// =============================================================
// MidiPro - sequencer/MidiEvent.h
// 트랙에 저장되는 시퀀서 이벤트.
//
// 왜 접근자 없는 공개 구조체(POD)인가 (스타일 예외):
//   재생/파일 입출력 핫 패스에서 수천 개씩 복사·정렬되는
//   순수 값 데이터라서, 불변식이 없는 필드에 접근자를 씌우면
//   소음만 늘어난다. 로직을 가진 클래스는 m_ 규칙을 따르되
//   이런 값 구조체는 공개 필드를 허용한다.
// =============================================================

#include "midi/MidiConstants.h"

#include <cstdint>

namespace midipro::seq {

struct MidiEvent {
    uint32_t tick = 0;  // 곡 시작부터의 절대 틱
    uint8_t status = 0; // 채널 포함 상태 바이트 (예: 0x90 | ch)
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    uint8_t channel() const { return status & midi::kChannelMask; }
    uint8_t kind() const { return status & midi::kStatusMask; }

    // velocity 0인 Note On은 Note Off로 취급 (MIDI 관례)
    bool isNoteOn() const { return kind() == midi::kStatusNoteOn && data2 > 0; }
    bool isNoteOff() const {
        return kind() == midi::kStatusNoteOff ||
               (kind() == midi::kStatusNoteOn && data2 == 0);
    }

    // Program Change / Channel Aftertouch만 2바이트, 나머지는 3바이트
    bool isThreeBytes() const {
        return kind() != midi::kStatusProgramChange &&
               kind() != midi::kStatusChannelAftertouch;
    }
};

} // namespace midipro::seq
