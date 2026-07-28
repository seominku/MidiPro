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
//   빈 칸 클릭      그 줄에 노트 추가 (기본 프렛 0 = 개방현)
//   노트 드래그     좌우 = 시각, 위아래 = 줄 (프렛 유지 -> 음이 바뀐다)
//   휠 / [ ]        가리킨 노트의 프렛 -1/+1
//   우클릭 / Del    삭제
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

struct Drag {
    bool active = false;
    seq::NoteSpan span{};   // 잡은 노트(원본)
    int fret = 0;           // 잡을 때의 프렛 (줄을 옮겨도 유지)
    uint32_t grabTick = 0;  // 잡은 지점과 노트 시작의 차이
};
Drag g_drag;

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
    ImGui::TextDisabled("클릭=찍기  드래그=이동(위아래=줄)  휠=프렛  우클릭=삭제");

    // ── 격자 ──
    const float S = uiDpiScale();
    const float rowH = kRowH * S, labelW = kLabelW * S, rulerH = kRulerH * S;
    const float zoom = 0.25f * S; // px/tick (피아노 롤 기본과 같은 감각)
    const float gridH = rowH * (float)ti.stringCount;

    ImGui::BeginChild("##stgrid", ImVec2(0, gridH + rulerH + 8.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const uint32_t songLen = std::max<uint32_t>(track.lengthTicks(), (uint32_t)(ppqn * 16));
    ImGui::Dummy(ImVec2((float)(songLen + ppqn * 4) * zoom, gridH + rulerH));

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

    // ── 노트 그리기 + 히트 테스트 ──
    const auto notes = seq::extractNotes(track);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered();
    int hitIdx = -1;

    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& n = notes[i];
        int s = 0, fret = 0;
        const bool ok = tabAssign(tuning, n.note, s, fret);
        const int r = ok ? (ti.stringCount - 1 - s) : 0;
        const float y = gridTop + rowH * (float)r + rowH * 0.5f;
        const float xa = x0 + (float)n.startTick * zoom;
        const float xb = x0 + (float)n.endTick * zoom;
        const ImU32 col = ok ? IM_COL32(120, 140, 240, 220) : IM_COL32(220, 90, 80, 220);
        dl->AddRectFilled(ImVec2(xa, y - rowH * 0.34f), ImVec2(std::max(xb, xa + 6.0f), y + rowH * 0.34f),
                          col, 3.0f);
        char b[8];
        if (ok) std::snprintf(b, sizeof(b), "%d", fret);
        else std::snprintf(b, sizeof(b), "?");
        dl->AddText(ImVec2(xa + 3.0f, y - 8.0f), IM_COL32(250, 250, 255, 255), b);

        if (hovered && mouse.x >= xa - 2.0f && mouse.x <= std::max(xb, xa + 6.0f) + 2.0f &&
            mouse.y >= y - rowH * 0.4f && mouse.y <= y + rowH * 0.4f)
            hitIdx = (int)i;
    }

    // ── 입력 ──
    const auto rowAt = [&](float my) {
        const int r = (int)((my - gridTop) / rowH);
        return (r < 0 || r >= ti.stringCount) ? -1 : r;
    };
    const auto tickAt = [&](float mx) {
        const float t = (mx - x0) / zoom;
        return t < 0.0f ? 0u : (uint32_t)t;
    };
    const auto snap = [&](uint32_t t) { return (t + snapTicks / 2) / snapTicks * snapTicks; };

    if (hovered) {
        const int r = rowAt(mouse.y);
        // 프렛 조절 (휠)
        const float wheel = ImGui::GetIO().MouseWheel;
        if (hitIdx >= 0 && wheel != 0.0f) {
            const auto& n = notes[(std::size_t)hitIdx];
            int s = 0, f = 0;
            if (tabAssign(tuning, n.note, s, f)) {
                const int nf = f + (wheel > 0 ? 1 : -1);
                const int nn = tabNoteAt(tuning, s, nf);
                if (nn > 0) {
                    state.snapshot();
                    seq::removeNote(track, n);
                    track.addNote(n.startTick, n.endTick - n.startTick, (uint8_t)nn, n.velocity);
                    refreshPlaybackIfPlaying(state);
                }
            }
        }
        // 드래그 시작 / 새 노트
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && r >= 0) {
            if (hitIdx >= 0) {
                const auto& n = notes[(std::size_t)hitIdx];
                int s = 0, f = 0;
                tabAssign(tuning, n.note, s, f);
                g_drag.active = true;
                g_drag.span = n;
                g_drag.fret = f;
                g_drag.grabTick = tickAt(mouse.x) > n.startTick ? tickAt(mouse.x) - n.startTick : 0;
            } else {
                const int s = ti.stringCount - 1 - r;
                const int nn = tabNoteAt(tuning, s, 0); // 개방현
                if (nn > 0) {
                    state.snapshot();
                    track.addNote(snap(tickAt(mouse.x)), noteLen, (uint8_t)nn, 100);
                    refreshPlaybackIfPlaying(state);
                }
            }
        }
        // 삭제
        if (hitIdx >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            state.snapshot();
            seq::removeNote(track, notes[(std::size_t)hitIdx]);
            refreshPlaybackIfPlaying(state);
        }
    }

    // 드래그 진행/확정
    if (g_drag.active) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // 미리보기 선
            const int r = rowAt(mouse.y);
            if (r >= 0) {
                const float y = gridTop + rowH * (float)r + rowH * 0.5f;
                const float x = x0 + (float)snap(tickAt(mouse.x) > g_drag.grabTick
                                                     ? tickAt(mouse.x) - g_drag.grabTick
                                                     : 0) * zoom;
                dl->AddRect(ImVec2(x, y - rowH * 0.36f),
                            ImVec2(x + (float)(g_drag.span.endTick - g_drag.span.startTick) * zoom,
                                   y + rowH * 0.36f),
                            IM_COL32(255, 230, 120, 255), 3.0f, 0, 2.0f);
            }
        } else {
            const int r = rowAt(mouse.y);
            if (r >= 0) {
                const int s = ti.stringCount - 1 - r;
                const int nn = tabNoteAt(tuning, s, g_drag.fret);
                const uint32_t st = snap(tickAt(mouse.x) > g_drag.grabTick
                                             ? tickAt(mouse.x) - g_drag.grabTick
                                             : 0);
                if (nn > 0 && (nn != g_drag.span.note || st != g_drag.span.startTick)) {
                    state.snapshot();
                    seq::removeNote(track, g_drag.span);
                    track.addNote(st, g_drag.span.endTick - g_drag.span.startTick, (uint8_t)nn,
                                  g_drag.span.velocity);
                    refreshPlaybackIfPlaying(state);
                }
            }
            g_drag.active = false;
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
