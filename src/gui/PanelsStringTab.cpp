// =============================================================
// MidiPro - gui/PanelsStringTab.cpp
// 줄 편집기: 기타 6줄 / 베이스 4줄 격자에서 노트를 직접 찍고 옮긴다.
//
// 왜 따로 만들었나 (Rule 1):
//   피아노 롤은 세로축이 음높이(88건반)라, 샘플 기타/베이스에 연주 불가능한
//   음이 쉽게 찍힌다 — 그런 음은 아무 소리도 나지 않는다(Ample Guitar LP에
//   C2를 찍으면 무음). 여기서는 세로축이 "줄"이라 음역을 벗어날 수가 없다.
//   기타 연습 창은 practice 트랙 전용이고 보기 전용이라, 곡 트랙 편집에는
//   맞지 않아 건드리지 않았다.
//
// 조작:
//   빈 칸 클릭        그 줄에 노트 추가 (기본 프렛 0 = 개방현)
//   노트 몸통 드래그  좌우 = 시각, 위아래 = 줄 (프렛 유지 -> 음이 바뀐다)
//   노트 오른쪽 끝    드래그로 길이 늘이기/줄이기
//   휠 / 위아래 화살표 고른 노트의 프렛 -1/+1
//   숫자 키          고른 노트의 프렛을 직접 입력 (두 자리까지)
//   우클릭 / Del     삭제
//
// 입력은 InvisibleButton으로 받는다 — 그래야 ImGui가 hover/active를 제대로
// 잡아주고, 자식 창 스크롤과 휠이 서로 먹히지 않는다.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"
#include "gui/StringTab.h"

#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace midipro::gui {
namespace {

constexpr float kRowH = 26.0f;   // 줄 간격
constexpr float kLabelW = 46.0f; // 왼쪽 줄 이름 칸
constexpr float kRulerH = 22.0f;

// 트랙의 튜닝 (0이면서 기타 표시면 기타 6줄로 본다 — 옛 프로젝트 호환)
TabTuning trackTuning(const seq::Track& t) {
    if (t.tabTuning == 1) return TabTuning::Guitar6;
    if (t.tabTuning == 2) return TabTuning::Bass4;
    return t.isGuitar ? TabTuning::Guitar6 : TabTuning::None;
}

enum class EditMode { None, Move, Resize };

struct EditState {
    EditMode mode = EditMode::None;
    seq::NoteSpan span{};  // 잡은/고른 노트
    int fret = 0;          // 잡을 때의 프렛 (줄을 옮겨도 유지)
    uint32_t grabTick = 0; // 잡은 지점과 노트 시작의 차이
    bool hasSel = false;   // 고른 노트가 있나 (span이 그것)
    int fretTyping = -1;   // 숫자 키로 입력 중인 프렛 (-1 = 입력 안 함)
};
EditState g_ed;

bool sameSpan(const seq::NoteSpan& a, const seq::NoteSpan& b) {
    return a.startTick == b.startTick && a.endTick == b.endTick && a.note == b.note;
}

// ---- 줄 고정(운지 힌트) ----
// 같은 음도 줄이 여럿이라(2번줄 9프렛 = 1번줄 4프렛), 그냥 두면 프렛을 올릴 때마다
// "가장 낮은 자리"로 다시 계산돼 윗줄로 튄다. 사용자가 고른 줄을 트랙의 tabHints에
// 적어 두고 그 줄을 우선한다. 힌트는 프로젝트에 함께 저장된다(thint 줄).
// 주의: TabHint::strIdx는 "위(높은 줄)=0" 순서라 우리 인덱스(아래=0)와 뒤집혀 있다.
int hintStringIdx(const seq::Track& t, uint32_t tick, uint8_t note) {
    for (const auto& h : t.tabHints)
        if (h.tick == tick && h.note == note) return (int)h.strIdx;
    return -1;
}
void clearHint(seq::Track& t, uint32_t tick, uint8_t note) {
    t.tabHints.erase(std::remove_if(t.tabHints.begin(), t.tabHints.end(),
                                    [&](const seq::Track::TabHint& h) {
                                        return h.tick == tick && h.note == note;
                                    }),
                     t.tabHints.end());
}
void setHint(seq::Track& t, uint32_t tick, uint8_t note, int hintIdx) {
    for (auto& h : t.tabHints)
        if (h.tick == tick && h.note == note) {
            h.strIdx = (uint8_t)hintIdx;
            return;
        }
    t.tabHints.push_back({tick, note, (uint8_t)hintIdx});
}

// 노트를 지우고 새 자리에 다시 놓는다 (되돌리기 1회로 묶인다).
// keepString이 0 이상이면 그 줄에 머무르도록 힌트를 옮겨 적는다.
void replaceNote(AppState& state, seq::Track& track, const seq::NoteSpan& oldSpan, uint32_t start,
                 uint32_t len, int note, uint8_t vel, int keepHintIdx = -1) {
    state.snapshot();
    seq::removeNote(track, oldSpan);
    clearHint(track, oldSpan.startTick, oldSpan.note);
    track.addNote(start, std::max<uint32_t>(len, 1), (uint8_t)note, vel);
    if (keepHintIdx >= 0) setHint(track, start, (uint8_t)note, keepHintIdx);
    refreshPlaybackIfPlaying(state);
}

} // namespace

