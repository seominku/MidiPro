// =============================================================
// MidiPro - tests/test_midi_message.cpp
// MidiMessage 파서/생성기 유닛 테스트 (Rule 6).
//
// 왜 프레임워크 없이 작성했는가:
//   Phase 1에서는 검증 대상이 순수 함수뿐이라 CHECK 매크로면
//   충분하다. 테스트가 커지면 Catch2/GoogleTest로 이전한다.
//   하드웨어(장치)가 필요 없는 순수 로직만 테스트한다.
// =============================================================

#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"

#include <iostream>
#include <vector>

using namespace midipro::midi;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

void testNoteOnParsing() {
    const auto m = MidiMessage::parse({0x90, 60, 100});
    CHECK(m.type() == MessageType::NoteOn);
    CHECK(m.channel() == 0);
    CHECK(m.data1() == 60);
    CHECK(m.data2() == 100);
}

void testNoteOnVelocityZeroIsNoteOff() {
    // MIDI 관례: velocity 0인 Note On은 Note Off로 취급
    const auto m = MidiMessage::parse({0x90, 60, 0});
    CHECK(m.type() == MessageType::NoteOff);
}

void testNoteOffParsing() {
    const auto m = MidiMessage::parse({0x85, 40, 64}); // 채널 6 (0-based 5)
    CHECK(m.type() == MessageType::NoteOff);
    CHECK(m.channel() == 5);
    CHECK(m.data1() == 40);
}

void testControlChangeParsing() {
    const auto m = MidiMessage::parse({0xB0, kCcVolume, 127});
    CHECK(m.type() == MessageType::ControlChange);
    CHECK(m.data1() == kCcVolume);
    CHECK(m.data2() == 127);
}

void testProgramChangeParsing() {
    const auto m = MidiMessage::parse({0xC1, kGmDistortionGuitar});
    CHECK(m.type() == MessageType::ProgramChange);
    CHECK(m.channel() == 1);
    CHECK(m.data1() == kGmDistortionGuitar);
}

void testPitchBendParsing() {
    // 중앙 (8192) -> bend 0
    const auto center = MidiMessage::parse({0xE0, 0x00, 0x40});
    CHECK(center.type() == MessageType::PitchBend);
    CHECK(center.pitchBend() == 0);

    // 최소 (0) -> -8192
    const auto min = MidiMessage::parse({0xE0, 0x00, 0x00});
    CHECK(min.pitchBend() == kPitchBendMin);

    // 최대 (16383) -> +8191
    const auto max = MidiMessage::parse({0xE0, 0x7F, 0x7F});
    CHECK(max.pitchBend() == kPitchBendMax);
}

void testSystemMessages() {
    CHECK(MidiMessage::parse({0xF8}).type() == MessageType::Clock);
    CHECK(MidiMessage::parse({0xFA}).type() == MessageType::Start);
    CHECK(MidiMessage::parse({0xFC}).type() == MessageType::Stop);
    CHECK(MidiMessage::parse({0xFF}).type() == MessageType::SystemReset);
}

void testEmptyAndInvalid() {
    const auto empty = MidiMessage::parse(nullptr, 0);
    CHECK(empty.type() == MessageType::Unknown);
    CHECK(empty.rawSize() == 0);
}

void testSysExTruncation() {
    // kMaxRawBytes보다 긴 SysEx: 앞부분만 보관, 전체 길이는 유지
    std::vector<uint8_t> sysex(100, 0x00);
    sysex[0] = 0xF0;
    sysex[99] = 0xF7;
    const auto m = MidiMessage::parse(sysex);
    CHECK(m.type() == MessageType::SysEx);
    CHECK(m.rawSize() == MidiMessage::kMaxRawBytes);
    CHECK(m.totalSize() == 100);
}

void testGeneratorRoundTrip() {
    // 생성 -> 파싱했을 때 원래 값이 복원되는지 (왕복 검증)
    const auto on = MidiMessage::parse(MidiMessage::makeNoteOn(3, 64, 90));
    CHECK(on.type() == MessageType::NoteOn);
    CHECK(on.channel() == 3);
    CHECK(on.data1() == 64);
    CHECK(on.data2() == 90);

    const auto off = MidiMessage::parse(MidiMessage::makeNoteOff(3, 64));
    CHECK(off.type() == MessageType::NoteOff);

    const auto cc = MidiMessage::parse(MidiMessage::makeControlChange(0, kCcPan, 32));
    CHECK(cc.type() == MessageType::ControlChange);
    CHECK(cc.data1() == kCcPan);
    CHECK(cc.data2() == 32);

    const auto pc = MidiMessage::parse(MidiMessage::makeProgramChange(9, kGmNylonGuitar));
    CHECK(pc.type() == MessageType::ProgramChange);
    CHECK(pc.channel() == 9);
    CHECK(pc.data1() == kGmNylonGuitar);

    const auto bendUp = MidiMessage::parse(MidiMessage::makePitchBend(0, 1000));
    CHECK(bendUp.type() == MessageType::PitchBend);
    CHECK(bendUp.pitchBend() == 1000);

    const auto bendClamped = MidiMessage::parse(MidiMessage::makePitchBend(0, 99999));
    CHECK(bendClamped.pitchBend() == kPitchBendMax);
}

void testNoteName() {
    CHECK(MidiMessage::noteName(60) == "C4");  // 가운데 도
    CHECK(MidiMessage::noteName(kNoteE2) == "E2"); // 기타 6번줄
    CHECK(MidiMessage::noteName(kNoteA2) == "A2");
    CHECK(MidiMessage::noteName(kNoteE4) == "E4"); // 기타 1번줄
    CHECK(MidiMessage::noteName(0) == "C-1");
    CHECK(MidiMessage::noteName(127) == "G9");
    CHECK(MidiMessage::noteName(61) == "C#4");
}

} // namespace

int main() {
    testNoteOnParsing();
    testNoteOnVelocityZeroIsNoteOff();
    testNoteOffParsing();
    testControlChangeParsing();
    testProgramChangeParsing();
    testPitchBendParsing();
    testSystemMessages();
    testEmptyAndInvalid();
    testSysExTruncation();
    testGeneratorRoundTrip();
    testNoteName();

    if (g_failures == 0) {
        std::cout << "[OK] all tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
