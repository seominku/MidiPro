// =============================================================
// MidiPro - gui/PanelsPianoRoll.cpp
// 피아노 롤 (노트 편집 + 벨로시티/CC 레인)
// Panels.cpp에서 분리 (동작 동일). 공유 헬퍼는 PanelsInternal.h 참고.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"
#include "sequencer/TimeBase.h"
#include "sequencer/ChordFinder.h"
#include "sequencer/Track.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace midipro::gui {

// ---------------------------------------------------------
// 피아노 롤
// ---------------------------------------------------------
void drawPianoRoll(AppState& state) {
    if (!state.showPianoRoll) return;
    ImGui::Begin("피아노 롤", &state.showPianoRoll);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("확대", &state.pianoRollZoom, 0.02f, 2.0f, "%.2f px/tick");
    ImGui::SameLine();
    ImGui::TextDisabled("(휠로 확대/축소)");
    // 재생 헤드 이동: 처음으로 / 현재 마디 시작으로 (뷰도 따라간다)
    ImGui::SameLine();
    if (ImGui::Button("|◀##pr")) seekTo(state, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("처음으로 (틱 0)");
    ImGui::SameLine();
    if (ImGui::Button("◀ 마디##pr")) {
        const uint32_t tpbPr = songTicksPerBar(state);
        const uint32_t barStart = (state.playPosTick / tpbPr) * tpbPr;
        // 이미 마디 시작이면 한 마디 앞으로 (연타로 뒤로 이동)
        seekTo(state, state.playPosTick > barStart
                          ? barStart
                          : (barStart >= tpbPr ? barStart - tpbPr : 0));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("현재 마디의 시작으로 (이미 시작이면 이전 마디로)");
    ImGui::SameLine();
    ImGui::Checkbox("노트 편집", &state.editMode);
    if (state.editMode) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        const char* lens[] = {"4분음표", "8분음표", "16분음표", "32분음표"};
        int lenSel = (state.editNoteLenDiv == 1)   ? 0
                     : (state.editNoteLenDiv == 2) ? 1
                     : (state.editNoteLenDiv == 4) ? 2
                                                   : 3;
        if (ImGui::Combo("길이", &lenSel, lens, IM_ARRAYSIZE(lens)))
            state.editNoteLenDiv = (lenSel == 0) ? 1 : (lenSel == 1) ? 2 : (lenSel == 2) ? 4 : 8;

        // 퀀타이즈: 선택 노트(없으면 전체)를 격자에 스냅
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        const char* grids[] = {"1/4", "1/8", "1/16", "1/32"};
        int gSel = (state.quantGridDiv == 1)   ? 0
                   : (state.quantGridDiv == 2) ? 1
                   : (state.quantGridDiv == 4) ? 2
                                               : 3;
        if (ImGui::Combo("##qgrid", &gSel, grids, IM_ARRAYSIZE(grids)))
            state.quantGridDiv = (gSel == 0) ? 1 : (gSel == 1) ? 2 : (gSel == 2) ? 4 : 8;
        ImGui::SameLine();
        if (ImGui::Button("퀀타이즈") && state.selectedTrack < (int)state.song.tracks.size()) {
            const uint32_t grid =
                std::max<uint32_t>(1, (uint32_t)state.song.ppqn / (uint32_t)state.quantGridDiv);
            quantizeNotes(state, state.song.tracks[state.selectedTrack], grid);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("선택한 노트(없으면 트랙 전체)의 시작을 격자에 맞춥니다");

        // 스윙: 엇박(짝수 격자 칸)을 뒤로 밀어 그루브를 만든다 (드럼 트랙과 동일)
        ImGui::SameLine();
        if (ImGui::Button("스윙")) ImGui::OpenPopup("swingpr");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("엇박을 뒤로 밀어 그루브를 만듭니다 (격자는 왼쪽 콤보 기준).\n"
                              "선택한 노트가 있으면 선택만, 없으면 트랙 전체.");
        if (ImGui::BeginPopup("swingpr")) {
            ImGui::SetNextItemWidth(160);
            ImGui::SliderInt("##prswing", &state.drumSwing, 0, 100, "스윙 %d%%");
            if (ImGui::Button("적용##prswing") &&
                state.selectedTrack < (int)state.song.tracks.size()) {
                const uint32_t grid = std::max<uint32_t>(
                    1, (uint32_t)state.song.ppqn / (uint32_t)state.quantGridDiv);
                applySwing(state, state.song.tracks[state.selectedTrack], grid,
                           state.drumSwing);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 휴머나이즈: 퀀타이즈의 반대 — 타이밍/세기를 살짝 흔들어 사람 느낌
        ImGui::SameLine();
        if (ImGui::Button("휴머나이즈")) ImGui::OpenPopup("humanize");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("선택한 노트(없으면 트랙 전체)의 타이밍/세기를\n"
                              "무작위로 살짝 흔들어 기계적인 느낌을 없앱니다");
        if (ImGui::BeginPopup("humanize")) {
            ImGui::TextDisabled("적용 대상: 선택 노트 (없으면 트랙 전체)");
            ImGui::SetNextItemWidth(160);
            ImGui::SliderInt("타이밍 (±틱)", &state.humanTiming, 0, state.song.ppqn / 4);
            ImGui::SetNextItemWidth(160);
            ImGui::SliderInt("세기 (±)", &state.humanVel, 0, 30);
            if (ImGui::Button("적용") &&
                state.selectedTrack < (int)state.song.tracks.size()) {
                auto& trk = state.song.tracks[(std::size_t)state.selectedTrack];
                state.snapshot();
                const auto targets = gatherSelected(state, trk); // 비면 전체
                const uint32_t seed = (uint32_t)(ImGui::GetTime() * 1000.0) + 1u;
                const int n = seq::humanizeTrack(trk, (uint32_t)state.humanTiming,
                                                 state.humanVel, seed, targets);
                state.selectedNotes.clear(); // 좌표가 바뀌었으니 선택 해제
                refreshPlaybackIfPlaying(state);
                state.statusMessage = "휴머나이즈: " + std::to_string(n) + "개 노트";
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 스케일 하이라이트: 조성을 고르면 구성음 행이 초록빛으로 표시된다
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        static const char* kScaleTypes[] = {"스케일: 끄기", "메이저", "마이너"};
        ImGui::Combo("##scaletype", &state.scaleType, kScaleTypes, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("조성의 구성음 줄을 밝게 표시합니다 (근음은 더 진하게)");
        if (state.scaleType != 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(56);
            static const char* kRoots[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                           "F#", "G",  "G#", "A",  "A#", "B"};
            ImGui::Combo("##scaleroot", &state.scaleRoot, kRoots, 12);
        }
        ImGui::SameLine();
        ImGui::Checkbox("고스트", &state.ghostNotes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("다른 트랙들의 노트를 흐리게 겹쳐 보여줍니다\n"
                              "(화음/베이스 라인을 맞춰 찍을 때)");
        // ── 코드 찾기: 멜로디 → 조성 + 마디별 코드 추천 ──
        ImGui::SameLine();
        if (ImGui::Button("코드 찾기") &&
            state.selectedTrack < (int)state.song.tracks.size()) {
            const auto& tr = state.song.tracks[(std::size_t)state.selectedTrack];
            std::vector<seq::MelNote> mel;
            for (const auto& n : seq::extractNotes(tr))
                mel.push_back({n.note, n.startTick,
                               n.endTick > n.startTick ? n.endTick - n.startTick : 1u});
            if (mel.empty()) {
                state.showChords = false;
                state.statusMessage = "이 트랙에 멜로디 노트가 없습니다";
            } else {
                const seq::MusicKey key = seq::detectKey(mel);
                seq::ChordRecoOptions opt;
                opt.ticksPerBar = songTicksPerBar(state);
                opt.minNoteTicks = songTicksPerBeat(state) / 4; // 16분음표 미만 제외
                const auto rec = seq::recommendChords(mel, key, opt);
                state.chordKeyName = seq::keyName(key);
                state.barChords.clear();
                for (const auto& bc : rec)
                    state.barChords.push_back({bc.bar, seq::chordName(bc.chord)});
                state.chordTrack = state.selectedTrack;
                state.showChords = true;
                state.statusMessage = "조성 " + state.chordKeyName + " · 마디 " +
                                      std::to_string(rec.size()) + "개 코드 추천";
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("선택 트랙의 멜로디를 분석해 조성과 마디별 코드를\n"
                              "추천합니다. 결과가 마디 위에 표시됩니다.");
        if (state.showChords) {
            ImGui::SameLine();
            ImGui::Checkbox("코드 표시##pr", &state.showChords);
            if (!state.chordKeyName.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "조성 %s",
                                   state.chordKeyName.c_str());
            }
        }
    }
    if (state.editMode)
        ImGui::TextDisabled(
            "빈 칸: 추가 · Shift+클릭: 직전 노트 길이/세기로 추가 · 드래그: 이동 · 오른쪽 끝: 길이 "
            "· 우클릭: 삭제 · 1~4: 길이 4/8/16/32분\n"
            "Shift+드래그: 범위 선택 · 선택 드래그: 이동 · Ctrl+C/V/X: 복사/붙여넣기/잘라내기 · "
            "Ctrl+클릭: 길이 반으로 · Del: 선택 삭제");

    // 현재 재생 위치 / 곡 길이 (분:초)
    {
        const double curSec = seq::songTickToSec(state.song, state.playPosTick);
        const double lenSec = seq::songTickToSec(state.song, state.song.lengthTicks());
        ImGui::Text("시간  %d:%04.1f / %d:%04.1f", (int)(curSec / 60.0),
                    curSec - (int)(curSec / 60.0) * 60, (int)(lenSec / 60.0),
                    lenSec - (int)(lenSec / 60.0) * 60);
    }

    if (state.song.tracks.empty() || state.selectedTrack >= (int)state.song.tracks.size()) {
        ImGui::TextDisabled("표시할 트랙을 선택하세요.");
        ImGui::End();
        return;
    }

    seq::Track& track = state.song.tracks[state.selectedTrack];
    const auto notes = seq::extractNotes(track);

    // 선택은 편집 트랙에 종속. 트랙이 바뀌면 이전 선택을 비운다.
    if (state.selNotesTrack != state.selectedTrack) {
        state.selectedNotes.clear();
        state.selNotesTrack = state.selectedTrack;
    }
    // 복사/붙여넣기/잘라내기 (Ctrl+C / Ctrl+V / Ctrl+X). 잘라내기=복사 후 삭제.
    {
        const ImGuiIO& kio = ImGui::GetIO();
        if (!kio.WantTextInput && kio.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelectedNotes(state, track);
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
                copySelectedNotes(state, track);
                deleteSelectedNotes(state, track);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteNotes(state, track);
        }
    }

    // 그리기 영역 (휠은 스크롤 대신 확대/축소로 쓴다).
    // 아래 kVelLaneH만큼은 벨로시티/CC 레인이 쓴다 (편집 모드에서만).
    // 위 헤더(콤보) 높이를 빼고도 조절 영역이 충분하도록 넉넉히 잡는다.
    const float kVelLaneH = state.editMode ? 100.0f : 0.0f;

    // 표준 88건반: A0(21) ~ C8(108). 둘 다 포함(inclusive).
    // (예전엔 C2~C6이었고 최상단 음이 필터에서 빠져 C6 노트가 안 보이던 버그가 있었다)
    constexpr int kLowNote = 21;   // A0
    constexpr int kHighNote = 108; // C8
    constexpr float kRulerH = 22.0f; // 상단 눈금자(마디번호) 높이
    constexpr float kKeyW = 46.0f;   // 왼쪽 고정 건반 열 폭
    // 행 높이는 글자(라벨)가 잘리지 않도록 폰트 줄 높이 이상으로 잡는다.
    const float kRowHeight = std::max(15.0f, ImGui::GetTextLineHeight() + 3.0f);
    const int rows = kHighNote - kLowNote + 1; // 양끝 포함
    const float gridH = rows * kRowHeight;
    const float contentH = kRulerH + gridH;

    // 왼쪽 고정 건반 열을 위해 격자 캔버스를 오른쪽으로 밀어 시작한다.
    // (건반을 캔버스 위에 겹쳐 그리면 스크롤할 때 노트를 가리므로 열을 나눈다)
    const ImVec2 keysPos = ImGui::GetCursorPos(); // 건반 열이 놓일 자리 (창 좌표)
    ImGui::SetCursorPosX(keysPos.x + kKeyW);
    ImGui::BeginChild("roll_canvas", ImVec2(0, -kVelLaneH), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float zoom = state.pianoRollZoom;

    // 표시 길이는 공용 타임라인 마디 수(오디오 길이 포함, 끝 5마디 전 20마디 여유)
    const uint32_t tpbForLen = songTicksPerBar(state);
    const uint32_t songLen = state.timelineBars * tpbForLen;
    // 좌우로 꽉 차게: 그래도 창보다 좁으면 창 폭까지 배경을 채운다
    const float availW = ImGui::GetContentRegionAvail().x;
    const float contentW = std::max(songLen * zoom + 40.0f, availW);
    const float gridTop = origin.y + kRulerH; // 노트 격자는 눈금자 아래부터

    // 마우스 휠: 기본은 확대/축소(커서 기준), Shift = 세로 스크롤, Ctrl = 가로 스크롤.
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+휠 = 가로 스크롤 (위로 굴리면 왼쪽으로)
                ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() - wheel * 160.0f));
            } else if (ImGui::GetIO().KeyShift) {
                // 위로 굴리면(양수) 올리고, 아래로 굴리면(음수) 내린다.
                ImGui::SetScrollY(ImGui::GetScrollY() - wheel * kRowHeight * 3.0f);
            } else {
                const float mouseX = ImGui::GetIO().MousePos.x;
                const float tickAtCursor = (mouseX - origin.x) / zoom;
                const float newZoom = std::clamp(zoom * std::pow(1.15f, wheel), 0.02f, 2.0f);
                state.pianoRollZoom = newZoom;
                ImGui::SetScrollX(
                    std::max(0.0f, ImGui::GetScrollX() + tickAtCursor * (newZoom - zoom)));
            }
        }
    }

    // 88건반은 세로로 길어서 맨 위(C8)부터 보이면 낯설다. 처음 한 번은
    // 가운데 C(60) 근처가 화면 중앙에 오도록 스크롤을 맞춘다.
    {
        static bool centered = false;
        if (!centered) {
            centered = true;
            const int midRow = kHighNote - 60; // 중앙 C의 행
            const float target = midRow * kRowHeight - ImGui::GetWindowHeight() * 0.5f;
            ImGui::SetScrollY(std::max(0.0f, target));
        }
    }

    // 상단 눈금자 배경
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + contentW, gridTop),
                      IM_COL32(30, 30, 34, 255));

    // 스케일 하이라이트: 선택한 조성의 구성음 행을 초록빛으로 띄운다.
    // (0=끄기, 1=메이저, 2=내추럴 마이너) 근음 행은 조금 더 진하게.
    static const bool kMajor[12] = {true, false, true,  false, true, true,
                                    false, true, false, true,  false, true};
    static const bool kMinor[12] = {true, false, true,  true,  false, true,
                                    false, true, true,  false, true,  false};
    const auto inScale = [&](int pc) {
        if (state.scaleType == 0) return false;
        const int rel = ((pc - state.scaleRoot) % 12 + 12) % 12;
        return state.scaleType == 1 ? kMajor[rel] : kMinor[rel];
    };

    // 배경 행 (흰/검은건반 음영 대비를 키우고 행마다 경계선을 긋는다)
    for (int r = 0; r < rows; ++r) {
        const int note = kHighNote - r;
        const int pc = note % 12;
        const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        const float y = gridTop + r * kRowHeight;
        dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + contentW, y + kRowHeight),
                          black ? IM_COL32(36, 36, 42, 255) : IM_COL32(60, 60, 68, 255));
        if (inScale(pc)) { // 스케일 구성음: 초록 틴트 (근음은 진하게)
            const bool root = ((pc - state.scaleRoot) % 12 + 12) % 12 == 0;
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + contentW, y + kRowHeight),
                              IM_COL32(90, 200, 120, root ? 46 : 24));
        }
        // 행 경계선: 옥타브(C) 위는 밝게, 나머지는 은은하게 -> 경계가 또렷해진다
        const ImU32 rowLine = (pc == 0) ? IM_COL32(100, 100, 118, 255)
                                        : IM_COL32(28, 28, 34, 160);
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + contentW, y), rowLine);
    }

    // 마디/박자 격자선 + 눈금자에 마디 번호
    const uint32_t ticksPerBar = songTicksPerBar(state);
    const uint32_t ticksPerBeat = songTicksPerBeat(state); // 6/8은 8분음표가 한 박
    // 박자선 (마디선보다 연하게). 너무 촘촘하면(축소 시) 생략한다.
    if (ticksPerBeat > 0 && ticksPerBeat * zoom >= 6.0f) {
        for (uint32_t t = 0; t <= songLen; t += ticksPerBeat) {
            if (t % ticksPerBar == 0) continue; // 마디선은 아래에서 진하게
            const float x = origin.x + t * zoom;
            dl->AddLine(ImVec2(x, gridTop), ImVec2(x, origin.y + contentH),
                        IM_COL32(66, 66, 76, 255));
        }
    }
    int barNo = 1;
    for (uint32_t t = 0; t <= songLen; t += ticksPerBar, ++barNo) {
        const float x = origin.x + t * zoom;
        dl->AddLine(ImVec2(x, gridTop), ImVec2(x, origin.y + contentH),
                    IM_COL32(115, 115, 130, 255));
        // 눈금자 눈금 + 마디 번호
        dl->AddLine(ImVec2(x, gridTop - 6), ImVec2(x, gridTop), IM_COL32(160, 160, 175, 255));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", barNo);
        dl->AddText(ImVec2(x + 3, origin.y + 3), IM_COL32(200, 200, 215, 255), buf);
        // 코드 찾기 결과: 그 마디의 추천 코드를 마디 번호 옆에 표시
        if (state.showChords) {
            for (const auto& bc : state.barChords)
                if (bc.first == barNo - 1) {
                    const float cw = ImGui::CalcTextSize(bc.second.c_str()).x;
                    dl->AddRectFilled(ImVec2(x + 15, origin.y + 2),
                                      ImVec2(x + 20 + cw, origin.y + 2 + kRulerH * 0.55f),
                                      IM_COL32(40, 60, 90, 220), 2.0f);
                    dl->AddText(ImVec2(x + 18, origin.y + 3),
                                IM_COL32(150, 210, 255, 255), bc.second.c_str());
                    break;
                }
        }
    }

    // 루프 구간 음영 + 경계선
    if (state.loopEnabled) {
        const float lx0 = origin.x + (float)state.loopStartTick * zoom;
        const float lx1 = origin.x + (float)state.loopEndTick * zoom;
        dl->AddRectFilled(ImVec2(lx0, gridTop), ImVec2(lx1, origin.y + contentH),
                          IM_COL32(120, 200, 120, 28));
        dl->AddLine(ImVec2(lx0, gridTop), ImVec2(lx0, origin.y + contentH),
                    IM_COL32(120, 220, 120, 200), 2.0f);
        dl->AddLine(ImVec2(lx1, gridTop), ImVec2(lx1, origin.y + contentH),
                    IM_COL32(120, 220, 120, 200), 2.0f);
    }

    // 고스트 노트: 다른 트랙들의 노트를 흐리게 겹쳐 보여준다 (편집 불가, 참고용)
    if (state.ghostNotes) {
        for (int gt = 0; gt < (int)state.song.tracks.size(); ++gt) {
            if (gt == state.selectedTrack) continue;
            if (state.song.tracks[(std::size_t)gt].practice) continue; // 연습 트랙 제외
            const auto ghost = seq::extractNotes(state.song.tracks[(std::size_t)gt]);
            for (const auto& n : ghost) {
                if (n.note < kLowNote || n.note > kHighNote) continue;
                const int r = kHighNote - n.note;
                const float gx0 = origin.x + n.startTick * zoom;
                const float gx1 = origin.x + n.endTick * zoom;
                const float gy = gridTop + r * kRowHeight;
                dl->AddRectFilled(ImVec2(gx0, gy + 2),
                                  ImVec2(std::max(gx1, gx0 + 2), gy + kRowHeight - 2),
                                  IM_COL32(170, 170, 185, 60), 2.0f);
            }
        }
    }

    // 노트 블록 (선택된 노트는 강조)
    for (const auto& n : notes) {
        if (n.note < kLowNote || n.note > kHighNote) continue;
        const int r = kHighNote - n.note;
        const float x0 = origin.x + n.startTick * zoom;
        const float x1 = origin.x + n.endTick * zoom;
        const float y0 = gridTop + r * kRowHeight;
        const bool isSel = state.selectedNotes.count({n.note, n.startTick}) != 0;
        // 벨로시티가 셀수록 밝게 (40%~100%)
        const float vb = 0.4f + 0.6f * (float)n.velocity / 127.0f;
        const ImU32 fill = isSel ? IM_COL32((int)(250 * vb), (int)(200 * vb), (int)(90 * vb), 255)
                                 : IM_COL32((int)(90 * vb), (int)(170 * vb), (int)(250 * vb), 255);
        const float rx1 = std::max(x1, x0 + 2);
        dl->AddRectFilled(ImVec2(x0, y0 + 1), ImVec2(rx1, y0 + kRowHeight - 1), fill, 2.0f);
        dl->AddRect(ImVec2(x0, y0 + 1), ImVec2(rx1, y0 + kRowHeight - 1),
                    isSel ? IM_COL32(255, 240, 200, 255) : IM_COL32(200, 220, 255, 255), 2.0f,
                    0, isSel ? 2.0f : 1.0f);
        // 음이름(C4·F#3 등)을 노트 위에 얹는다 — 블록이 글자보다 넓고 행이
        // 글자 높이를 담을 때만. 좁으면 생략해 격자가 지저분해지지 않게.
        if (kRowHeight >= 9.0f) {
            const std::string nm = noteName(n.note);
            const ImVec2 ts = ImGui::CalcTextSize(nm.c_str());
            if (rx1 - x0 >= ts.x + 5.0f) {
                const float tx = x0 + 3.0f;
                const float ty = y0 + (kRowHeight - ts.y) * 0.5f;
                // 채움 위 가독성: 밝은(선택) 블록엔 어두운 글자, 아니면 밝은 글자
                dl->AddText(ImVec2(tx, ty),
                            isSel ? IM_COL32(40, 30, 10, 255) : IM_COL32(235, 245, 255, 255),
                            nm.c_str());
            }
        }
    }

    // 선택 트랙의 오디오 클립들 (어두운 블록 + 파형)
    for (const auto& cp : track.clips) {
        if (!cp) continue;
        drawClipBlock(dl, *cp, origin.x, gridTop, gridH, zoom, state.song, true);
        drawWaveform(dl, *cp, origin.x, gridTop, gridH, zoom, state.song,
                     IM_COL32(130, 220, 175, 200));
    }

    // 템포 변경 지점 (주황 세로선 + BPM 라벨)
    drawTempoMarkers(dl, state.song, origin.x, zoom, gridTop, gridTop + gridH, true,
                     state.selectedTempoMarker);

    // 재생 헤드(플레이헤드): 정지 중에도 항상 그린다. 눈금자엔 드래그용 손잡이.
    {
        // 레이턴시 보정: 재생 중엔 출력 지연만큼 바를 뒤로 그려 "보이는 바"와
        // "들리는 소리"를 맞춘다. tick X의 소리는 지연 뒤에 들리므로, 그 순간
        // 트랜스포트는 X+지연에 있다 -> 바를 (현재틱 - 지연)에 그리면 일치.
        uint32_t headTick = state.playPosTick;
        const bool playing = state.player && state.player->isPlaying();
        if (playing && state.output) {
            const double latSec = state.output->outputLatencySeconds();
            const uint32_t latTicks = (uint32_t)seq::secondsToTicks(
                latSec, seq::bpmAtTick(state.song, headTick), state.song.ppqn);
            headTick = headTick > latTicks ? headTick - latTicks : 0;
        }
        const float x = origin.x + headTick * zoom;
        dl->AddLine(ImVec2(x, gridTop), ImVec2(x, origin.y + contentH), IM_COL32(255, 90, 90, 255),
                    2.0f);
        // 손잡이 (눈금자 안, 이미지의 흰 핸들처럼)
        dl->AddRectFilled(ImVec2(x - 6, origin.y + 2), ImVec2(x + 6, gridTop - 2),
                          IM_COL32(230, 230, 235, 255), 3.0f);
        dl->AddTriangleFilled(ImVec2(x - 5, gridTop - 2), ImVec2(x + 5, gridTop - 2),
                              ImVec2(x, gridTop + 4), IM_COL32(255, 90, 90, 255));
        // 재생 중 따라가기 + 시크 시 뷰 이동 (보정된 위치 기준)
        if ((playing && state.followPlayhead) || state.scrollToPlayhead) {
            const float scrollTarget = headTick * zoom - ImGui::GetWindowWidth() * 0.5f;
            ImGui::SetScrollX(std::max(0.0f, scrollTarget));
        }
    }


    // 눈금자 시크: 눈금자 영역을 드래그해 재생 위치를 지정한다.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("roll_ruler", ImVec2(contentW, kRulerH));
    bool rulerDragging = false; // 플레이헤드 바를 끄는 중 -> 가장자리 자동 스크롤
    if (!state.recording) {
        rulerDragging = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) {
            // 드래그 시작: 재생 중이면 잠시 멈추고, 아니면 남은 잔음을 끊는다
            state.seekWasPlaying = state.player && state.player->isPlaying();
            if (state.seekWasPlaying) {
                state.player->stop();
                if (state.audioClips) state.audioClips->stopAudio();
            } else {
                silenceOutput(state);
            }
        }
        if (ImGui::IsItemActive()) {
            const float mx = ImGui::GetIO().MousePos.x;
            uint32_t t = mx > origin.x ? (uint32_t)((mx - origin.x) / zoom) : 0;
            if (t > songLen) t = songLen;
            state.playPosTick = t;
            if (state.audioClips) state.audioClips->seekAudio(tickToFrame(state, t));
        }
        if (ImGui::IsItemDeactivated() && state.seekWasPlaying) {
            state.seekWasPlaying = false;
            startPlayback(state); // 놓으면 새 위치부터 이어서 재생
        }
    }

    // 노트 편집/스크롤 영역: 눈금자 아래 격자만 덮는다.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, gridTop));
    ImGui::InvisibleButton("roll_input", ImVec2(contentW, gridH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    if (state.editMode) {
        // 마우스 위치 -> (틱, 노트). 드래그 중엔 아이템 밖이어도 전역 좌표 사용.
        const ImGuiIO& eio = ImGui::GetIO();
        const bool shift = eio.KeyShift;
        const bool ctrl = eio.KeyCtrl;
        const ImVec2 mouse = eio.MousePos;
        const float relX = mouse.x - origin.x;
        const float relY = mouse.y - gridTop;
        const int hoverNote = kHighNote - (int)(relY / kRowHeight);
        const uint32_t hoverTick = relX > 0 ? (uint32_t)(relX / zoom) : 0;
        const uint32_t grid = std::max<uint32_t>(1, (uint32_t)state.song.ppqn / 4); // 16분음표
        const uint32_t minLen = std::max<uint32_t>(1, (uint32_t)state.song.ppqn / 8); // 32분 하한
        auto snap = [grid](uint32_t t) { return (t / grid) * grid; };
        auto& drag = state.noteDrag;
        auto& box = state.boxSelect;
        auto& smove = state.selMove;
        // 빈 칸 클릭-드래그로 경로에 노트를 연달아 그리는 "페인트" 상태.
        // paintCells: 이번 스트로크에서 채운 시간 칸 — 같은 칸에 음정만 다른
        // 노트가 쌓이는 것(축소 시 세로 흔들림)을 막는다.
        static bool paintActive = false;
        static uint32_t paintLastTick = 0;
        static int paintLastNote = -1;
        static std::set<uint32_t> paintCells;
        auto key = [](const seq::NoteSpan& s) { return std::make_pair(s.note, s.startTick); };

        if (smove.active) {
            // 선택 무리 이동 중
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                smove.dTick = (int)snap(hoverTick) - (int)snap(smove.grabTick);
                smove.dNote = hoverNote - smove.grabNote;
            } else {
                moveSelectedNotes(state, track, smove.dTick, smove.dNote);
                smove.active = false;
                state.statusMessage = "선택 이동";
            }
        } else if (box.active) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float ddx = mouse.x - box.downX;
                const float ddy = mouse.y - box.downY;
                if (ddx * ddx + ddy * ddy < 16.0f) {
                    // 움직임 없이 놓음(클릭) = 직전 노트의 길이/세기를 복사해 생성
                    box.active = false;
                    const int bn = box.anchorNote;
                    bool occupied = false;
                    if (bn >= kLowNote && bn <= kHighNote) {
                        seq::noteSpanAt(track, (uint8_t)bn, box.anchorTick, occupied);
                        if (!occupied) {
                            state.snapshot();
                            uint32_t len = state.lastNoteDurationTicks;
                            if (len == 0)
                                len = (uint32_t)state.song.ppqn /
                                      (uint32_t)state.editNoteLenDiv;
                            track.addNote(snap(box.anchorTick), std::max<uint32_t>(len, 1),
                                          (uint8_t)bn, state.lastNoteVelocity);
                            seq::adoptNoteIntoClips(track, (uint8_t)bn, snap(box.anchorTick));
                            track.sortEvents();
                            state.statusMessage = "노트 추가 (직전 노트 복제)";
                            triggerNote(state, track.channel, (uint8_t)bn,
                                        state.lastNoteVelocity, 0.3);
                            refreshPlaybackIfPlaying(state);
                        }
                    }
                } else {
                    // 드래그였음 = 범위 안 노트 선택
                    const uint32_t t0 = std::min(box.anchorTick, hoverTick);
                    const uint32_t t1 = std::max(box.anchorTick, hoverTick);
                    const int n0 = std::min(box.anchorNote, hoverNote);
                    const int n1 = std::max(box.anchorNote, hoverNote);
                    state.selectedNotes.clear();
                    for (const auto& n : notes)
                        if ((int)n.note >= n0 && (int)n.note <= n1 && n.startTick <= t1 &&
                            n.endTick >= t0)
                            state.selectedNotes.insert(key(n));
                    box.active = false;
                    state.statusMessage =
                        "선택: " + std::to_string(state.selectedNotes.size()) + "개";
                }
            }
        } else if (drag.active) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (drag.mode == AppState::NoteDrag::Move) {
                    long ns = (long)hoverTick - drag.grabTickOffset;
                    drag.curStart = snap((uint32_t)(ns < 0 ? 0 : ns));
                    drag.curNote = (uint8_t)std::clamp(hoverNote, 0, 127);
                    drag.curDuration = drag.durationTicks;
                    // 음정이 바뀌면 그 음을 잠깐 들려준다 (드래그 중 드론 방지).
                    if ((int)drag.curNote != drag.lastAuditionNote) {
                        drag.lastAuditionNote = drag.curNote;
                        triggerNote(state, track.channel, drag.curNote, drag.velocity, 0.3);
                    }
                } else { // Resize: 오른쪽 끝을 끈다
                    uint32_t endT = snap(hoverTick) + grid;
                    if (endT <= drag.startTick) endT = drag.startTick + grid;
                    drag.curStart = drag.startTick;
                    drag.curNote = drag.note;
                    drag.curDuration = endT - drag.startTick;
                }
            } else {
                // 놓음 -> 새 위치/길이로 확정 (undo 스냅샷은 드래그 시작 때 이미 남김)
                track.addNote(drag.curStart, std::max<uint32_t>(drag.curDuration, 1), drag.curNote,
                              drag.velocity);
                seq::adoptNoteIntoClips(track, drag.curNote, drag.curStart);
                track.sortEvents();
                state.lastNoteDurationTicks = std::max<uint32_t>(drag.curDuration, 1);
                state.lastNoteVelocity = drag.velocity;
                drag.active = false;
                state.statusMessage =
                    drag.mode == AppState::NoteDrag::Move ? "노트 이동" : "노트 길이 조절";
                refreshPlaybackIfPlaying(state);
            }
        } else if (paintActive) {
            // 페인트: 드래그 경로의 격자 칸마다 노트를 깐다 (이미 있는 칸은 건너뜀).
            // 빠르게 움직여 칸을 건너뛰면 지난 칸도 직선 보간으로 채운다.
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (relX >= 0 && hoverNote >= kLowNote && hoverNote <= kHighNote) {
                    const uint32_t pt = snap(hoverTick);
                    if (pt != paintLastTick || hoverNote != paintLastNote) {
                        const uint32_t len =
                            std::max<uint32_t>(1, (uint32_t)state.song.ppqn /
                                                      (uint32_t)state.editNoteLenDiv);
                        const int steps =
                            grid > 0 ? (int)(std::llabs((long long)pt -
                                                        (long long)paintLastTick) /
                                             (long long)grid)
                                     : 0;
                        bool added = false;
                        for (int k = 1; k <= std::max(1, steps); ++k) {
                            const float f = steps > 0 ? (float)k / (float)steps : 1.0f;
                            const uint32_t tk =
                                steps > 0
                                    ? (uint32_t)((int64_t)paintLastTick +
                                                 (int64_t)((double)((int64_t)pt -
                                                                    (int64_t)paintLastTick) *
                                                           f))
                                    : pt;
                            const int nn = (int)std::lround(
                                paintLastNote + (hoverNote - paintLastNote) * f);
                            const uint32_t st = snap(tk);
                            if (nn < kLowNote || nn > kHighNote) continue;
                            if (paintCells.count(st)) continue; // 이번 스트로크: 칸당 하나
                            bool occ = false;
                            seq::noteSpanAt(track, (uint8_t)nn, st, occ);
                            if (occ) continue;
                            track.addNote(st, len, (uint8_t)nn, 100);
                            seq::adoptNoteIntoClips(track, (uint8_t)nn, st);
                            paintCells.insert(st);
                            added = true;
                        }
                        if (added) track.sortEvents();
                        if (hoverNote != paintLastNote) // 음정이 바뀔 때만 살짝 들려준다
                            triggerNote(state, track.channel, (uint8_t)hoverNote, 100, 0.12);
                        paintLastTick = pt;
                        paintLastNote = hoverNote;
                    }
                }
            } else {
                paintActive = false;
                refreshPlaybackIfPlaying(state);
                state.statusMessage = "노트 그리기 완료";
            }
        } else if (ImGui::IsItemHovered() && relX >= 0) {
            const bool leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            const bool rightClick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
            bool found = false;
            seq::NoteSpan hit{};
            if (hoverNote >= kLowNote && hoverNote <= kHighNote)
                hit = seq::noteSpanAt(track, (uint8_t)hoverNote, hoverTick, found);

            // 노트 오른쪽 끝 근처면 "길이 조절" 커서로 알려준다
            if (found) {
                const float endX = origin.x + hit.endTick * zoom;
                if ((endX - mouse.x) >= -1.0f && (endX - mouse.x) <= 7.0f)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }

            if (shift && leftClick) {
                // Shift+드래그 = 박스 선택 / 움직임 없이 놓으면 직전 노트 복제 생성
                box.active = true;
                box.anchorTick = hoverTick;
                box.anchorNote = hoverNote;
                box.downX = mouse.x;
                box.downY = mouse.y;
            } else if (ctrl && leftClick && found) {
                // Ctrl+클릭 = 길이 반으로 (32분음표 미만으로는 자르지 않음)
                const uint32_t dur = hit.endTick - hit.startTick;
                const uint32_t half = dur / 2;
                if (half >= minLen) {
                    state.snapshot();
                    seq::removeNote(track, hit);
                    track.addNote(hit.startTick, half, hit.note, hit.velocity);
                    seq::adoptNoteIntoClips(track, hit.note, hit.startTick);
                    track.sortEvents();
                    state.statusMessage = "길이 반으로";
                    refreshPlaybackIfPlaying(state);
                } else {
                    state.statusMessage = "이미 32분음표라 더 못 자릅니다";
                }
            } else if (rightClick && found) {
                state.snapshot();
                seq::removeNote(track, hit); // 우클릭 = 삭제
                state.selectedNotes.erase(key(hit));
                state.statusMessage = "노트 삭제";
                refreshPlaybackIfPlaying(state);
            } else if (leftClick && found && state.selectedNotes.count(key(hit))) {
                // 선택된 노트를 잡음 -> 선택 무리 이동 시작 (스냅샷은 이동 확정 때)
                smove.active = true;
                smove.grabTick = hoverTick;
                smove.grabNote = hoverNote;
                smove.dTick = 0;
                smove.dNote = 0;
            } else if (leftClick && found) {
                // 선택 밖 노트 좌클릭 -> 선택 해제 후 단일 이동/크기 조절
                state.selectedNotes.clear();
                state.snapshot();
                const float endX = origin.x + hit.endTick * zoom;
                const bool nearRight = (endX - mouse.x) >= -1.0f && (endX - mouse.x) <= 7.0f;
                seq::removeNote(track, hit);
                drag.active = true;
                drag.mode = nearRight ? AppState::NoteDrag::Resize : AppState::NoteDrag::Move;
                drag.note = hit.note;
                drag.velocity = hit.velocity;
                drag.startTick = hit.startTick;
                drag.durationTicks = hit.endTick - hit.startTick;
                drag.grabTickOffset = (int)hoverTick - (int)hit.startTick;
                drag.curNote = hit.note;
                drag.curStart = hit.startTick;
                drag.curDuration = drag.durationTicks;
                drag.lastAuditionNote = hit.note; // 원래 음정에선 소리내지 않음
                // 만진 노트가 "직전 노트"가 된다 (Shift+클릭 복제용)
                state.lastNoteDurationTicks = hit.endTick - hit.startTick;
                state.lastNoteVelocity = hit.velocity;
            } else if (leftClick && !found && hoverNote >= kLowNote && hoverNote <= kHighNote) {
                // 빈 칸 좌클릭 -> 노트 추가 + "페인트" 시작 (드래그하면 경로에 연달아 생성)
                state.selectedNotes.clear();
                state.snapshot(); // 스트로크 전체가 언두 1회
                const uint32_t len = (uint32_t)state.song.ppqn / (uint32_t)state.editNoteLenDiv;
                track.addNote(snap(hoverTick), len > 0 ? len : 1, (uint8_t)hoverNote, 100);
                seq::adoptNoteIntoClips(track, (uint8_t)hoverNote, snap(hoverTick));
                track.sortEvents();
                paintActive = true;
                paintLastTick = snap(hoverTick);
                paintLastNote = hoverNote;
                paintCells.clear();
                paintCells.insert(paintLastTick); // 시작 칸은 이미 채웠다
                state.lastNoteDurationTicks = len > 0 ? len : 1;
                state.lastNoteVelocity = 100;
                state.statusMessage = "노트 추가 (드래그: 경로에 계속 그리기)";
                // 추가한 음을 잠깐 들려준다(미리듣기): 노트 길이만큼, 0.15~1.0초로 제한.
                const double auditionSec = std::clamp(
                    seq::ticksToSeconds(len > 0 ? len : 1,
                                        seq::bpmAtTick(state.song, hoverTick), state.song.ppqn),
                    0.15, 1.0);
                triggerNote(state, track.channel, (uint8_t)hoverNote, 100, auditionSec);
                refreshPlaybackIfPlaying(state);
            }
        }

        // 단일 드래그 중 고스트 노트
        if (drag.active) {
            const int r = kHighNote - drag.curNote;
            if (r >= 0 && r < rows) {
                const float gx0 = origin.x + drag.curStart * zoom;
                const float gx1 = origin.x + (drag.curStart + drag.curDuration) * zoom;
                const float gy0 = gridTop + r * kRowHeight;
                dl->AddRectFilled(ImVec2(gx0, gy0 + 1),
                                  ImVec2(std::max(gx1, gx0 + 2), gy0 + kRowHeight - 1),
                                  IM_COL32(250, 210, 90, 220), 2.0f);
            }
        }
        // 선택 무리 이동 중 고스트
        if (smove.active) {
            for (const auto& n : notes) {
                if (!state.selectedNotes.count(key(n))) continue;
                const int nn = std::clamp((int)n.note + smove.dNote, 0, 127);
                const long nt = std::max(0L, (long)n.startTick + smove.dTick);
                const int r = kHighNote - nn;
                if (r < 0 || r >= rows) continue;
                const float gx0 = origin.x + nt * zoom;
                const float gx1 = origin.x + (nt + (long)(n.endTick - n.startTick)) * zoom;
                const float gy0 = gridTop + r * kRowHeight;
                dl->AddRectFilled(ImVec2(gx0, gy0 + 1),
                                  ImVec2(std::max(gx1, gx0 + 2), gy0 + kRowHeight - 1),
                                  IM_COL32(250, 210, 90, 200), 2.0f);
            }
        }
        // 박스 선택 사각형
        if (box.active) {
            const float bx0 = origin.x + std::min(box.anchorTick, hoverTick) * zoom;
            const float bx1 = origin.x + std::max(box.anchorTick, hoverTick) * zoom;
            const int rn0 = kHighNote - std::max(box.anchorNote, hoverNote);
            const int rn1 = kHighNote - std::min(box.anchorNote, hoverNote);
            const float by0 = gridTop + std::max(0, rn0) * kRowHeight;
            const float by1 = gridTop + std::min(rows, rn1 + 1) * kRowHeight;
            dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(120, 180, 255, 40));
            dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(150, 200, 255, 200));
        }
    }

    // 드래그 중 마우스가 뷰 좌/우 끝에 닿으면 자동으로 가로 스크롤(옆으로 넘어감).
    // 플레이헤드 바(눈금자) 드래그와 노트 편집 드래그 모두에 적용된다.
    // 위치/틱은 매 프레임 스크롤된 원점 기준으로 다시 계산되므로, 스크롤이
    // 진행되면 바 위치나 선택 범위도 자연스럽게 따라 늘어난다.
    if (rulerDragging || state.noteDrag.active || state.selMove.active ||
        state.boxSelect.active) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float left = ImGui::GetWindowPos().x;
        const float right = left + ImGui::GetWindowWidth();
        constexpr float kEdge = 48.0f;     // 가장자리 감지 폭
        constexpr float kMaxSpeed = 18.0f; // 프레임당 최대 스크롤 px
        float dx = 0.0f;
        if (mx < left + kEdge)
            dx = -(1.0f - std::clamp((mx - left) / kEdge, 0.0f, 1.0f)) * kMaxSpeed;
        else if (mx > right - kEdge)
            dx = (1.0f - std::clamp((right - mx) / kEdge, 0.0f, 1.0f)) * kMaxSpeed;
        if (dx != 0.0f) ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + dx));
    }

    // 방향키 스크롤 (←→ 가로, ↑↓ 세로 — 트랙 뷰와 같은 속도)
    if (state.keyScrollX != 0.0f)
        ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + state.keyScrollX));
    if (state.keyScrollY != 0.0f)
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() + state.keyScrollY));

    const float canvasScrollX = ImGui::GetScrollX(); // 벨로시티 레인과 가로 동기화
    // 건반 열이 격자 행과 정확히 맞도록 캔버스의 실제 행 시작 화면 좌표를 넘긴다
    // (스크롤 값을 따로 맞추면 한 프레임 어긋난다)
    const float canvasGridTopY = gridTop;
    ImGui::EndChild();

    // ── 왼쪽 고정 피아노 건반 열 ──
    // 어느 행이 어떤 음인지 한눈에 보이도록 실제 건반 모양으로 그린다.
    // 캔버스와 별도 열이라 가로 스크롤을 해도 노트를 가리지 않는다. 세로
    // 스크롤은 캔버스 값을 그대로 따라간다(같은 프레임에 맞추려고 뒤에 그린다).
    // 클릭하면 그 음을 미리듣기 한다.
    {
        const ImVec2 afterPos = ImGui::GetCursorPos(); // 캔버스 다음 자리 (복원용)
        ImGui::SetCursorPos(keysPos);
        ImGui::BeginChild("roll_keys", ImVec2(kKeyW, -kVelLaneH), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* kdl = ImGui::GetWindowDrawList();
        const ImVec2 kp = ImGui::GetCursorScreenPos();
        // 캔버스가 알려준 행 시작 위치를 그대로 쓴다 — 세로 스크롤을 해도
        // 건반과 격자 행이 같은 프레임에 정확히 맞는다. 열 밖은 자동 클립.
        const float kTop = canvasGridTopY;
        const float blackW = kKeyW * 0.62f;
        auto isBlack = [](int n) {
            const int pc = ((n % 12) + 12) % 12;
            return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        };
        // 배경 (눈금자 줄과 건반 위/아래 여백)
        kdl->AddRectFilled(ImVec2(kp.x, kp.y), ImVec2(kp.x + kKeyW, kp.y + contentH),
                           IM_COL32(30, 30, 34, 255));
        // 1) 흰건반: 검은건반 행까지 덮도록 먼저 전체를 흰색으로
        kdl->AddRectFilled(ImVec2(kp.x, kTop), ImVec2(kp.x + kKeyW, kTop + gridH),
                           IM_COL32(238, 238, 242, 255));
        // 2) 흰건반 경계선: 검은건반이 끼지 않은 자리(E-F, B-C)에만 실선
        for (int r = 0; r <= rows; ++r) {
            const int above = kHighNote - r;
            if (r > 0 && r < rows && (isBlack(above) || isBlack(above - 1))) continue;
            const float y = kTop + r * kRowHeight;
            kdl->AddLine(ImVec2(kp.x, y), ImVec2(kp.x + kKeyW, y), IM_COL32(150, 150, 158, 255));
        }
        // 3) 검은건반: 짧고 어둡게 위에 얹는다
        for (int r = 0; r < rows; ++r) {
            const int note = kHighNote - r;
            if (!isBlack(note)) continue;
            const float y = kTop + r * kRowHeight;
            kdl->AddRectFilled(ImVec2(kp.x, y + 1.0f),
                               ImVec2(kp.x + blackW, y + kRowHeight - 1.0f),
                               IM_COL32(28, 28, 34, 255), 2.0f);
        }
        // 4) 옥타브 C 이름 (흰건반 오른쪽 끝 — 검은건반과 겹치지 않는 자리)
        {
            const float dy = (kRowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            for (int r = 0; r < rows; ++r) {
                const int note = kHighNote - r;
                if (note % 12 != 0) continue; // C만
                const std::string nm = noteName((uint8_t)note);
                const ImVec2 ts = ImGui::CalcTextSize(nm.c_str());
                kdl->AddText(ImVec2(kp.x + kKeyW - ts.x - 3.0f, kTop + r * kRowHeight + dy),
                             IM_COL32(40, 40, 48, 255), nm.c_str());
            }
        }
        // 5) 클릭 = 미리듣기, 호버 = 그 건반 강조.
        //    입력 영역은 열의 보이는 범위 전체 (행 계산은 kTop 기준이라 스크롤 반영)
        ImGui::SetCursorScreenPos(kp);
        ImGui::InvisibleButton("keys_input", ImVec2(kKeyW, std::max(1.0f, contentH)));
        if (ImGui::IsItemHovered()) {
            const float my = ImGui::GetIO().MousePos.y;
            const int r = (int)std::floor((my - kTop) / kRowHeight);
            if (r >= 0 && r < rows) {
                const int note = kHighNote - r;
                const float y = kTop + r * kRowHeight;
                kdl->AddRectFilled(ImVec2(kp.x, y + 1.0f),
                                   ImVec2(kp.x + (isBlack(note) ? blackW : kKeyW),
                                          y + kRowHeight - 1.0f),
                                   IM_COL32(120, 180, 255, 110), 2.0f);
                ImGui::SetTooltip("%s", noteName((uint8_t)note).c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    triggerNote(state, track.channel, (uint8_t)note, 100, 0.5);
            }
        }
        ImGui::EndChild();
        // 커서를 캔버스 다음 자리로 되돌린다. 되돌린 뒤에는 아이템을 하나
        // 제출해야 창 경계가 그 자리까지 자란다 — 편집 모드가 꺼져 있으면
        // 아래 레인이 없어 아무 아이템도 안 나오고, 그러면 ImGui가
        // "SetCursorPos 뒤에 Dummy를 넣으라"며 단언에 걸린다.
        ImGui::SetCursorPos(afterPos);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    // ── 아래 레인 (편집 모드): 벨로시티 막대 또는 CC 곡선 (콤보로 전환) ──
    // 캔버스 밖의 고정 높이 영역이라 세로 스크롤과 무관하게 항상 보인다.
    if (state.editMode) {
        ImGui::BeginChild("vel_lane", ImVec2(0, 0), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* vdl = ImGui::GetWindowDrawList();
        const ImVec2 vp0 = ImGui::GetCursorScreenPos();
        const float laneW = ImGui::GetContentRegionAvail().x;
        const float laneH = ImGui::GetContentRegionAvail().y;
        const float originX = vp0.x - canvasScrollX; // 틱 0의 화면 x

        vdl->AddRectFilled(vp0, ImVec2(vp0.x + laneW, vp0.y + laneH), IM_COL32(26, 26, 30, 255));

        // 레인 내용 선택 콤보 (왼쪽 위). 입력 영역보다 먼저 제출돼 클릭 우선권을 갖는다.
        static const char* kLaneNames[] = {"벨로시티",         "모듈레이션 (CC1)",
                                           "익스프레션 (CC11)", "서스테인 (CC64)",
                                           "팬 (CC10)",        "피치 벤드"};
        static const int kLaneCc[] = {-1, 1, 11, 64, 10, -2}; // -1=벨로시티, -2=피치 벤드
        int laneSel = 0;
        for (int s = 0; s < 6; ++s)
            if (kLaneCc[s] == state.prLaneCc) laneSel = s;
        ImGui::SetCursorScreenPos(ImVec2(vp0.x + 4.0f, vp0.y + 3.0f));
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("##lanesel", &laneSel, kLaneNames, 6)) state.prLaneCc = kLaneCc[laneSel];

        // 위 헤더(콤보) / 아래 페인트 영역을 분리해 막대·곡선과 겹치지 않게 한다
        const float headerH = ImGui::GetFrameHeight() + 6.0f;
        const ImVec2 pp0(vp0.x, vp0.y + headerH);
        const float paintH = std::max(10.0f, laneH - headerH);
        vdl->AddLine(ImVec2(vp0.x, pp0.y - 1.0f), ImVec2(vp0.x + laneW, pp0.y - 1.0f),
                     IM_COL32(60, 60, 70, 255));
        ImGui::SetCursorScreenPos(pp0);
        ImGui::InvisibleButton("vel_input", ImVec2(laneW, paintH));
        const bool laneActive = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) state.snapshot(); // 드래그 시작 시 한 번만 undo 지점
        const ImVec2 vm = ImGui::GetIO().MousePos;

        if (state.prLaneCc == -2) {
            // ── 피치 벤드: 가운데=0, 맨 위=+최대, 맨 아래=-최대 (14비트) ──
            const uint8_t pbStatus =
                (uint8_t)(midi::kStatusPitchBend | (track.channel & 0x0F));
            const auto isPb = [](const seq::MidiEvent& e) {
                return (e.status & 0xF0) == midi::kStatusPitchBend;
            };
            // 중앙선 (벤드 0)
            const float cy = pp0.y + paintH * 0.5f;
            vdl->AddLine(ImVec2(vp0.x, cy), ImVec2(vp0.x + laneW, cy),
                         IM_COL32(90, 90, 105, 200));
            if (laneActive) {
                const double tickF = std::max(0.0, (double)(vm.x - originX) / zoom);
                const uint32_t t = (uint32_t)tickF;
                const float rel = std::clamp(1.0f - (vm.y - pp0.y) / paintH, 0.0f, 1.0f);
                const int v14 = std::clamp((int)std::lround(rel * 16383.0f), 0, 16383);
                const uint32_t win = (uint32_t)std::max(1, state.song.ppqn / 16);
                track.events.erase(
                    std::remove_if(track.events.begin(), track.events.end(),
                                   [&](const seq::MidiEvent& e) {
                                       return isPb(e) && e.tick + win >= t && e.tick <= t + win;
                                   }),
                    track.events.end());
                seq::MidiEvent e;
                e.tick = t;
                e.status = pbStatus;
                e.data1 = (uint8_t)(v14 & 0x7F); // LSB
                e.data2 = (uint8_t)(v14 >> 7);   // MSB
                track.events.push_back(e);
                track.sortEvents();
                // 즉시 전송해 그리는 동안 소리로 확인
                if (state.output && state.output->isOpen())
                    state.output->send({pbStatus, e.data1, e.data2});
            }
            // 그리기: 계단 라인 + 점
            const ImU32 pbCol = IM_COL32(120, 230, 190, 235);
            float prevX = -1.0f, prevY = 0.0f;
            for (const auto& e : track.events) {
                if (!isPb(e)) continue;
                const int v14 = (int)e.data1 | ((int)e.data2 << 7);
                const float x = originX + (float)e.tick * zoom;
                const float y = pp0.y + paintH * (1.0f - (float)v14 / 16383.0f);
                if (prevX >= 0.0f) {
                    vdl->AddLine(ImVec2(prevX, prevY), ImVec2(x, prevY), pbCol, 1.6f);
                    vdl->AddLine(ImVec2(x, prevY), ImVec2(x, y), pbCol, 1.6f);
                }
                vdl->AddCircleFilled(ImVec2(x, y), 2.6f, pbCol);
                prevX = x;
                prevY = y;
            }
            if (prevX >= 0.0f)
                vdl->AddLine(ImVec2(prevX, prevY), ImVec2(vp0.x + laneW, prevY), pbCol, 1.6f);
            // 우클릭: 전체 삭제
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("pblanectx");
            if (ImGui::BeginPopup("pblanectx")) {
                if (ImGui::MenuItem("피치 벤드 전체 삭제")) {
                    state.snapshot();
                    track.events.erase(
                        std::remove_if(track.events.begin(), track.events.end(), isPb),
                        track.events.end());
                    // 눌려 있던 벤드를 중앙으로 되돌린다
                    if (state.output && state.output->isOpen())
                        state.output->send({pbStatus, 0, 64});
                    refreshPlaybackIfPlaying(state);
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("드래그: 피치 벤드 그리기 (가운데 = 0)\n"
                                  "우클릭: 전체 삭제 · 범위는 악기의 벤드 설정을 따릅니다");
        } else if (state.prLaneCc < 0) {
            // ── 벨로시티: 노트마다 세로 막대, 드래그로 세기 조절 ──
            constexpr float kBarW = 6.0f;
            for (const auto& n : notes) {
                const float bx = originX + n.startTick * zoom;
                if (bx + kBarW < vp0.x || bx > vp0.x + laneW) continue; // 화면 밖
                const float h = paintH * (float)n.velocity / 127.0f;
                const bool isSel = state.selectedNotes.count({n.note, n.startTick}) != 0;

                // 드래그 중 막대 x 범위에 마우스가 있으면 세기 갱신 (선택 노트면 선택 전체)
                if (laneActive && vm.x >= bx - 1.0f && vm.x <= bx + kBarW + 1.0f) {
                    const uint8_t nv = (uint8_t)std::clamp(
                        (int)std::lround((1.0f - (vm.y - pp0.y) / paintH) * 127.0f), 1, 127);
                    if (nv != n.velocity) {
                        if (isSel) {
                            for (const auto& s : notes)
                                if (state.selectedNotes.count({s.note, s.startTick}))
                                    seq::setNoteVelocity(track, s, nv);
                        } else {
                            seq::setNoteVelocity(track, n, nv);
                        }
                        state.lastNoteVelocity = nv; // Shift+클릭 복제에 이 세기를 쓴다
                    }
                }

                const ImU32 col =
                    isSel ? IM_COL32(250, 200, 90, 235) : IM_COL32(90, 170, 250, 235);
                vdl->AddRectFilled(ImVec2(bx, pp0.y + paintH - h),
                                   ImVec2(bx + kBarW, pp0.y + paintH), col);
                vdl->AddRectFilled(ImVec2(bx, pp0.y + paintH - h - 2.0f),
                                   ImVec2(bx + kBarW, pp0.y + paintH - h),
                                   IM_COL32(255, 255, 255, 200));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("드래그: 세기 조절 (위=강하게) · 선택된 노트는 함께 바뀝니다\n"
                                  "우클릭: 크레센도/랜덤/일괄 설정 도구");

            // 우클릭: 벨로시티 도구 (선택 노트, 없으면 전체)
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("velctx");
            if (ImGui::BeginPopup("velctx")) {
                static int s_v0 = 60, s_v1 = 120, s_rand = 10, s_setAll = 100;
                // 대상: 선택 노트(없으면 전체)를 시작 틱 순으로
                auto targets = state.selectedNotes.empty()
                                   ? notes
                                   : gatherSelected(state, track);
                std::sort(targets.begin(), targets.end(),
                          [](const seq::NoteSpan& a, const seq::NoteSpan& b) {
                              return a.startTick < b.startTick;
                          });
                ImGui::TextDisabled("대상: %s %d개",
                                    state.selectedNotes.empty() ? "전체" : "선택",
                                    (int)targets.size());
                ImGui::Separator();
                ImGui::SetNextItemWidth(70);
                ImGui::DragInt("##v0", &s_v0, 1, 1, 127, "시작 %d");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragInt("##v1", &s_v1, 1, 1, 127, "끝 %d");
                ImGui::SameLine();
                if (ImGui::Button("램프 적용") && !targets.empty()) {
                    // 크레센도/디크레센도: 첫 노트부터 끝 노트까지 선형 변화
                    state.snapshot();
                    const int n = (int)targets.size();
                    for (int ti2 = 0; ti2 < n; ++ti2) {
                        const float f = n > 1 ? (float)ti2 / (float)(n - 1) : 0.0f;
                        const int v = std::clamp(
                            (int)std::lround(s_v0 + (s_v1 - s_v0) * f), 1, 127);
                        seq::setNoteVelocity(track, targets[(std::size_t)ti2], (uint8_t)v);
                    }
                    refreshPlaybackIfPlaying(state);
                    state.statusMessage = "벨로시티 램프: " + std::to_string(n) + "개";
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("첫 노트=시작 값, 마지막 노트=끝 값으로 선형 변화\n"
                                      "(시작<끝 = 크레센도, 시작>끝 = 디크레센도)");
                ImGui::SetNextItemWidth(70);
                ImGui::DragInt("##vr", &s_rand, 1, 0, 40, "±%d");
                ImGui::SameLine();
                if (ImGui::Button("랜덤 적용") && !targets.empty()) {
                    state.snapshot();
                    uint32_t rng = 0x9E3779B9u ^ (uint32_t)targets.size();
                    for (const auto& nsp : targets) {
                        rng = rng * 1664525u + 1013904223u;
                        const int d = (int)(rng % (uint32_t)(s_rand * 2 + 1)) - s_rand;
                        seq::setNoteVelocity(
                            track, nsp,
                            (uint8_t)std::clamp((int)nsp.velocity + d, 1, 127));
                    }
                    refreshPlaybackIfPlaying(state);
                    state.statusMessage = "벨로시티 랜덤: ±" + std::to_string(s_rand);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("세기를 ±범위 안에서 무작위로 흔듭니다 (사람 느낌)");
                ImGui::SetNextItemWidth(70);
                ImGui::DragInt("##vs", &s_setAll, 1, 1, 127, "%d");
                ImGui::SameLine();
                if (ImGui::Button("일괄 설정") && !targets.empty()) {
                    state.snapshot();
                    for (const auto& nsp : targets)
                        seq::setNoteVelocity(track, nsp, (uint8_t)s_setAll);
                    refreshPlaybackIfPlaying(state);
                    state.statusMessage = "벨로시티 일괄: " + std::to_string(s_setAll);
                }
                ImGui::EndPopup();
            }
        } else {
            // ── CC 곡선: 드래그로 그리면 CC 이벤트가 깔린다 (계단형으로 재생) ──
            const uint8_t ccNum = (uint8_t)state.prLaneCc;
            const uint8_t ccStatus =
                (uint8_t)(midi::kStatusControlChange | (track.channel & 0x0F));
            const auto isThisCc = [&](const seq::MidiEvent& e) {
                return (e.status & 0xF0) == midi::kStatusControlChange && e.data1 == ccNum;
            };

            if (laneActive) {
                const double tickF = std::max(0.0, (double)(vm.x - originX) / zoom);
                const uint32_t t = (uint32_t)tickF;
                const int val = std::clamp(
                    (int)std::lround((1.0f - (vm.y - pp0.y) / paintH) * 127.0f), 0, 127);
                // 같은 CC의 주변 이벤트를 지우고 새 값으로 (드래그 = 곡선 다시 그리기)
                const uint32_t win = (uint32_t)std::max(1, state.song.ppqn / 16);
                track.events.erase(
                    std::remove_if(track.events.begin(), track.events.end(),
                                   [&](const seq::MidiEvent& e) {
                                       return isThisCc(e) && e.tick + win >= t &&
                                              e.tick <= t + win;
                                   }),
                    track.events.end());
                seq::MidiEvent e;
                e.tick = t;
                e.status = ccStatus;
                e.data1 = ccNum;
                e.data2 = (uint8_t)val;
                track.events.push_back(e);
                track.sortEvents();
                // 즉시 전송해 그리는 동안 소리로 확인할 수 있게 한다
                if (state.output && state.output->isOpen())
                    state.output->send({ccStatus, ccNum, (uint8_t)val});
            }

            // 그리기: 계단(값 유지) 라인 + 점. 마지막 값은 오른쪽 끝까지 이어진다.
            const ImU32 lineCol = IM_COL32(170, 130, 250, 235);
            float prevX = -1.0f, prevY = 0.0f;
            for (const auto& e : track.events) {
                if (!isThisCc(e)) continue;
                const float x = originX + (float)e.tick * zoom;
                const float y = pp0.y + paintH * (1.0f - (float)e.data2 / 127.0f);
                if (prevX >= 0.0f) { // 이전 값을 이 지점까지 유지 (계단)
                    vdl->AddLine(ImVec2(prevX, prevY), ImVec2(x, prevY), lineCol, 1.6f);
                    vdl->AddLine(ImVec2(x, prevY), ImVec2(x, y), lineCol, 1.6f);
                }
                vdl->AddCircleFilled(ImVec2(x, y), 2.6f, lineCol);
                prevX = x;
                prevY = y;
            }
            if (prevX >= 0.0f)
                vdl->AddLine(ImVec2(prevX, prevY), ImVec2(vp0.x + laneW, prevY), lineCol, 1.6f);

            // 우클릭: 이 CC 전체 삭제
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("cclanectx");
            if (ImGui::BeginPopup("cclanectx")) {
                if (ImGui::MenuItem("이 컨트롤 이벤트 전체 삭제")) {
                    state.snapshot();
                    track.events.erase(
                        std::remove_if(track.events.begin(), track.events.end(), isThisCc),
                        track.events.end());
                    refreshPlaybackIfPlaying(state);
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("드래그: %s 곡선 그리기 (위=127) · 우클릭: 전체 삭제",
                                  kLaneNames[laneSel]);
        }

        // 놓을 때 한 번만 재생 스냅샷 갱신 (드래그 중 매 프레임 재시작 방지)
        if (ImGui::IsItemDeactivated()) refreshPlaybackIfPlaying(state);
        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace midipro::gui