void drawStringTab(AppState& state) {
    if (!state.showStringTab) return;

    // 6줄 + 눈금자 + 툴바 + 아래 안내가 한 번에 보이는 높이
    ImGui::SetNextWindowSize(uiVec(940, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("줄 편집기", &state.showStringTab)) {
        ImGui::End();
        return;
    }

    const int tcount = (int)state.song.tracks.size();
    if (tcount == 0) {
        ImGui::TextDisabled("트랙이 없습니다.");
        ImGui::End();
        return;
    }
    if (state.selectedTrack < 0 || state.selectedTrack >= tcount) state.selectedTrack = 0;
    auto& track = state.song.tracks[(std::size_t)state.selectedTrack];

    // ── 툴바: 트랙 + 튜닝 + 길이 ──
    (ImGui::SetNextItemWidth)(200.0f * uiDpiScale());
    if (ImGui::BeginCombo("##sttrack", track.name.c_str())) {
        for (int i = 0; i < tcount; ++i) {
            const auto& t = state.song.tracks[(std::size_t)i];
            if (t.practice) continue; // 연습 트랙은 기타 연습 창에서 다룬다
            if (ImGui::Selectable(t.name.c_str(), i == state.selectedTrack))
                state.selectedTrack = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    TabTuning tuning = trackTuning(track);
    (ImGui::SetNextItemWidth)(190.0f * uiDpiScale());
    if (ImGui::BeginCombo("##sttune", tuningLabel(tuning))) {
        const TabTuning kOpts[3] = {TabTuning::None, TabTuning::Guitar6, TabTuning::Bass4};
        for (TabTuning o : kOpts)
            if (ImGui::Selectable(tuningLabel(o), o == tuning)) {
                state.snapshot();
                track.tabTuning = (int)o;
                tuning = o;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    static int s_lenDiv = 8; // 새 노트 길이 (분음표)
    (ImGui::SetNextItemWidth)(110.0f * uiDpiScale());
    const char* kLenNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
    const int kLenDivs[] = {1, 2, 4, 8, 16, 32};
    int lenIdx = 3;
    for (int i = 0; i < 6; ++i)
        if (kLenDivs[i] == s_lenDiv) lenIdx = i;
    if (ImGui::Combo("길이", &lenIdx, kLenNames, 6)) s_lenDiv = kLenDivs[lenIdx];

    if (tuning == TabTuning::None) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "이 트랙은 줄 악기가 아닙니다. 위에서 '기타 6줄' 또는 '베이스 4줄'을 고르면 "
            "그 악기가 실제로 낼 수 있는 음만 찍을 수 있는 격자가 나타납니다.");
        ImGui::End();
        return;
    }

    const TuningInfo& ti = tuningInfo(tuning);
    const int ppqn = state.song.ppqn > 0 ? state.song.ppqn : seq::kDefaultPpqn;
    const uint32_t noteLen = (uint32_t)std::max(1, (4 * ppqn) / std::max(1, s_lenDiv));
    const uint32_t snapTicks = noteLen;

    ImGui::SameLine();
    ImGui::TextDisabled("클릭=찍기 · 드래그=이동(↕줄) · 오른쪽 끝 드래그=길이 · "
                        "휠/↑↓=프렛 · 숫자=프렛 입력 · Shift+←→=길이 · 우클릭/Del=삭제");

    // ── 격자 ──
    const float S = uiDpiScale();
    const float rowH = kRowH * S, labelW = kLabelW * S, rulerH = kRulerH * S;
    const float zoom = 0.25f * S; // px/tick (피아노 롤 기본과 같은 감각)
    const float gridH = rowH * (float)ti.stringCount;

    // NoScrollWithMouse: 세로 스크롤이 없는 창에서 ImGui는 휠을 가로 스크롤에 쓴다.
    // 그러면 휠로 프렛을 바꾸려 할 때 화면이 옆으로 밀려 조작이 안 먹는 것처럼 보인다.
    // 휠은 프렛 전용으로 두고, 가로 이동은 아래 스크롤바로 한다.
    ImGui::BeginChild("##stgrid", ImVec2(0, gridH + rulerH + 8.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const uint32_t songLen = std::max<uint32_t>(track.lengthTicks(), (uint32_t)(ppqn * 16));
    const float canvasW = labelW + (float)(songLen + ppqn * 4) * zoom;
    // InvisibleButton으로 입력을 받는다 (Dummy는 hover/active를 안 준다)
    ImGui::InvisibleButton("##stcanvas", ImVec2(canvasW, gridH + rulerH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const float gridTop = p0.y + rulerH;
    const float x0 = p0.x + labelW;

    // 마디선 + 눈금
    const uint32_t barTicks = (uint32_t)ppqn * 4;
    for (uint32_t t = 0; t <= songLen + barTicks; t += (uint32_t)ppqn) {
        const float x = x0 + (float)t * zoom;
        const bool bar = (t % barTicks) == 0;
        dl->AddLine(ImVec2(x, gridTop), ImVec2(x, gridTop + gridH),
                    bar ? IM_COL32(110, 110, 130, 220) : IM_COL32(70, 70, 85, 140), 1.0f);
        if (bar) {
            char b[16];
            std::snprintf(b, sizeof(b), "%u", t / barTicks + 1);
            dl->AddText(ImVec2(x + 3.0f, p0.y + 3.0f), IM_COL32(170, 170, 190, 255), b);
        }
    }
    // 줄 (위 = 가장 높은 줄, 타브 관례)
    for (int r = 0; r < ti.stringCount; ++r) {
        const int s = ti.stringCount - 1 - r; // 화면 r번째 = 줄 인덱스 s
        const float y = gridTop + rowH * (float)r + rowH * 0.5f;
        dl->AddLine(ImVec2(x0, y), ImVec2(x0 + (float)(songLen + barTicks) * zoom, y),
                    IM_COL32(150, 150, 165, 230), 1.2f);
        dl->AddText(ImVec2(p0.x + 6.0f, y - 8.0f), IM_COL32(200, 200, 215, 255), ti.names[s]);
    }

    // 재생 헤드
    {
        const float x = x0 + (float)state.playPosTick * zoom;
        dl->AddLine(ImVec2(x, gridTop), ImVec2(x, gridTop + gridH), IM_COL32(240, 90, 90, 230), 1.6f);
    }

    // ── 좌표 변환 ──
    const auto rowAt = [&](float my) {
        const int r = (int)((my - gridTop) / rowH);
        return (r < 0 || r >= ti.stringCount) ? -1 : r;
    };
    const auto tickAt = [&](float mx) {
        const float t = (mx - x0) / zoom;
        return t < 0.0f ? 0u : (uint32_t)t;
    };
    const auto snap = [&](uint32_t t) { return (t + snapTicks / 2) / snapTicks * snapTicks; };
    const float edgeW = 8.0f * S; // 오른쪽 끝 손잡이 폭

    // ── 노트 그리기 + 히트 테스트 ──
    const auto notes = seq::extractNotes(track);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hitIdx = -1, hitEdge = -1;

    // 화면 줄 인덱스(위=0) <-> 우리 줄 인덱스(아래=0)
    const auto toHint = [&](int s) { return ti.stringCount - 1 - s; };
    // 그 노트가 어느 줄에 놓일지: 고정 힌트가 있으면 그 줄, 없으면 가장 낮은 자리
    const auto placeOf = [&](const seq::NoteSpan& n, int& s, int& fret) {
        return tabPlaceWithHint(tuning, n.note, hintStringIdx(track, n.startTick, n.note), s, fret);
    };

    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& n = notes[i];
        int s = 0, fret = 0;
        const bool ok = placeOf(n, s, fret);
        const int r = ok ? (ti.stringCount - 1 - s) : 0;
        const float y = gridTop + rowH * (float)r + rowH * 0.5f;
        const float xa = x0 + (float)n.startTick * zoom;
        const float xb = std::max(x0 + (float)n.endTick * zoom, xa + 10.0f * S);
        const bool sel = g_ed.hasSel && sameSpan(g_ed.span, n);
        const ImU32 col = !ok ? IM_COL32(220, 90, 80, 220)
                              : (sel ? IM_COL32(255, 190, 90, 240) : IM_COL32(120, 140, 240, 220));
        dl->AddRectFilled(ImVec2(xa, y - rowH * 0.34f), ImVec2(xb, y + rowH * 0.34f), col, 3.0f);
        if (sel)
            dl->AddRect(ImVec2(xa - 1.0f, y - rowH * 0.4f), ImVec2(xb + 1.0f, y + rowH * 0.4f),
                        IM_COL32(255, 255, 255, 230), 3.0f, 0, 1.6f);
        // 오른쪽 끝 손잡이 (길이 조절)
        dl->AddRectFilled(ImVec2(xb - edgeW * 0.5f, y - rowH * 0.34f), ImVec2(xb, y + rowH * 0.34f),
                          IM_COL32(255, 255, 255, sel ? 110 : 60), 2.0f);
        char b[8];
        std::snprintf(b, sizeof(b), ok ? "%d" : "?", fret);
        dl->AddText(ImVec2(xa + 3.0f, y - 8.0f), IM_COL32(20, 20, 28, 255), b);

        if (canvasHovered && mouse.y >= y - rowH * 0.42f && mouse.y <= y + rowH * 0.42f &&
            mouse.x >= xa - 2.0f && mouse.x <= xb + 2.0f) {
            hitIdx = (int)i;
            hitEdge = (mouse.x >= xb - edgeW) ? 1 : 0;
        }
    }
    if (canvasHovered && hitEdge == 1)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // ── 마우스 ──
    if (canvasHovered) {
        const int r = rowAt(mouse.y);
        // 프렛: 휠
        const float wheel = ImGui::GetIO().MouseWheel;
        if (hitIdx >= 0 && wheel != 0.0f) {
            const auto& n = notes[(std::size_t)hitIdx];
            int s = 0, f = 0;
            if (placeOf(n, s, f)) {
                // 그 줄에 머무른다 — 프렛만 바꾸고 줄은 고정한다
                const int nn = tabNoteAt(tuning, s, f + (wheel > 0 ? 1 : -1));
                if (nn > 0) {
                    replaceNote(state, track, n, n.startTick, n.endTick - n.startTick, nn,
                                n.velocity, toHint(s));
                    g_ed.hasSel = true;
                    g_ed.span = {n.startTick, n.endTick, (uint8_t)nn, n.velocity};
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && r >= 0) {
            if (hitIdx >= 0) {
                const auto& n = notes[(std::size_t)hitIdx];
                int s = 0, f = 0;
                placeOf(n, s, f);
                g_ed.mode = (hitEdge == 1) ? EditMode::Resize : EditMode::Move;
                g_ed.span = n;
                g_ed.fret = f;
                g_ed.grabTick = tickAt(mouse.x) > n.startTick ? tickAt(mouse.x) - n.startTick : 0;
                g_ed.hasSel = true;
                g_ed.fretTyping = -1;
            } else {
                const int s = ti.stringCount - 1 - r;
                const int nn = tabNoteAt(tuning, s, 0); // 개방현
                if (nn > 0) {
                    const uint32_t st = snap(tickAt(mouse.x));
                    state.snapshot();
                    track.addNote(st, noteLen, (uint8_t)nn, 100);
                    setHint(track, st, (uint8_t)nn, toHint(s)); // 찍은 줄에 고정
                    refreshPlaybackIfPlaying(state);
                    g_ed.hasSel = true;
                    g_ed.span = {st, st + noteLen, (uint8_t)nn, 100};
                    g_ed.fretTyping = -1;
                }
            }
        }
        if (hitIdx >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            state.snapshot();
            const auto& dn = notes[(std::size_t)hitIdx];
            seq::removeNote(track, dn);
            clearHint(track, dn.startTick, dn.note);
            refreshPlaybackIfPlaying(state);
            g_ed.hasSel = false;
        }
    }

    // ── 드래그 진행/확정 ──
    if (g_ed.mode != EditMode::None) {
        const uint32_t curTick = tickAt(mouse.x);
        const uint32_t oldLen = g_ed.span.endTick - g_ed.span.startTick;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // 미리보기
            if (g_ed.mode == EditMode::Resize) {
                int s = 0, f = 0;
                placeOf(g_ed.span, s, f);
                const float y = gridTop + rowH * (float)(ti.stringCount - 1 - s) + rowH * 0.5f;
                const uint32_t end = std::max(snap(curTick), g_ed.span.startTick + snapTicks);
                dl->AddRect(ImVec2(x0 + (float)g_ed.span.startTick * zoom, y - rowH * 0.4f),
                            ImVec2(x0 + (float)end * zoom, y + rowH * 0.4f),
                            IM_COL32(255, 230, 120, 255), 3.0f, 0, 2.0f);
            } else {
                const int r = rowAt(mouse.y);
                if (r >= 0) {
                    const float y = gridTop + rowH * (float)r + rowH * 0.5f;
                    const uint32_t st = snap(curTick > g_ed.grabTick ? curTick - g_ed.grabTick : 0);
                    dl->AddRect(ImVec2(x0 + (float)st * zoom, y - rowH * 0.4f),
                                ImVec2(x0 + (float)(st + oldLen) * zoom, y + rowH * 0.4f),
                                IM_COL32(255, 230, 120, 255), 3.0f, 0, 2.0f);
                }
            }
        } else {
            if (g_ed.mode == EditMode::Resize) {
                const uint32_t end = std::max(snap(curTick), g_ed.span.startTick + snapTicks);
                const uint32_t len = end - g_ed.span.startTick;
                if (len != oldLen) {
                    int s = 0, f = 0;
                    placeOf(g_ed.span, s, f);
                    replaceNote(state, track, g_ed.span, g_ed.span.startTick, len, g_ed.span.note,
                                g_ed.span.velocity, toHint(s));
                    g_ed.span.endTick = g_ed.span.startTick + len;
                }
            } else {
                const int r = rowAt(mouse.y);
                if (r >= 0) {
                    const int s = ti.stringCount - 1 - r;
                    const int nn = tabNoteAt(tuning, s, g_ed.fret);
                    const uint32_t st = snap(curTick > g_ed.grabTick ? curTick - g_ed.grabTick : 0);
                    if (nn > 0 && (nn != g_ed.span.note || st != g_ed.span.startTick)) {
                        // 놓은 줄에 고정 (프렛은 잡을 때 값을 유지)
                        replaceNote(state, track, g_ed.span, st, oldLen, nn, g_ed.span.velocity,
                                    toHint(s));
                        g_ed.span = {st, st + oldLen, (uint8_t)nn, g_ed.span.velocity};
                    }
                }
            }
            g_ed.mode = EditMode::None;
        }
    }

    // ── 키보드 (고른 노트) ──
    if (g_ed.hasSel && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput) {
        // 고른 노트가 아직 있는지 확인 (되돌리기 등으로 사라졌을 수 있다)
        bool alive = false;
        for (const auto& n : notes)
            if (sameSpan(n, g_ed.span)) { alive = true; break; }
        if (!alive) {
            g_ed.hasSel = false;
        } else {
            int s = 0, f = 0;
            placeOf(g_ed.span, s, f);
            const uint32_t len = g_ed.span.endTick - g_ed.span.startTick;
            // 프렛만 바꾸고 줄은 그대로 둔다 (윗줄로 튀지 않게)
            const auto setFret = [&](int nf) {
                const int nn = tabNoteAt(tuning, s, nf);
                if (nn <= 0) return;
                replaceNote(state, track, g_ed.span, g_ed.span.startTick, len, nn,
                            g_ed.span.velocity, toHint(s));
                g_ed.span.note = (uint8_t)nn;
            };
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) { setFret(f + 1); g_ed.fretTyping = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { setFret(f - 1); g_ed.fretTyping = -1; }
            // 길이: Shift+좌우 = 늘이기/줄이기, 좌우 = 이동
            const bool shift = ImGui::GetIO().KeyShift;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                if (shift) { // 길이 늘이기
                    replaceNote(state, track, g_ed.span, g_ed.span.startTick, len + snapTicks,
                                g_ed.span.note, g_ed.span.velocity, toHint(s));
                    g_ed.span.endTick += snapTicks;
                } else { // 뒤로 이동
                    replaceNote(state, track, g_ed.span, g_ed.span.startTick + snapTicks, len,
                                g_ed.span.note, g_ed.span.velocity, toHint(s));
                    g_ed.span.startTick += snapTicks;
                    g_ed.span.endTick += snapTicks;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                if (shift) { // 길이 줄이기
                    if (len > snapTicks) {
                        replaceNote(state, track, g_ed.span, g_ed.span.startTick, len - snapTicks,
                                    g_ed.span.note, g_ed.span.velocity, toHint(s));
                        g_ed.span.endTick -= snapTicks;
                    }
                } else if (g_ed.span.startTick >= snapTicks) { // 앞으로 이동
                    replaceNote(state, track, g_ed.span, g_ed.span.startTick - snapTicks, len,
                                g_ed.span.note, g_ed.span.velocity, toHint(s));
                    g_ed.span.startTick -= snapTicks;
                    g_ed.span.endTick -= snapTicks;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                state.snapshot();
                seq::removeNote(track, g_ed.span);
                clearHint(track, g_ed.span.startTick, g_ed.span.note);
                refreshPlaybackIfPlaying(state);
                g_ed.hasSel = false;
            }
            // 숫자 키로 프렛 직접 입력 (두 자리까지: 1 -> 12)
            for (int d = 0; d <= 9 && g_ed.hasSel; ++d) {
                const bool hit = ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + d), false) ||
                                 ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + d), false);
                if (!hit) continue;
                const int two = (g_ed.fretTyping >= 0) ? g_ed.fretTyping * 10 + d : d;
                const int nf = (two <= ti.maxFret) ? two : d;
                setFret(nf);
                g_ed.fretTyping = (nf <= 2 && nf > 0) ? nf : -1; // 1·2로 시작할 때만 두 자리 대기
            }
        }
    }

    ImGui::EndChild();

    ImGui::TextDisabled("%s · 낼 수 있는 음: %d ~ %d", tuningLabel(tuning),
                        tabLowestNote(tuning), tabHighestNote(tuning));
    ImGui::SameLine();
    if (ImGui::SmallButton("음역 밖 노트 맞추기")) {
        // 피아노 롤 등에서 찍혀 음역을 벗어난 노트를 옥타브로 끌어온다
        const auto ns = seq::extractNotes(track);
        int fixed = 0;
        bool first = true;
        for (const auto& n : ns) {
            if (tabPlayable(tuning, n.note)) continue;
            int out = 0;
            if (!tabFitOctave(tuning, n.note, out)) continue;
            if (first) { state.snapshot(); first = false; }
            seq::removeNote(track, n);
            track.addNote(n.startTick, n.endTick - n.startTick, (uint8_t)out, n.velocity);
            ++fixed;
        }
        state.statusMessage = fixed ? ("음역 밖 노트 " + std::to_string(fixed) + "개를 옥타브로 옮겼습니다")
                                    : "음역을 벗어난 노트가 없습니다";
        if (fixed) refreshPlaybackIfPlaying(state);
    }

    ImGui::End();
}

} // namespace midipro::gui
