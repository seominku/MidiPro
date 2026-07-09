#pragma once
// =============================================================
// MidiPro - audio/IMidi2Input.h
// MIDI 2.0(UMP) 입력 진입점 (GUI가 의존하는 얇은 경계, Rule 1).
//
// send()가 MIDI 1.0 바이트를 받는 것처럼, 이건 UMP 워드를 받아
// 내부에서 해석해 신스로 보낸다. 하드웨어 MIDI 2.0 전송(Windows
// MIDI Services)이 붙으면 그 계층이 이 메서드를 호출하면 된다.
// =============================================================

#include <cstdint>

namespace midipro::audio {

class IMidi2Input {
public:
    virtual ~IMidi2Input() = default;
    // UMP 워드 배열(1~2워드 메시지)을 전달한다. 처리했으면 true.
    virtual bool sendUmp(const uint32_t* words, int count) = 0;
};

} // namespace midipro::audio
