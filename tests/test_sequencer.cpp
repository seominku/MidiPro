// =============================================================
// MidiPro - tests/test_sequencer.cpp
// 시퀀서 순수 로직 유닛 테스트 (Rule 6):
//   타이밍 변환, 노트 추출, VLQ 인코딩, SMF 저장/불러오기 왕복.
// 하드웨어(장치)나 GUI 없이 검증한다.
// =============================================================

#include "sequencer/SmfFile.h"
#include "sequencer/Song.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"
#include "guitar/Fretboard.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>

using namespace midipro;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

bool approx(double a, double b) { return std::fabs(a - b) < 1e-6; }

void testTimeBase() {
    // 120 BPM, 480 PPQN -> 초당 960틱
    CHECK(approx(seq::ticksPerSecond(120.0, 480), 960.0));
    // 4분음표(480틱) = 0.5초 @120BPM
    CHECK(approx(seq::ticksToSeconds(480, 120.0, 480), 0.5));
    CHECK(approx(seq::secondsToTicks(0.5, 120.0, 480), 480.0));

    // 마디:박:틱 (4/4, 480 PPQN)
    auto p = seq::toBarBeatTick(0, 480);
    CHECK(p.bar == 1 && p.beat == 1 && p.tick == 0);
    p = seq::toBarBeatTick(480, 480); // 2박째
    CHECK(p.bar == 1 && p.beat == 2 && p.tick == 0);
    p = seq::toBarBeatTick(480 * 4, 480); // 2마디째
    CHECK(p.bar == 2 && p.beat == 1 && p.tick == 0);
}

void testNoteExtraction() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    t.addNote(480, 240, 64, 90);
    t.sortEvents();

    const auto notes = seq::extractNotes(t);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60 && notes[0].startTick == 0 && notes[0].endTick == 480);
    CHECK(notes[1].note == 64 && notes[1].startTick == 480 && notes[1].endTick == 720);
    CHECK(t.lengthTicks() == 720);
}

void testNoteEditing() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    t.addNote(480, 480, 64, 100);
    t.addNote(480, 240, 67, 90); // 64와 같은 시작, 다른 음
    t.sortEvents();
    CHECK(seq::extractNotes(t).size() == 3);

    // (60, tick 100)을 포함하는 노트를 찾아 삭제
    bool found = false;
    const seq::NoteSpan hit = seq::noteSpanAt(t, 60, 100, found);
    CHECK(found);
    CHECK(hit.startTick == 0 && hit.endTick == 480);
    CHECK(seq::removeNote(t, hit));
    CHECK(seq::extractNotes(t).size() == 2);

    // 삭제된 자리에는 더 이상 노트가 없다
    bool found2 = false;
    seq::noteSpanAt(t, 60, 100, found2);
    CHECK(!found2);

    // 같은 시작 틱의 다른 음(67)은 그대로 남아 있다
    bool found3 = false;
    seq::noteSpanAt(t, 67, 500, found3);
    CHECK(found3);
}

void testVlq() {
    struct Case {
        uint32_t value;
        std::size_t bytes;
    };
    const Case cases[] = {{0, 1}, {127, 1}, {128, 2}, {8192, 2}, {0x0FFFFFFF, 4}};
    for (const auto& c : cases) {
        std::vector<uint8_t> buf;
        seq::smf::writeVlq(buf, c.value);
        CHECK(buf.size() == c.bytes);
        uint32_t decoded = 0;
        const std::size_t consumed = seq::smf::readVlq(buf.data(), buf.size(), 0, decoded);
        CHECK(consumed == c.bytes);
        CHECK(decoded == c.value);
    }
}

void testSmfRoundTrip() {
    seq::Song song;
    song.bpm = 100.0;
    song.ppqn = 480;

    seq::Track t1;
    t1.name = "Lead";
    t1.channel = 0;
    t1.addProgramChange(0, guitar::kStringCount); // 임의 프로그램
    t1.addNote(0, 480, 60, 100);
    t1.addNote(480, 480, 67, 90);
    t1.sortEvents();
    song.tracks.push_back(t1);

    seq::Track t2;
    t2.name = "Bass";
    t2.channel = 1;
    t2.addNote(0, 960, 40, 110);
    t2.sortEvents();
    song.tracks.push_back(t2);

    const auto path =
        std::filesystem::temp_directory_path() / "midipro_roundtrip_test.mid";
    CHECK(seq::smf::save(song, path));

    seq::Song loaded;
    CHECK(seq::smf::load(loaded, path));
    CHECK(loaded.ppqn == 480);
    CHECK(std::fabs(loaded.bpm - 100.0) < 0.5); // 템포는 마이크로초 반올림 오차 허용
    CHECK(loaded.tracks.size() == 2);

    // 첫 트랙의 노트가 보존됐는지 (Program Change 포함)
    const auto notes = seq::extractNotes(loaded.tracks[0]);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60);
    CHECK(notes[1].note == 67);
    CHECK(loaded.tracks[0].name == "Lead");
    CHECK(loaded.tracks[1].name == "Bass");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void testFretboard() {
    // 6번줄(인덱스0) 개방 = E2(40), 5프렛 = A2(45)
    CHECK(guitar::noteAt(0, 0) == 40);
    CHECK(guitar::noteAt(0, 5) == 45);
    // 1번줄(인덱스5) 개방 = E4(64)
    CHECK(guitar::noteAt(5, 0) == 64);

    // A2(45)는 6번줄 5프렛과 5번줄 0프렛에서 난다
    const auto pos = guitar::positionsForNote(45);
    bool found6th5 = false, found5th0 = false;
    for (const auto& p : pos) {
        if (p.stringIndex == 0 && p.fret == 5) found6th5 = true;
        if (p.stringIndex == 1 && p.fret == 0) found5th0 = true;
    }
    CHECK(found6th5 && found5th0);

    // 코드 공식이 비어있지 않다
    CHECK(!guitar::commonChords().empty());
    CHECK(std::string(guitar::pitchClassName(0)) == "C");
    CHECK(std::string(guitar::pitchClassName(9)) == "A");
}

} // namespace

int main() {
    testTimeBase();
    testNoteExtraction();
    testNoteEditing();
    testVlq();
    testSmfRoundTrip();
    testFretboard();

    if (g_failures == 0) {
        std::cout << "[OK] sequencer tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
