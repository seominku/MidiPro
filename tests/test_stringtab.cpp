// =============================================================
// MidiPro - tests/test_stringtab.cpp
// 줄·프렛 <-> 노트 변환 (gui/StringTab.h)
// =============================================================

#include "gui/StringTab.h"

#include <cstdio>

using namespace midipro::gui;

static int g_fail = 0;
static void expect(bool c, const char* what) {
    if (!c) { std::printf("[FAIL] %s\n", what); ++g_fail; }
}

int main() {
    // ---- 튜닝 기본 ----
    expect(tuningInfo(TabTuning::Guitar6).stringCount == 6, "기타는 6줄");
    expect(tuningInfo(TabTuning::Bass4).stringCount == 4, "베이스는 4줄");
    expect(tuningInfo(TabTuning::None).stringCount == 0, "줄 없음은 0");

    // 개방현 (앱 기준 C4=60)
    expect(tabNoteAt(TabTuning::Guitar6, 0, 0) == 40, "기타 6번줄 개방 = E2(40)");
    expect(tabNoteAt(TabTuning::Guitar6, 5, 0) == 64, "기타 1번줄 개방 = E4(64)");
    expect(tabNoteAt(TabTuning::Bass4, 0, 0) == 28, "베이스 4번줄 개방 = E1(28)");
    expect(tabNoteAt(TabTuning::Bass4, 3, 0) == 43, "베이스 1번줄 개방 = G2(43)");

    // 프렛
    expect(tabNoteAt(TabTuning::Guitar6, 0, 5) == 45, "6번줄 5프렛 = A2");
    expect(tabNoteAt(TabTuning::Guitar6, 0, 12) == 52, "6번줄 12프렛 = 한 옥타브 위");
    expect(tabNoteAt(TabTuning::Guitar6, 0, -1) == -1, "음수 프렛 거부");
    expect(tabNoteAt(TabTuning::Guitar6, 0, 25) == -1, "24프렛 초과 거부");
    expect(tabNoteAt(TabTuning::Guitar6, 6, 0) == -1, "없는 줄 거부");
    expect(tabNoteAt(TabTuning::Bass4, 4, 0) == -1, "베이스 5번줄은 없다");

    // ---- 음역 ----
    expect(tabLowestNote(TabTuning::Guitar6) == 40, "기타 최저 E2");
    expect(tabHighestNote(TabTuning::Guitar6) == 88, "기타 최고 = 1번줄 24프렛");
    expect(tabLowestNote(TabTuning::Bass4) == 28, "베이스 최저 E1");
    expect(tabHighestNote(TabTuning::Bass4) == 67, "베이스 최고 = 1번줄 24프렛");
    expect(!tabPlayable(TabTuning::Guitar6, 36), "기타로 C2(36)는 못 낸다");
    expect(tabPlayable(TabTuning::Guitar6, 40), "기타로 E2는 낸다");
    expect(!tabPlayable(TabTuning::Bass4, 27), "베이스로 27은 못 낸다");
    expect(tabPlayable(TabTuning::Bass4, 28), "베이스로 E1은 낸다");
    expect(tabPlayable(TabTuning::None, 0) && tabPlayable(TabTuning::None, 127),
           "줄 없음은 제한이 없다");

    // ---- 배정: 로우 포지션 우선 ----
    int s = -1, f = -1;
    expect(tabAssign(TabTuning::Guitar6, 40, s, f) && s == 0 && f == 0, "E2 = 6번줄 개방");
    expect(tabAssign(TabTuning::Guitar6, 64, s, f) && s == 5 && f == 0, "E4 = 1번줄 개방");
    // A2(45)는 6번줄 5프렛 / 5번줄 개방 -> 프렛이 낮은 5번줄 개방을 고른다
    expect(tabAssign(TabTuning::Guitar6, 45, s, f) && s == 1 && f == 0, "A2는 5번줄 개방");
    // C3(48)은 5번줄 3프렛이 가장 낮은 자리
    expect(tabAssign(TabTuning::Guitar6, 48, s, f) && s == 1 && f == 3, "C3은 5번줄 3프렛");
    expect(!tabAssign(TabTuning::Guitar6, 36, s, f), "못 내는 음은 배정 실패");
    expect(tabAssign(TabTuning::Bass4, 38, s, f) && s == 2 && f == 0, "베이스 D2는 2번줄 개방");
    expect(tabAssign(TabTuning::Bass4, 36, s, f) && s == 1 && f == 3, "베이스 C2는 A줄 3프렛");
    expect(!tabAssign(TabTuning::None, 60, s, f), "줄 없음은 배정하지 않는다");

    // 배정한 자리가 실제로 그 음을 내는지 (왕복)
    for (int n = tabLowestNote(TabTuning::Guitar6); n <= tabHighestNote(TabTuning::Guitar6); ++n) {
        int ss = -1, ff = -1;
        if (!tabAssign(TabTuning::Guitar6, n, ss, ff)) { expect(false, "기타 전 음역 배정"); break; }
        if (tabNoteAt(TabTuning::Guitar6, ss, ff) != n) { expect(false, "기타 배정 왕복"); break; }
    }
    for (int n = tabLowestNote(TabTuning::Bass4); n <= tabHighestNote(TabTuning::Bass4); ++n) {
        int ss = -1, ff = -1;
        if (!tabAssign(TabTuning::Bass4, n, ss, ff)) { expect(false, "베이스 전 음역 배정"); break; }
        if (tabNoteAt(TabTuning::Bass4, ss, ff) != n) { expect(false, "베이스 배정 왕복"); break; }
    }

    // ---- 힌트로 줄 고정 ----
    // 힌트 없으면 로우 포지션 (A2=45는 5번줄 개방)
    expect(tabPlaceWithHint(TabTuning::Guitar6, 45, -1, s, f) && s == 1 && f == 0,
           "힌트 없으면 로우 포지션");
    // 힌트가 6번줄(위=0 순서로 5)이면 6번줄 5프렛에 머문다
    expect(tabPlaceWithHint(TabTuning::Guitar6, 45, 5, s, f) && s == 0 && f == 5,
           "힌트가 있으면 그 줄에 머문다");
    // 프렛을 올려도 같은 줄: 6번줄에서 45->46->47 이 계속 6번줄
    for (int n = 40; n <= 64; ++n) {
        if (!tabPlaceWithHint(TabTuning::Guitar6, n, 5, s, f)) { expect(false, "힌트 배정"); break; }
        if (s != 0) { expect(false, "힌트를 준 줄에서 벗어나지 않는다"); break; }
    }
    // 그 줄로 못 내는 음이면 힌트를 무시하고 자동 배정 (6번줄 24프렛 초과)
    expect(tabPlaceWithHint(TabTuning::Guitar6, 70, 5, s, f) && s != 0,
           "힌트로 못 내는 음은 자동 배정");
    // 잘못된 힌트 번호는 무시
    expect(tabPlaceWithHint(TabTuning::Guitar6, 45, 99, s, f) && s == 1 && f == 0,
           "범위 밖 힌트는 무시");
    // 베이스도 같다 (4줄: 위=0이 G2)
    expect(tabPlaceWithHint(TabTuning::Bass4, 43, 3, s, f) && s == 0 && f == 15,
           "베이스: 힌트가 4번줄이면 그 줄 15프렛");
    expect(tabPlaceWithHint(TabTuning::Bass4, 43, -1, s, f) && s == 3 && f == 0,
           "베이스: 힌트 없으면 1번줄 개방");

    // ---- 옥타브로 끌어오기 ----
    int out = -1;
    expect(tabFitOctave(TabTuning::Guitar6, 36, out) && out == 48, "기타: C2 -> C3");
    expect(tabFitOctave(TabTuning::Guitar6, 24, out) && out == 48, "기타: C1 -> C3 (두 옥타브)");
    expect(tabFitOctave(TabTuning::Guitar6, 50, out) && out == 50, "이미 되는 음은 그대로");
    expect(tabFitOctave(TabTuning::Guitar6, 100, out) && out == 88, "너무 높으면 내린다");
    expect(tabFitOctave(TabTuning::Bass4, 21, out) && out == 33, "베이스: A0 -> A1");
    expect(tabFitOctave(TabTuning::None, 5, out) && out == 5, "줄 없음은 손대지 않는다");
    // 끌어온 값은 반드시 연주 가능해야 한다
    for (int n = 0; n <= 127; ++n) {
        int o = -1;
        if (tabFitOctave(TabTuning::Guitar6, n, o))
            if (!tabPlayable(TabTuning::Guitar6, o)) { expect(false, "끌어온 음이 음역 안"); break; }
    }

    if (g_fail) { std::printf("[FAIL] string tab tests failed (%d)\n", g_fail); return 1; }
    std::printf("[OK] string tab tests passed\n");
    return 0;
}
