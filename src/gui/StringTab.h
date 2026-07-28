#pragma once
// =============================================================
// MidiPro - gui/StringTab.h
// 줄 있는 악기(기타 6줄 / 베이스 4줄)의 줄·프렛 <-> MIDI 노트 변환.
//
// 왜 필요한가:
//   샘플 기반 기타/베이스 악기는 진짜 악기의 음역만 녹음돼 있다. 피아노 롤에서
//   자유롭게 찍으면 연주 불가능한 음이 섞이고, 그런 음은 아무 소리도 나지 않는다
//   (Ample Guitar LP에 C2를 찍으면 무음). 줄 위에서만 찍게 하면 음역을 벗어날
//   수가 없다.
//
// 왜 헤더로 빼나 (Rule 1, 6):
//   순수 계산이라 GUI 없이 단위 테스트한다. 타브 창과 노트 생성 로직이 공유한다.
//
// 음이름 기준은 앱 전체와 같다 (C4 = 60). 기타 6번줄 개방현 = E2(40),
// 베이스 4번줄 개방현 = E1(28).
// =============================================================

#include <cstddef>
#include <cstdint>

namespace midipro::gui {

enum class TabTuning : int {
    None = 0,     // 줄 악기가 아님 (피아노 롤로 편집)
    Guitar6 = 1,  // 표준 EADGBE
    Bass4 = 2,    // 표준 EADG
};

struct TuningInfo {
    int stringCount;
    const int* openNotes;   // [0] = 가장 낮은 줄(기타 6번줄)
    const char* const* names; // 같은 순서의 표시 이름
    int maxFret;
};

inline const TuningInfo& tuningInfo(TabTuning t) {
    // 기타: E2 A2 D3 G3 B3 E4
    static const int kGuitar[6] = {40, 45, 50, 55, 59, 64};
    static const char* const kGuitarNames[6] = {"E2", "A2", "D3", "G3", "B3", "E4"};
    // 베이스: E1 A1 D2 G2
    static const int kBass[4] = {28, 33, 38, 43};
    static const char* const kBassNames[4] = {"E1", "A1", "D2", "G2"};
    static const TuningInfo kNone{0, nullptr, nullptr, 0};
    static const TuningInfo kG{6, kGuitar, kGuitarNames, 24};
    static const TuningInfo kB{4, kBass, kBassNames, 24};
    switch (t) {
    case TabTuning::Guitar6: return kG;
    case TabTuning::Bass4: return kB;
    default: return kNone;
    }
}

inline const char* tuningLabel(TabTuning t) {
    switch (t) {
    case TabTuning::Guitar6: return "기타 6줄 (EADGBE)";
    case TabTuning::Bass4: return "베이스 4줄 (EADG)";
    default: return "줄 없음 (피아노 롤)";
    }
}

// 그 줄/프렛이 내는 음. 범위를 벗어나면 -1.
inline int tabNoteAt(TabTuning t, int stringIdx, int fret) {
    const TuningInfo& ti = tuningInfo(t);
    if (stringIdx < 0 || stringIdx >= ti.stringCount) return -1;
    if (fret < 0 || fret > ti.maxFret) return -1;
    const int n = ti.openNotes[stringIdx] + fret;
    return n <= 127 ? n : -1;
}

// 이 튜닝으로 낼 수 있는 가장 낮은/높은 음
inline int tabLowestNote(TabTuning t) {
    const TuningInfo& ti = tuningInfo(t);
    return ti.stringCount ? ti.openNotes[0] : -1;
}
inline int tabHighestNote(TabTuning t) {
    const TuningInfo& ti = tuningInfo(t);
    return ti.stringCount ? ti.openNotes[ti.stringCount - 1] + ti.maxFret : -1;
}
inline bool tabPlayable(TabTuning t, int note) {
    const TuningInfo& ti = tuningInfo(t);
    if (!ti.stringCount) return true; // 줄 악기가 아니면 제한 없음
    return note >= tabLowestNote(t) && note <= tabHighestNote(t);
}

// 음 -> (줄, 프렛). 로우 포지션 우선: 프렛이 가장 낮은 자리를 고르고,
// 같으면 더 낮은(굵은) 줄을 쓴다. 못 내는 음이면 false.
inline bool tabAssign(TabTuning t, int note, int& stringOut, int& fretOut) {
    const TuningInfo& ti = tuningInfo(t);
    if (!ti.stringCount) return false;
    int bestS = -1, bestF = 0;
    for (int s = 0; s < ti.stringCount; ++s) {
        const int f = note - ti.openNotes[s];
        if (f < 0 || f > ti.maxFret) continue;
        if (bestS < 0 || f < bestF) { bestS = s; bestF = f; }
    }
    if (bestS < 0) return false;
    stringOut = bestS;
    fretOut = bestF;
    return true;
}

// 힌트가 있으면 그 줄에, 없으면 로우 포지션에 놓는다.
// hintTopIdx는 트랙의 TabHint::strIdx 순서(위=0, 즉 가장 높은 줄이 0)다.
// 음수면 힌트 없음. 힌트가 그 음을 못 내는 줄이면 무시하고 자동 배정한다.
//
// 왜 필요한가: 그냥 로우 포지션으로만 놓으면, 프렛을 한 칸 올릴 때마다 "더 낮은
// 프렛으로 낼 수 있는 윗줄"로 표시가 튄다. 사용자가 고른 줄에 머무르게 하려면
// 그 줄을 기억해 우선해야 한다.
inline bool tabPlaceWithHint(TabTuning t, int note, int hintTopIdx, int& stringOut, int& fretOut) {
    const TuningInfo& ti = tuningInfo(t);
    if (!ti.stringCount) return false;
    if (hintTopIdx >= 0 && hintTopIdx < ti.stringCount) {
        const int s = ti.stringCount - 1 - hintTopIdx; // 위=0 -> 아래=0
        const int f = note - ti.openNotes[s];
        if (f >= 0 && f <= ti.maxFret) {
            stringOut = s;
            fretOut = f;
            return true;
        }
    }
    return tabAssign(t, note, stringOut, fretOut);
}

// 음을 이 튜닝이 낼 수 있는 곳으로 옥타브 단위로 끌어온다 (못 하면 false).
// 피아노 롤에서 찍은 음을 줄 악기 트랙에 맞출 때 쓴다.
inline bool tabFitOctave(TabTuning t, int note, int& out) {
    const TuningInfo& ti = tuningInfo(t);
    if (!ti.stringCount) { out = note; return true; }
    int n = note;
    while (n < tabLowestNote(t) && n + 12 <= 127) n += 12;
    while (n > tabHighestNote(t) && n - 12 >= 0) n -= 12;
    if (!tabPlayable(t, n)) return false;
    out = n;
    return true;
}

} // namespace midipro::gui
