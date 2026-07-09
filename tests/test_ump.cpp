// =============================================================
// MidiPro - tests/test_ump.cpp
// MIDI 2.0 UMP 코덱 순수 로직 테스트 (Rule 6):
//   빌드/파싱 왕복, 스케일 변환, 메시지 길이 판정.
// =============================================================

#include "midi2/Ump.h"

#include <cmath>
#include <iostream>

using namespace midipro::midi2;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

void testWordCount() {
    // MIDI 2.0 CV(MT=4)는 2워드
    auto on = makeNoteOn(0, 0, 60, 0x8000);
    CHECK(on.count == 2);
    CHECK(umpWordCount(on.words[0]) == 2);
}

void testNoteRoundTrip() {
    auto on = makeNoteOn(1, 5, 64, 40000);
    auto m = parseMidi2Cv(on.words[0], on.words[1]);
    CHECK(m.type == Cv2Type::NoteOn);
    CHECK(m.group == 1);
    CHECK(m.channel == 5);
    CHECK(m.note == 64);
    CHECK(m.velocity16 == 40000);

    auto off = makeNoteOff(0, 2, 60, 0);
    auto mo = parseMidi2Cv(off.words[0], off.words[1]);
    CHECK(mo.type == Cv2Type::NoteOff);
    CHECK(mo.channel == 2 && mo.note == 60);
}

void testPerNotePitchBend() {
    // 노트별 피치벤드: 같은 채널, 다른 노트
    auto pb = makePerNotePitchBend(0, 0, 67, normToPitch32(0.5f));
    auto m = parseMidi2Cv(pb.words[0], pb.words[1]);
    CHECK(m.type == Cv2Type::PerNotePitchBend);
    CHECK(m.channel == 0 && m.note == 67);
    CHECK(std::fabs(pitch32ToNorm(m.data32) - 0.5f) < 1e-3);

    // 중앙값
    CHECK(std::fabs(pitch32ToNorm(0x80000000u)) < 1e-6);
    // 왕복
    CHECK(std::fabs(pitch32ToNorm(normToPitch32(-0.75f)) + 0.75f) < 1e-3);
}

void testControlAndPressure() {
    auto cc = makeControlChange(0, 3, 74, cc7to32(64));
    auto m = parseMidi2Cv(cc.words[0], cc.words[1]);
    CHECK(m.type == Cv2Type::ControlChange);
    CHECK(m.index == 74);
    CHECK(std::fabs(cc32ToFloat(m.data32) - (64.0f / 127.0f)) < 1e-3);

    auto cp = makeChannelPressure(0, 0, cc7to32(127));
    auto mp = parseMidi2Cv(cp.words[0], cp.words[1]);
    CHECK(mp.type == Cv2Type::ChannelPressure);
    CHECK(std::fabs(cc32ToFloat(mp.data32) - 1.0f) < 1e-3);
}

void testScaling() {
    CHECK(vel7to16(0) == 0);
    CHECK(vel7to16(127) == 65535);
    CHECK(std::fabs(vel16ToFloat(65535) - 1.0f) < 1e-6);
    CHECK(std::fabs(vel16ToFloat(0)) < 1e-6);
    CHECK(cc7to32(0) == 0);
    CHECK(std::fabs(cc32ToFloat(cc7to32(127)) - 1.0f) < 1e-3);
}

} // namespace

int main() {
    testWordCount();
    testNoteRoundTrip();
    testPerNotePitchBend();
    testControlAndPressure();
    testScaling();

    if (g_failures == 0) {
        std::cout << "[OK] ump tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
