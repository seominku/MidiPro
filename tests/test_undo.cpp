// =============================================================
// MidiPro - tests/test_undo.cpp
// 실행취소/다시실행 히스토리 순수 로직 테스트 (Rule 6).
// =============================================================

#include "core/UndoHistory.h"
#include "sequencer/Song.h"
#include "sequencer/Track.h"

#include <iostream>
#include <string>

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

void testBasicUndoRedo() {
    core::UndoHistory<int> h;
    int value = 1;

    CHECK(!h.canUndo());
    CHECK(!h.canRedo());
    CHECK(!h.undo(value)); // 빈 히스토리는 실패

    h.record(value); // 복원점: 1
    value = 2;
    h.record(value); // 복원점: 2
    value = 3;

    CHECK(h.undo(value)); // 3 -> 2
    CHECK(value == 2);
    CHECK(h.undo(value)); // 2 -> 1
    CHECK(value == 1);
    CHECK(!h.undo(value)); // 더 없음
    CHECK(value == 1);

    CHECK(h.redo(value)); // 1 -> 2
    CHECK(value == 2);
    CHECK(h.redo(value)); // 2 -> 3
    CHECK(value == 3);
    CHECK(!h.redo(value));
}

void testNewEditClearsRedo() {
    core::UndoHistory<int> h;
    int value = 10;
    h.record(value);
    value = 20;
    CHECK(h.undo(value)); // 20 -> 10
    CHECK(value == 10);
    CHECK(h.canRedo());

    // 되돌린 뒤 새 편집을 하면 redo 스택은 무효
    h.record(value);
    value = 99;
    CHECK(!h.canRedo());
}

void testMaxDepthEvictsOldest() {
    core::UndoHistory<int> h(3); // 최대 3개만 보관
    for (int i = 0; i < 10; ++i) h.record(i);
    CHECK(h.undoCount() == 3); // 오래된 것은 버려짐
}

void testWithSong() {
    // 실제 사용처: Song 스냅샷 되돌리기
    seq::Song song;
    core::UndoHistory<seq::Song> h;

    h.record(song); // 빈 곡
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    song.tracks.push_back(t);
    CHECK(song.tracks.size() == 1);

    CHECK(h.undo(song));
    CHECK(song.tracks.empty()); // 빈 곡으로 복원

    CHECK(h.redo(song));
    CHECK(song.tracks.size() == 1); // 다시 트랙 하나
    CHECK(seq::extractNotes(song.tracks[0]).size() == 1);
}

} // namespace

int main() {
    testBasicUndoRedo();
    testNewEditClearsRedo();
    testMaxDepthEvictsOldest();
    testWithSong();

    if (g_failures == 0) {
        std::cout << "[OK] undo tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
