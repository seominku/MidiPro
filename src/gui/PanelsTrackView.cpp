// =============================================================
// MidiPro - gui/PanelsTrackView.cpp
// 트랙 목록 창 + 트랙 뷰(레인/클립/오토메이션/루프 띠/마커)
// Panels.cpp에서 분리 (동작 동일). 공유 헬퍼는 PanelsInternal.h 참고.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "audio/BuiltinFx.h"
#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace midipro::gui {

// FX 체인 박스 호버 (이번 프레임 수집 -> 다음 프레임 휠 줌 판정에 사용).
// 박스가 줌 핸들러보다 늦게 그려져서 한 프레임 지연 판정이 필요하다.
static bool g_fxBoxHoveredNow = false;

// 트랙 뷰 왼쪽 헤더 열 폭 (요청으로 210→260 확대, DPI 배율 포함)
static float kTrackHdrW() { return 260.0f * uiDpiScale(); }

// 스냅 격자 한 칸의 틱 수 (트랙 뷰 눈금·스냅 공용). 박자표(tpb)를 나눠 쓴다.
static uint32_t trackSnapTicks(const AppState& state, uint32_t tpb) {
    const uint32_t beat = std::max<uint32_t>(1, (uint32_t)state.song.ppqn);
    switch (state.trackSnapDiv) {
    case 0: return std::max<uint32_t>(1, tpb);       // 1마디
    case 1: return std::max<uint32_t>(1, tpb / 2);   // 1/2마디
    case 2: return beat;                             // 1박
    case 3: return std::max<uint32_t>(1, beat / 2);  // 1/2박
    case 4: return std::max<uint32_t>(1, beat / 4);  // 1/4박(16분)
    default: return beat;
    }
}

// 스냅이 켜져 있으면 틱을 격자에 반올림한다. 꺼져 있으면 그대로.
static uint32_t applyTrackSnap(const AppState& state, uint32_t tpb, long tick) {
    if (!state.trackSnap) return (uint32_t)std::max<long>(0, tick);
    const long g = (long)trackSnapTicks(state, tpb);
    if (g <= 0) return (uint32_t)std::max<long>(0, tick);
    return (uint32_t)std::max<long>(0, ((tick + g / 2) / g) * g);
}

// ---------------------------------------------------------
// 트랙 FX 체인 박스 (트랙 뷰 · 기타 연습 창 공용)
// ---------------------------------------------------------

// 박스 안: [+] 메뉴 + 악기 1줄 + 이펙트 줄들 (더블클릭=편집기, 우클릭=메뉴).
// withInstrument=false면 이펙트만 다룬다 (연습 창: 기타 톤용 FX 체인).
// [+] 추가 버튼 + 악기/FX/내장 이펙트/프리즈 메뉴.
// 트랙 뷰 FX 박스와 믹서·채널 FX 박스(drawFxChainBox)가 같은 메뉴를 공유한다.
void drawFxAddButton(AppState& state, int trackIndex, bool withInstrument) {
    if (!state.vst || trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto& track = state.song.tracks[(std::size_t)trackIndex];
    const int i = trackIndex;
    const int ch = track.channel & 0x0F;
    // [+] : 악기/FX/프리즈 선택 메뉴
    if (ImGui::SmallButton("+")) {
        // 연습 트랙은 MIDI 쪽 선택을 건드리지 않는다 (트랙 뷰에 없는 트랙이다)
        if (!track.practice) state.selectedTrack = i;
        if (!state.vstInstrumentsFiltered) {
            state.vstInstrumentsOnly.clear();
            for (const auto& e : state.vstScanned)
                if (state.vst->pluginHasInstrumentClass(e.path))
                    state.vstInstrumentsOnly.push_back(e);
            state.vstInstrumentsFiltered = true;
        }
        if (!state.vstEffectsFiltered) {
            // 악기 전용 번들(예: Surge XT)은 오디오 입력이 없어 트랙을
            // 무음으로 만든다. 이펙트 클래스가 있는 것만 남긴다.
            state.vstEffectsOnly.clear();
            for (const auto& e : state.vstScanned)
                if (state.vst->pluginHasEffectClass(e.path))
                    state.vstEffectsOnly.push_back(e);
            state.vstEffectsFiltered = true;
        }
        ImGui::OpenPopup("addmenu");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(withInstrument ? "추가: 악기(VSTi) / FX(이펙트) / 프리즈"
                                         : "추가: FX(이펙트) — 앰프 시뮬·오버드라이브 등");
    if (ImGui::BeginPopup("addmenu")) {
        if (withInstrument && ImGui::BeginMenu("악기 (VSTi)")) {
            if (state.vstScanned.empty())
                ImGui::TextDisabled("VST3 창에서 '플러그인 검색'을 먼저 하세요");
            else if (state.vstInstrumentsOnly.empty())
                ImGui::TextDisabled("악기로 쓸 수 있는 VST3가 없습니다");
            for (const auto& e : state.vstInstrumentsOnly) {
                if (!ImGui::MenuItem(e.name.c_str())) continue;
                std::string err;
                if (state.vst->loadTrackInstrument(ch, e.path, -1, err)) {
                    // 저장 목록 갱신 (악기 항목은 트랙당 1개)
                    for (auto it = track.plugins.begin(); it != track.plugins.end();)
                        it = it->isInstrument ? track.plugins.erase(it) : std::next(it);
                    seq::TrackPlugin pl;
                    pl.name = state.vst->trackInstrumentName(ch);
                    if (pl.name.empty()) pl.name = e.name;
                    pl.path = e.path;
                    pl.classIndex = -1;
                    pl.isInstrument = true;
                    pl.enabled = true;
                    state.statusMessage = "트랙 악기: " + pl.name;
                    track.plugins.push_back(std::move(pl));
                } else {
                    state.statusMessage = e.name + " - 악기 로드 실패: " + err;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("FX (이펙트)")) {
            if (state.vstScanned.empty())
                ImGui::TextDisabled("VST3 창에서 '플러그인 검색'을 먼저 하세요");
            else if (state.vstEffectsOnly.empty())
                ImGui::TextDisabled("이펙트로 쓸 수 있는 VST3가 없습니다\n"
                                    "(악기 전용 번들은 트랙 이펙트로 못 씁니다)");
            for (const auto& e : state.vstEffectsOnly) {
                if (!ImGui::MenuItem(e.name.c_str())) continue;
                std::string err;
                // classIndex = -1: 번들에서 이펙트 클래스를 자동 선택
                if (state.vst->loadTrackEffect(ch, e.path, -1, err)) {
                    seq::TrackPlugin pl;
                    pl.name =
                        state.vst->trackEffectName(ch, state.vst->trackEffectCount(ch) - 1);
                    if (pl.name.empty()) pl.name = e.name;
                    pl.path = e.path;
                    pl.classIndex = -1; // 복원 때도 자동 선택
                    pl.isInstrument = false;
                    pl.enabled = true;
                    track.plugins.push_back(std::move(pl));
                    state.statusMessage = "트랙 이펙트 추가: " + pl.name;
                } else {
                    state.statusMessage = e.name + " - 추가 실패: " + err;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("내장 이펙트")) {
            // VST 없이 쓰는 기본 이펙트. VST와 같은 체인에 순서대로 걸린다.
            for (int bt = 0; bt < audio::BuiltinFx::kTypes; ++bt) {
                if (!ImGui::MenuItem(audio::BuiltinFx::typeName(bt))) continue;
                if (state.vst->addBuiltinTrackEffect(ch, bt)) {
                    const int idx = state.vst->trackEffectCount(ch) - 1;
                    seq::TrackPlugin pl;
                    pl.name = audio::BuiltinFx::typeName(bt);
                    if (auto* bf = state.vst->trackEffectBuiltin(ch, idx))
                        pl.path = builtinFxPathString(*bf);
                    pl.classIndex = -1;
                    pl.isInstrument = false;
                    pl.enabled = true;
                    track.plugins.push_back(std::move(pl));
                    state.builtinFxCh = ch; // 파라미터 창 바로 열기
                    state.builtinFxIdx = idx;
                    state.statusMessage =
                        std::string("내장 이펙트 추가: ") + audio::BuiltinFx::typeName(bt);
                }
            }
            ImGui::EndMenu();
        }
        if (withInstrument) {
            ImGui::Separator();
            if (ImGui::MenuItem(track.frozen ? "프리즈 해제"
                                             : "프리즈 (MIDI를 오디오로 굽기)")) {
                if (track.frozen) unfreezeTrack(state, i);
                else freezeTrack(state, i);
            }
        }
        ImGui::EndPopup();
    }

}
void drawTrackFxChain(AppState& state, int trackIndex, float boxW, float boxH,
                      bool withInstrument) {
    if (!state.vst || trackIndex < 0 || trackIndex >= (int)state.song.tracks.size())
        return;
    auto& track = state.song.tracks[(std::size_t)trackIndex];
    const int i = trackIndex;
    const int ch = track.channel & 0x0F;
    // 저장 목록(track.plugins)엔 악기 항목이 섞여 있어, 체인의 fi번째
    // 이펙트가 목록의 몇 번째인지 이펙트만 세어 찾는다.
    const auto fxPluginIndex = [&track](int wantFx) {
        int k = 0;
        for (int idx = 0; idx < (int)track.plugins.size(); ++idx) {
            if (track.plugins[(std::size_t)idx].isInstrument) continue;
            if (k == wantFx) return idx;
            ++k;
        }
        return -1;
    };

    ImGui::PushID(trackIndex);
    ImGui::BeginChild("fxbox", ImVec2(boxW, boxH), true);
    // FX 박스 위에서는 휠이 "박스 스크롤"이어야 한다 — 확대/축소 핸들러가
    // 건너뛰도록 호버를 알린다 (다음 프레임 판정용)
    if (ImGui::IsWindowHovered()) g_fxBoxHoveredNow = true;
    drawFxAddButton(state, trackIndex, withInstrument);

    // 악기 줄 ([+] 옆)
    if (withInstrument) {
        ImGui::SameLine();
        if (state.vst->trackInstrumentActive(ch)) {
            const std::string inm = state.vst->trackInstrumentName(ch);
            ImGui::TextColored(ImVec4(0.75f, 0.6f, 1.0f, 1.0f), "[i] %s", inm.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n더블클릭: 편집기 · 우클릭: 메뉴", inm.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (auto* h = state.vst->trackInstrumentHost(ch)) h->openEditor();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("instctx");
            if (ImGui::BeginPopup("instctx")) {
                if (ImGui::MenuItem("편집기 열기")) {
                    if (auto* h = state.vst->trackInstrumentHost(ch)) h->openEditor();
                }
                if (ImGui::MenuItem("악기 제거")) {
                    state.vst->clearTrackInstrument(ch);
                    for (auto it = track.plugins.begin(); it != track.plugins.end();)
                        it = it->isInstrument ? track.plugins.erase(it) : std::next(it);
                }
                ImGui::EndPopup();
            }
        } else {
            ImGui::TextDisabled(track.frozen ? "(프리즈됨)" : "비어 있음");
        }
    }

    // 이펙트 줄들: [켜기] 이름 (더블클릭 = 편집기, 우클릭 = 메뉴)
    const int nfx = state.vst->trackEffectCount(ch);
    int removeFi = -1;
    for (int fi = 0; fi < nfx; ++fi) {
        ImGui::PushID(fi);
        bool on = state.vst->trackEffectEnabled(ch, fi);
        if (ImGui::Checkbox("##fxon", &on)) { // 실시간 바이패스
            state.vst->setTrackEffectEnabled(ch, fi, on);
            const int pidx = fxPluginIndex(fi);
            if (pidx >= 0) track.plugins[(std::size_t)pidx].enabled = on;
        }
        ImGui::SameLine(0.0f, 4.0f);
        const std::string nm = state.vst->trackEffectName(ch, fi);
        ImGui::TextColored(on ? ImVec4(1.0f, 0.78f, 0.45f, 1.0f)
                              : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "%s", nm.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n더블클릭: 편집기 · 우클릭: 메뉴", nm.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (auto* h = state.vst->trackEffectHost(ch, fi)) h->openEditor();
            else if (state.vst->trackEffectBuiltin(ch, fi)) {
                state.builtinFxCh = ch; // 내장 이펙트: 파라미터 창
                state.builtinFxIdx = fi;
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("fxctx");
        if (ImGui::BeginPopup("fxctx")) {
            if (ImGui::MenuItem("편집기 열기")) {
                if (auto* h = state.vst->trackEffectHost(ch, fi)) h->openEditor();
                else if (state.vst->trackEffectBuiltin(ch, fi)) {
                    state.builtinFxCh = ch;
                    state.builtinFxIdx = fi;
                }
            }
            if (ImGui::MenuItem("삭제")) removeFi = fi;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeFi >= 0) {
        const int pidx = fxPluginIndex(removeFi);
        state.vst->removeTrackEffect(ch, removeFi);
        if (pidx >= 0) track.plugins.erase(track.plugins.begin() + pidx);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

// ---------------------------------------------------------
// 트랙 목록
// ---------------------------------------------------------

void drawTrackList(AppState& state) {
    if (!state.showTracks) return;
    ImGui::Begin("트랙", &state.showTracks);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    if (ImGui::Button("+ 트랙 추가")) ImGui::OpenPopup("addtrackmenu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("일반 트랙 또는 드럼 트랙을 만듭니다");
    if (ImGui::BeginPopup("addtrackmenu")) {
        if (ImGui::MenuItem("일반 트랙")) addTrack(state);
        if (ImGui::MenuItem("드럼 트랙")) addDrumTrack(state); // 채널 10 + 에디터 열기
        if (ImGui::MenuItem("기타 연습 트랙")) addGuitarTrack(state); // + 타브 악보 창
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("데모 채우기") && !state.song.tracks.empty()) {
        state.snapshot();
        // C 메이저 스케일 한 마디를 선택 트랙에 넣는다 (테스트용)
        auto& t = state.song.tracks[state.selectedTrack];
        t.events.clear();
        const uint8_t scale[8] = {60, 62, 64, 65, 67, 69, 71, 72};
        const uint32_t step = state.song.ppqn / 2; // 8분음표
        for (int i = 0; i < 8; ++i) t.addNote(i * step, step, scale[i], 100);
        t.sortEvents();
        state.statusMessage = "데모 스케일을 추가했습니다";
    }
    ImGui::SameLine();
    if (ImGui::Button("오디오 임포트")) state.audioImportRequested = true; // 선택 트랙에 붙임
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MP3 / WAV / FLAC");

    ImGui::Separator();

    // 이름(왼쪽, 넘치면 가로 스크롤) + 제어(뮤트/채널/레벨, 오른쪽 고정) 2열 표.
    // 이름이 길어도 제어가 밀리거나 겹치지 않고, 이름은 스크롤로 확인한다.
    constexpr float kCtrlColW = 205.0f; // 뮤트 + ch + 미니 레벨 미터 칸 폭
    if (!state.song.tracks.empty() &&
        ImGui::BeginTable("tracklist", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("제어", ImGuiTableColumnFlags_WidthFixed, kCtrlColW);

        for (int i = 0; i < (int)state.song.tracks.size(); ++i) {
            auto& track = state.song.tracks[i];
            if (track.practice) continue; // 연습 트랙은 '기타 연습' 창에서만
            ImGui::TableNextRow();
            ImGui::PushID(i);

            // ── 왼쪽: 이름 (칸보다 길면 잘리고, 마우스 오버 시 전체 이름 툴팁) ──
            ImGui::TableSetColumnIndex(0);
            const bool selected = (i == state.selectedTrack);
            trackTypeBadge(track); // [드럼]/[기타] 유형 표시
            // 표 셀이 자동으로 클리핑하므로 이름이 길면 잘려 보인다.
            if (ImGui::Selectable(track.name.c_str(), selected)) {
                state.selectedTrack = i;
                // Shift+클릭 = 타브 창 표시 목록에 토글 (클릭 순서대로)
                if (ImGui::GetIO().KeyShift) toggleTabTrack(state, i);
                // 드럼 트랙이면 드럼 에디터도 띄운다
                if ((track.channel & 0x0F) == 9) state.showDrums = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", track.name.c_str());
            // 우클릭 -> 삭제 메뉴
            if (ImGui::BeginPopupContextItem("trkctx")) {
                state.selectedTrack = i;
                if (ImGui::MenuItem("이 트랙에 MIDI 불러오기...")) {
                    // 실제 파일 열기·병합은 App이 처리한다 (재생 위치부터 얹는다)
                    state.midiImportTrack = i;
                    state.midiImportRequested = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(".mid 파일을 트랙 뷰의 트랙 위로 끌어다 놓아도 됩니다");
                ImGui::Separator();
                if (ImGui::MenuItem("트랙 삭제")) deleteTrack(state, i);
                ImGui::EndPopup();
            }

            // ── 오른쪽: 뮤트 + 채널 ──
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("뮤트", &track.muted);
            ImGui::SameLine();
            int ch = track.channel + 1;
            ImGui::SetNextItemWidth(60);
            if (ImGui::InputInt("ch", &ch, 0, 0)) { // 0,0 = +/- 버튼 없이 폭 절약
                ch = std::clamp(ch, 1, 16);
                track.channel = (uint8_t)(ch - 1);
            }
            // 미니 레벨 미터 (이 트랙의 버스 신호, 포스트 FX·페이더)
            ImGui::SameLine();
            const int bus = track.channel & 0x0F;
            miniMeterH("##lvl", state.meterBus[bus], state.busPeakCache[bus], 46.0f,
                       ImGui::GetFrameHeight() * 0.62f);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (state.song.tracks.empty())
        ImGui::TextDisabled("트랙이 없습니다. '+ 트랙 추가'를 누르세요.");

    ImGui::End();
}

// ---------------------------------------------------------
// 트랙 뷰: 왼쪽 헤더(이름/뮤트/볼륨) + 오른쪽 타임라인(파형/노트)
// ---------------------------------------------------------
void drawTrackView(AppState& state) {
    if (!state.showTrackView) return;
    ImGui::Begin("트랙 뷰", &state.showTrackView);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // 상단 컨트롤
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("확대##tv", &state.pianoRollZoom, 0.02f, 2.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("+ 트랙##tv")) ImGui::OpenPopup("addtrackmenu_tv");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("일반 트랙 또는 드럼 트랙을 만듭니다");
    if (ImGui::BeginPopup("addtrackmenu_tv")) {
        if (ImGui::MenuItem("일반 트랙")) addTrack(state);
        if (ImGui::MenuItem("드럼 트랙")) addDrumTrack(state); // 채널 10 + 에디터 열기
        if (ImGui::MenuItem("기타 연습 트랙")) addGuitarTrack(state); // + 타브 악보 창
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("오디오 임포트##tv")) state.audioImportRequested = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MP3 / WAV / FLAC");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const char* kAutoModes[] = {"오토메이션: 끔",     "볼륨 곡선 그리기",
                                "팬 곡선 그리기",     "모듈레이션 (CC1)",
                                "익스프레션 (CC11)",  "서스테인 (CC64)",
                                "필터 (CC74)"};
    ImGui::Combo("##autolane", &state.autoLane, kAutoModes, 7);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("켜면 레인 드래그가 곡선 그리기로 바뀝니다 (끄면 일반 편집).\n"
                          "볼륨/팬 곡선은 재생/내보내기에서 페이더 대신 쓰입니다.\n"
                          "CC 곡선은 실제 CC 이벤트를 심습니다 (VST 악기에 적용).");

    // 축소/확대 버튼 (휠 없이도 조절). 한 번에 약 15%씩.
    ImGui::SameLine();
    ImGui::TextUnformatted("| 보기:");
    ImGui::SameLine();
    if (ImGui::Button("축소##tvzo"))
        state.pianoRollZoom = std::clamp(state.pianoRollZoom / 1.3f, 0.02f, 2.0f);
    ImGui::SameLine();
    if (ImGui::Button("확대##tvzi"))
        state.pianoRollZoom = std::clamp(state.pianoRollZoom * 1.3f, 0.02f, 2.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("(휠: 확대/축소)");

    // ── 스냅: 클립/구간을 격자에 맞춘다. 격자 눈금도 이 설정대로 그린다 ──
    ImGui::SameLine();
    ImGui::TextUnformatted("| 스냅:");
    ImGui::SameLine();
    ImGui::Checkbox("##snapon", &state.trackSnap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("켜면 클립을 옮길 때 아래 격자에 딱 맞춰집니다.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    const char* kSnapDivs[] = {"1마디", "1/2", "1박", "1/2박", "1/4박"};
    ImGui::Combo("##snapdiv", &state.trackSnapDiv, kSnapDivs, 5);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("격자 간격. 세로선이 이 간격으로 그려지고, 스냅도 여기에 맞춥니다.");

    // ASIO 장치/버퍼 설정은 '설정 > 개인설정 > ASIO' 탭으로 옮겼다.

    // 마우스 휠로 확대/축소 (트랙 뷰 위에서). 휠을 소비해 표가 스크롤되지 않게 한다.
    // 단, FX 체인 박스 위에서는 휠이 박스 스크롤로 가야 하므로 건너뛴다.
    // (박스는 이 아래에서 그려지므로 "지난 프레임" 호버 결과로 판정한다)
    const bool fxBoxHovered = g_fxBoxHoveredNow;
    g_fxBoxHoveredNow = false; // 이번 프레임 수집 시작
    if (!fxBoxHovered && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+휠 = 가로 스크롤 (미니맵과 같은 요청 메커니즘으로 표에 전달)
                state.tvScrollReq = std::max(0.0f, state.tvScrollX - wheel * 160.0f);
            } else if (ImGui::GetIO().KeyShift) {
                // Shift+휠 = 세로 스크롤. 트랙 뷰 "전용" 요청을 쓴다 —
                // keyScrollY는 방향키용 공유 변수라 피아노 롤도 함께 움직인다.
                state.tvScrollYDelta += -wheel * 90.0f;
            } else {
                state.pianoRollZoom =
                    std::clamp(state.pianoRollZoom * std::pow(1.15f, wheel), 0.02f, 2.0f);
            }
            ImGui::GetIO().MouseWheel = 0.0f;
        }
    }

    const float zoom = state.pianoRollZoom;
    const uint32_t tpb = songTicksPerBar(state);
    const uint32_t songLen = state.timelineBars * tpb; // 공용 타임라인 마디 수
    const float timelineW = songLen * zoom + 40.0f;
    // 헤더(이름/컨트롤/FX 박스)가 들어갈 만큼 확보. 레인 그림도 같은 높이를 쓴다.
    const float laneH = 100.0f * uiDpiScale();

    // ── 곡 미니맵: 곡 전체를 축소한 띠. 클릭/드래그로 뷰가 그 위치로 이동한다 ──
    {
        const float miniH = 34.0f;
        const float miniW = std::max(50.0f, ImGui::GetContentRegionAvail().x);
        const uint32_t totalTicks = std::max<uint32_t>(songLen, 1);
        const float sc = miniW / (float)totalTicks; // 미니맵 px/tick
        ImDrawList* mdl = ImGui::GetWindowDrawList();
        const ImVec2 mp0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##minimap", ImVec2(miniW, miniH));
        mdl->AddRectFilled(mp0, ImVec2(mp0.x + miniW, mp0.y + miniH), IM_COL32(24, 24, 30, 255),
                           3.0f);
        for (uint32_t t = 0; t <= songLen; t += tpb * 4) // 4마디 눈금
            mdl->AddLine(ImVec2(mp0.x + (float)t * sc, mp0.y),
                         ImVec2(mp0.x + (float)t * sc, mp0.y + miniH),
                         IM_COL32(58, 58, 70, 160));
        // 내용: 트랙마다 얇은 줄 — 클립은 주황 막대, 노트는 파란 점
        const int nTr = (int)state.song.tracks.size();
        if (nTr > 0) {
            const float rowH = (miniH - 4.0f) / (float)nTr;
            for (int ti = 0; ti < nTr; ++ti) {
                const auto& t = state.song.tracks[(std::size_t)ti];
                const float y0 = mp0.y + 2.0f + rowH * (float)ti;
                const float y1 = y0 + std::max(1.5f, rowH - 1.0f);
                for (const auto& cp : t.clips) {
                    if (!cp) continue;
                    const float x0 = mp0.x + (float)cp->startTick * sc;
                    const float x1 = mp0.x + (float)clipEndTick(*cp, state.song) * sc;
                    mdl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 1.5f), y1),
                                       IM_COL32(230, 150, 90, 200));
                }
                for (const auto& e : t.events) {
                    if (!e.isNoteOn()) continue;
                    const float x = mp0.x + (float)e.tick * sc;
                    mdl->AddRectFilled(ImVec2(x, y0), ImVec2(x + 1.2f, y1),
                                       IM_COL32(110, 180, 250, 190));
                }
            }
        }
        if (state.loopEnabled) // 루프 구간
            mdl->AddRectFilled(ImVec2(mp0.x + (float)state.loopStartTick * sc, mp0.y),
                               ImVec2(mp0.x + (float)state.loopEndTick * sc, mp0.y + miniH),
                               IM_COL32(90, 150, 240, 45));
        // 재생 헤드
        mdl->AddLine(ImVec2(mp0.x + (float)state.playPosTick * sc, mp0.y),
                     ImVec2(mp0.x + (float)state.playPosTick * sc, mp0.y + miniH),
                     IM_COL32(255, 90, 90, 230), 1.5f);
        // 지금 보이는 범위 (뷰 창 테두리)
        if (state.tvVisibleW > 0.0f && zoom > 0.0f) {
            const float vx0 = mp0.x + (state.tvScrollX / zoom) * sc;
            const float vx1 = mp0.x + ((state.tvScrollX + state.tvVisibleW) / zoom) * sc;
            mdl->AddRect(ImVec2(vx0, mp0.y + 0.5f),
                         ImVec2(std::min(vx1, mp0.x + miniW), mp0.y + miniH - 0.5f),
                         IM_COL32(230, 230, 245, 210), 2.0f, 0, 1.5f);
        }
        // 클릭/드래그: 그 지점이 화면 가운데 오도록 스크롤 요청 (표가 소비)
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float rel =
                std::clamp((ImGui::GetIO().MousePos.x - mp0.x) / miniW, 0.0f, 1.0f);
            state.tvScrollReq =
                std::max(0.0f, rel * (float)totalTicks * zoom - state.tvVisibleW * 0.5f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("곡 전체 보기 — 클릭/드래그로 이동");
    }

    ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_BordersInner;
    if (ImGui::BeginTable("trackview", 2, flags)) {
        ImGui::TableSetupColumn("트랙", ImGuiTableColumnFlags_WidthFixed, kTrackHdrW());
        ImGui::TableSetupColumn("타임라인", ImGuiTableColumnFlags_WidthFixed, timelineW);
        ImGui::TableSetupScrollFreeze(1, 0); // 헤더 열 고정

        // ImGui는 고정폭 열의 WidthRequest를 첫 프레임에만 init값으로 잡고
        // 이후엔 갱신하지 않는다. 타임라인이 늘어도(피아노 롤은 늘지만) 트랙 뷰가
        // 특정 폭에서 멈추는 문제를 막기 위해 매 프레임 열 폭을 강제로 맞춘다.
        if (ImGuiTable* tbl = ImGui::GetCurrentTable()) {
            if (tbl->Columns.size() > 1) tbl->Columns[1].WidthRequest = timelineW;
            // 휠 스크롤은 전부 우리 핸들러(확대/Shift 세로/Ctrl 가로)가 담당한다.
            // ImGui 기본 처리를 끄지 않으면 Shift+휠을 가로 스크롤로 자동 변환해
            // 표가 먼저 옆으로 굴러가 버린다 (다음 프레임부터 적용되는 플래그).
            if (tbl->InnerWindow)
                tbl->InnerWindow->Flags |= ImGuiWindowFlags_NoScrollWithMouse;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // 드롭 대상 판정용. 매 프레임 0으로 지우면 아직 안 그려진 아래 레인의
        // 좌표가 비어 "아래 트랙으로 이동"이 실패하므로, 지난 프레임 값을 유지한다.
        if (state.laneRects.size() != state.song.tracks.size())
            state.laneRects.assign(state.song.tracks.size(), {});

        for (int i = 0; i < (int)state.song.tracks.size(); ++i) {
            auto& track = state.song.tracks[i];
            if (track.practice) continue; // 연습 트랙은 '기타 연습' 창에서만
            const bool sel = (i == state.selectedTrack);
            const bool recThis = (state.audioRecTrack == i);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, laneH);
            ImGui::PushID(i);
            // 녹음 중인 트랙은 배경을 붉게(깜빡임) 표시해 한눈에 알 수 있게 한다.
            if (recThis) {
                const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(160, 32, 32, (int)(80 + 100 * pulse)));
            }

            // ── 왼쪽: 헤더 ──
            // 배치: 왼쪽 그룹(이름/컨트롤/FX 박스) + 오른쪽 세로 볼륨·레벨 미터.
            // 볼륨/미터는 이름 줄 높이부터 박스 바닥까지 헤더 전체를 세로로 쓴다.
            ImGui::TableSetColumnIndex(0);
            const ImVec2 hp0 = ImGui::GetCursorScreenPos();
            const float S2 = uiDpiScale(); // 헤더 안 고정 크기도 DPI 배율
            const float kVolW = 16.0f * S2, kMeterHdrW = 11.0f * S2, kGap = 4.0f * S2,
                        kBoxH = 56.0f * S2; // FX 박스도 살짝 키움
            const float leftW =
                ImGui::GetContentRegionAvail().x - (kVolW + kMeterHdrW + kGap * 2.0f);
            const float headerH = ImGui::GetFrameHeight() * 2.0f +
                                  ImGui::GetStyle().ItemSpacing.y * 2.0f + kBoxH;
            ImGui::BeginGroup();
            ImGui::AlignTextToFramePadding();
            // 번호 버튼 = 순서 변경 손잡이 (위/아래 드래그로 다른 줄에 놓는다)
            {
                char gripLbl[12];
                std::snprintf(gripLbl, sizeof(gripLbl), "%d", i + 1);
                ImGui::SmallButton(gripLbl);
                if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                    ImGui::SetTooltip("위/아래로 드래그: 트랙 순서 바꾸기");
                if (ImGui::IsItemActive() &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    const float my2 = ImGui::GetIO().MousePos.y;
                    int target = i;
                    for (int li = 0; li < (int)state.laneRects.size() &&
                                     li < (int)state.song.tracks.size();
                         ++li)
                        if (my2 >= state.laneRects[(std::size_t)li].y0 &&
                            my2 < state.laneRects[(std::size_t)li].y1)
                            target = li;
                    state.trackReorderFrom = i;
                    state.trackReorderTo = target;
                    if (target != i) { // 놓일 줄을 파란 테두리로 미리 보여준다
                        const auto& lr = state.laneRects[(std::size_t)target];
                        ImGui::GetForegroundDrawList()->AddRect(
                            ImVec2(ImGui::GetWindowPos().x + 4.0f, lr.y0),
                            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 4.0f,
                                   lr.y1),
                            IM_COL32(120, 200, 255, 220), 0.0f, 0, 2.0f);
                    }
                } else if (!ImGui::IsItemActive() && state.trackReorderFrom == i &&
                           !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // 놓음: 실제 이동은 표 순회가 끝난 뒤 (아래 지연 실행)
                }
            }
            ImGui::SameLine();
            bool m = track.muted;
            if (ImGui::Checkbox("M", &m)) track.muted = m;
            ImGui::SameLine();
            trackTypeBadge(track); // [드럼]/[기타] 유형 표시
            char nbuf[64];
            std::snprintf(nbuf, sizeof(nbuf), "%s", track.name.c_str());
            // 물리 좌표에서 직접 계산한 폭 — DPI 매크로를 우회한다(이중 스케일 방지)
            (ImGui::SetNextItemWidth)(
                std::max(50.0f, hp0.x + leftW - ImGui::GetCursorScreenPos().x));
            if (ImGui::InputText("##name", nbuf, sizeof(nbuf))) track.name = nbuf;
            // ── 컨트롤 줄: 녹음 | ASIO | 입력 (컴팩트) ──
            // ASIO 듀플렉스 스트림은 하나뿐이라 한 번에 한 트랙만 모니터한다.
            // ASIO 장치 선택/검색은 '설정 > 개인설정 > ASIO'에 있다.
            if (state.audioInput) {
                auto* in = state.audioInput;
                const bool asioThis = (state.asioTrack == i);
                const bool activeInput = recThis || asioThis; // 이 트랙이 입력을 점유

                if (recThis) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 40, 40, 255));
                    if (ImGui::SmallButton("■정지")) stopAudioRecording(state);
                    ImGui::PopStyleColor();
                } else if (ImGui::SmallButton("●녹음")) {
                    startAudioRecording(state, i);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("R: 녹음 시작/정지 (시작하면 재생도 함께)\n"
                                      "스페이스: 녹음 중이면 정지");
                ImGui::SameLine();
                if (asioThis) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 130, 90, 255));
                    if (ImGui::SmallButton("ASIO중지")) {
                        in->stopAsio();
                        state.asioTrack = -1;
                        refreshPlaybackIfPlaying(state);
                    }
                    ImGui::PopStyleColor();
                } else {
                    if (ImGui::SmallButton("ASIO")) {
                        // 이미 켜져 있으면 재개폐하지 않고 소유권만 넘긴다 —
                        // ASIO 드라이버 재개폐가 시스템을 멈추게 하던 원인 제거.
                        if (in->startAsio(state.asioDeviceIndex, track.inputChannelMode)) {
                            state.asioTrack = i;
                            state.statusMessage = "ASIO 입력 = 이 트랙 (저지연)";
                        } else {
                            state.statusMessage =
                                "ASIO를 열 수 없습니다 (설정 > 개인설정 > ASIO에서 장치 확인)";
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("ASIO 저지연 입력을 이 트랙으로 잡습니다\n"
                                          "(스트림은 하나 — 다른 트랙에서 누르면 옮겨집니다)");
                }
                ImGui::SameLine();
                // 입력 채널: 1+2 합침 / 1 / 2 (모노 인풋 기타는 보통 1 또는 2)
                const char* chModes[] = {"1+2", "입력1", "입력2"};
                ImGui::SetNextItemWidth(64);
                if (ImGui::Combo("##inch", &track.inputChannelMode, chModes,
                                 IM_ARRAYSIZE(chModes)))
                    if (activeInput) in->setInputChannelMode(track.inputChannelMode);
                if (activeInput) { // 입력 점유 중일 때만 입력 레벨 표시 (짧게)
                    ImGui::SameLine();
                    ImGui::ProgressBar(
                        std::clamp(in->inputLevel(), 0.0f, 1.0f),
                        ImVec2(std::max(20.0f, hp0.x + leftW - ImGui::GetCursorScreenPos().x),
                               ImGui::GetFrameHeight() * 0.5f),
                        "");
                }
            }

            // (오디오 클립 이름/삭제는 헤더에 두지 않는다. 이름은 레인의 클립 블록에
            //  표시되고, 삭제는 레인 우클릭 메뉴나 Del 키로 한다.)

            // ── FX 체인 박스([+]: 악기/FX/프리즈) | 세로 볼륨 | 세로 레벨 미터 ──
            // 박스 안: 악기 1줄 + 이펙트 줄들 (더블클릭 = 편집기, 우클릭 = 메뉴)
            if (state.vst) {
                const float boxW =
                    std::max(60.0f, hp0.x + leftW - ImGui::GetCursorScreenPos().x);
                drawTrackFxChain(state, i, boxW, kBoxH, /*withInstrument=*/true);
            }
            ImGui::EndGroup();

            // 세로 볼륨 슬라이더 + 세로 레벨 미터: 이름 줄 높이부터 헤더 끝까지
            ImGui::SameLine(0.0f, kGap);
            ImGui::VSliderFloat("##vvol", ImVec2(kVolW, headerH), &track.volume, 0.0f, 1.5f,
                                "");
            if (ImGui::IsItemActivated()) state.snapshot();
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetTooltip("볼륨 %.0f%%", track.volume * 100.0f);
            ImGui::SameLine(0.0f, kGap);
            {
                const int mbus = track.channel & 0x0F;
                levelMeterWidget("##hdrmeter", &state.meterBusHdr[mbus],
                                 &state.busPeakCache[mbus], 1, kMeterHdrW, headerH);
            }

            // ── 오른쪽: 타임라인 레인 ──
            ImGui::TableSetColumnIndex(1);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();

            // 재생 헤드(빨간 바) 잡아 끌기: 스크럽 + 가장자리 자동 스크롤.
            // 레인 버튼보다 먼저 제출해 마우스 우선권을 갖는다.
            // (녹음 중이거나 오토메이션 그리기 모드에서는 비활성 — 드래그를 뺏지 않게)
            if (!state.recording && state.autoLane == 0) {
                const float phx = p0.x + (float)state.playPosTick * zoom;
                ImGui::SetCursorScreenPos(ImVec2(phx - 4.0f, p0.y));
                ImGui::PushID(88000 + i);
                ImGui::InvisibleButton("phgrab", ImVec2(8.0f, laneH));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsItemActivated()) {
                    // 재생 중이면 잠깐 멈추고 놓을 때 새 위치부터 이어서
                    state.seekWasPlaying = state.player && state.player->isPlaying();
                    if (state.seekWasPlaying) {
                        state.player->stop();
                        if (state.audioClips) state.audioClips->stopAudio();
                    } else {
                        silenceOutput(state);
                    }
                }
                if (ImGui::IsItemActive()) {
                    const float mxp = ImGui::GetIO().MousePos.x;
                    state.playPosTick = mxp > p0.x ? (uint32_t)((mxp - p0.x) / zoom) : 0;
                    if (state.audioClips)
                        state.audioClips->seekAudio(tickToFrame(state, state.playPosTick));
                    trackViewEdgeScroll(mxp); // 화면 끝에 다가가면 뷰가 옆으로 따라 흐른다
                }
                if (ImGui::IsItemDeactivated() && state.seekWasPlaying) {
                    state.seekWasPlaying = false;
                    startPlayback(state); // 놓으면 새 위치부터 이어서 재생
                }
                ImGui::PopID();
                ImGui::SetCursorScreenPos(p0);
            }

            // 선택된 클립의 페이드 핸들: 클립 위 모서리의 흰 점을 좌우로 드래그해
            // 페이드 인(왼쪽 점)/아웃(오른쪽 점) 길이를 조절한다.
            if (state.autoLane == 0 && i == state.selClipTrack && state.selClipIndex >= 0 &&
                state.selClipIndex < (int)track.clips.size() &&
                track.clips[(std::size_t)state.selClipIndex] &&
                track.clips[(std::size_t)state.selClipIndex]->sampleRate > 0) {
                auto& fclip = *track.clips[(std::size_t)state.selClipIndex];
                const double sSec = seq::songTickToSec(state.song, fclip.startTick);
                const double dSec = fclip.durationSeconds();
                const double eSec = sSec + dSec;
                for (int h = 0; h < 2; ++h) {
                    const double hSec =
                        h == 0 ? sSec + fclip.fadeInSec : eSec - fclip.fadeOutSec;
                    const float hx =
                        p0.x + (float)seq::songSecToTick(state.song, hSec) * zoom;
                    ImGui::PushID(66000 + h);
                    ImGui::SetCursorScreenPos(ImVec2(hx - 6.0f, p0.y + 1.0f));
                    ImGui::InvisibleButton("fadeh", ImVec2(12.0f, 13.0f));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (!ImGui::IsItemActive())
                            ImGui::SetTooltip("드래그: 페이드 %s 길이 (%.2f초)",
                                              h == 0 ? "인" : "아웃",
                                              h == 0 ? fclip.fadeInSec : fclip.fadeOutSec);
                    }
                    if (ImGui::IsItemActivated()) state.snapshot();
                    if (ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                        const float mxf = ImGui::GetIO().MousePos.x;
                        trackViewEdgeScroll(mxf);
                        const double mSec = seq::songTickToSec(
                            state.song, std::max(0.0, (double)(mxf - p0.x) / zoom));
                        if (h == 0)
                            fclip.fadeInSec = std::clamp(
                                mSec - sSec, 0.0,
                                std::max(0.0, dSec - fclip.fadeOutSec - 0.01));
                        else
                            fclip.fadeOutSec = std::clamp(
                                eSec - mSec, 0.0,
                                std::max(0.0, dSec - fclip.fadeInSec - 0.01));
                    }
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(p0);
            }

            // 루프 구간 핸들 (첫 트랙 줄, 루프 켜짐일 때): 파란 띠의 양끝을
            // 드래그해 "틱 단위로 자유롭게" 조절한다. 레인 버튼보다 먼저 = 우선권.
            // (오토메이션 그리기 모드에서는 곡선 드래그를 뺏지 않도록 쉼)
            if (i == 0 && state.loopEnabled && state.autoLane == 0) {
                const uint32_t minLoop = (uint32_t)std::max(1, state.song.ppqn / 8);
                const float lx0 = p0.x + (float)state.loopStartTick * zoom;
                const float lx1 = p0.x + (float)state.loopEndTick * zoom;
                for (int hi = 0; hi < 2; ++hi) {
                    const float hx = hi == 0 ? lx0 : lx1;
                    ImGui::PushID(99000 + hi);
                    ImGui::SetCursorScreenPos(ImVec2(hx - 5.0f, p0.y));
                    ImGui::InvisibleButton("loophandle", ImVec2(10.0f, 18.0f));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (!ImGui::IsItemActive())
                            ImGui::SetTooltip("루프 %s 드래그 (자유 이동)",
                                              hi == 0 ? "시작" : "끝");
                    }
                    if (ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                        const float mxp = ImGui::GetIO().MousePos.x;
                        trackViewEdgeScroll(mxp);
                        const double tick = std::max(0.0, (double)(mxp - p0.x) / zoom);
                        if (hi == 0)
                            state.loopStartTick = (uint32_t)std::min(
                                tick, (double)state.loopEndTick - (double)minLoop);
                        else
                            state.loopEndTick = (uint32_t)std::max(
                                tick, (double)state.loopStartTick + (double)minLoop);
                    }
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(p0);
            }

            // 루프 스트립 (첫 줄 상단 18px): 빈 곳 드래그 = 새 루프 구간 만들기,
            // 파란 띠 가운데 드래그 = 루프 통째로 좌우 이동 (틱 단위, 스냅 없음).
            // (오토메이션 그리기 모드에서는 곡선 드래그를 뺏지 않도록 쉼)
            if (i == 0 && state.autoLane == 0) {
                static int loopDragMode = 0; // 0 없음, 1 새로 만들기, 2 이동
                static double loopAnchorTick = 0.0;
                static double loopGrabOff = 0.0;
                const uint32_t minLoop = (uint32_t)std::max(1, state.song.ppqn / 8);
                ImGui::SetCursorScreenPos(p0);
                ImGui::InvisibleButton("loopstrip", ImVec2(timelineW, 18.0f));
                const float mxs = ImGui::GetIO().MousePos.x;
                const double mTick = std::max(0.0, (double)(mxs - p0.x) / zoom);
                if (ImGui::IsItemActivated()) {
                    const bool inBand = state.loopEnabled &&
                                        mTick >= (double)state.loopStartTick &&
                                        mTick < (double)state.loopEndTick;
                    if (inBand) {
                        loopDragMode = 2;
                        loopGrabOff = mTick - (double)state.loopStartTick;
                    } else {
                        loopDragMode = 1;
                        loopAnchorTick = mTick;
                    }
                }
                if (ImGui::IsItemActive() && loopDragMode != 0 &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
                    trackViewEdgeScroll(mxs);
                    if (loopDragMode == 1) {
                        const double a = std::min(loopAnchorTick, mTick);
                        const double b = std::max(loopAnchorTick, mTick);
                        if (b - a >= (double)minLoop) { // 실제로 끌어야 켜진다
                            state.loopEnabled = true;
                            state.loopStartTick = (uint32_t)a;
                            state.loopEndTick = (uint32_t)b;
                        }
                    } else {
                        const uint32_t len =
                            std::max(minLoop, state.loopEndTick - state.loopStartTick);
                        const double ns = std::max(0.0, mTick - loopGrabOff);
                        state.loopStartTick = (uint32_t)ns;
                        state.loopEndTick = (uint32_t)ns + len;
                    }
                }
                if (ImGui::IsItemDeactivated()) loopDragMode = 0;
                if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                    ImGui::SetTooltip("드래그: 루프 구간 만들기 · 파란 띠 드래그: 이동\n"
                                      "(틱 단위 자유 조절 · 마디 정렬은 트랜스포트 숫자로)");
                ImGui::SetCursorScreenPos(p0);
            }

            // 템포 마커 위젯 (첫 트랙 줄): 드래그 = 위치 이동(스냅 없음), 우클릭 =
            // BPM 편집/삭제. ImGui는 "먼저 제출된 위젯"이 클릭/호버를 선점하므로
            // 레인 버튼보다 먼저 만들어야 마커가 마우스를 받는다. (그림은 뒤에서)
            // 우클릭은 좌클릭과 달리 ActiveId를 잡지 않아 레인에도 새어 들어가므로,
            // 마커가 받았으면 플래그로 레인의 우클릭 메뉴를 막는다.
            // (오토메이션 그리기 모드에서는 마커도 쉼 — 곡선 드래그 우선)
            bool tempoMarkerTookRightClick = false;
            if (i == 0 && !state.song.tempoChanges.empty() && state.autoLane == 0) {
                int removeK = -1;
                for (int k = 0; k < (int)state.song.tempoChanges.size(); ++k) {
                    auto& tc = state.song.tempoChanges[(std::size_t)k];
                    const float x = p0.x + (float)tc.tick * zoom;
                    if (x < p0.x - 60.0f || x > p0.x + timelineW + 60.0f) continue;
                    ImGui::PushID(77000 + k);
                    ImGui::SetCursorScreenPos(ImVec2(x - 5.0f, p0.y));
                    ImGui::InvisibleButton("tdrag", ImVec2(10.0f, laneH));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (!ImGui::IsItemActive())
                            ImGui::SetTooltip("%.0f BPM\n드래그: 이동 · 우클릭: 편집/삭제",
                                              tc.bpm);
                    }
                    if (ImGui::IsItemActivated()) {
                        state.snapshot(); // 드래그 시작 = undo 1회
                        state.selectedTempoMarker = k; // 클릭 = 선택 (흰 아웃라인 + Del 삭제)
                    }
                    if (ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                        const float mxp = ImGui::GetIO().MousePos.x;
                        tc.tick = (uint32_t)std::max(0.0, (double)(mxp - p0.x) / zoom);
                        trackViewEdgeScroll(mxp); // 끝쪽에서 자동 스크롤
                    }
                    if (ImGui::IsItemDeactivated()) { // 드래그 끝: 틱 순서 복구
                        const uint32_t keepTick = tc.tick; // 선택이 정렬 후에도 따라가게
                        std::stable_sort(
                            state.song.tempoChanges.begin(), state.song.tempoChanges.end(),
                            [](const seq::TempoChange& a, const seq::TempoChange& b2) {
                                return a.tick < b2.tick;
                            });
                        for (int m = 0; m < (int)state.song.tempoChanges.size(); ++m)
                            if (state.song.tempoChanges[(std::size_t)m].tick == keepTick) {
                                state.selectedTempoMarker = m;
                                break;
                            }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        state.selectedTempoMarker = k; // 우클릭도 선택으로 취급
                        tempoMarkerTookRightClick = true;
                        ImGui::OpenPopup("tempomarker");
                    }
                    if (ImGui::BeginPopup("tempomarker")) {
                        const seq::BarBeatTick bb = seq::toBarBeatTick(tc.tick, state.song.ppqn);
                        ImGui::TextDisabled("%d마디 %d박부터", bb.bar, bb.beat);
                        double b = tc.bpm;
                        ImGui::SetNextItemWidth(90);
                        if (ImGui::InputDouble("BPM##mk", &b, 0, 0, "%.1f",
                                               ImGuiInputTextFlags_EnterReturnsTrue)) {
                            state.snapshot();
                            tc.bpm = std::clamp(b, 20.0, 300.0);
                        }
                        bool rp = tc.ramp; // 점점 빠르게/느리게 (다음 지점까지 선형)
                        if (ImGui::Checkbox("다음 지점까지 점진 변화", &rp)) {
                            state.snapshot();
                            tc.ramp = rp;
                            refreshPlaybackIfPlaying(state);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("이 지점의 BPM에서 다음 템포 지점의 BPM까지\n"
                                              "서서히 변합니다 (점점 빠르게/느리게).\n"
                                              "다음 지점이 없으면 일정하게 유지됩니다.");
                        if (ImGui::MenuItem("삭제")) {
                            removeK = k;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                if (removeK >= 0) {
                    state.snapshot();
                    state.song.tempoChanges.erase(state.song.tempoChanges.begin() + removeK);
                    state.selectedTempoMarker = -1;
                }
                ImGui::SetCursorScreenPos(p0); // 레인 버튼 위치 복원
            }

            // 구간 마커 위젯 (첫 트랙 줄, 이름표 위치): 클릭 = 그 위치로 점프,
            // 드래그 = 이동, 우클릭 = 이름 바꾸기/삭제
            if (i == 0 && !state.song.markers.empty() && state.autoLane == 0) {
                int removeMk = -1;
                static char mkName[64] = "";
                static bool mkSnapDone = false;
                for (int k = 0; k < (int)state.song.markers.size(); ++k) {
                    auto& mk = state.song.markers[(std::size_t)k];
                    const float x = p0.x + (float)mk.tick * zoom;
                    if (x < p0.x - 120.0f || x > p0.x + timelineW + 120.0f) continue;
                    ImGui::PushID(55000 + k);
                    ImGui::SetCursorScreenPos(ImVec2(x - 5.0f, p0.y + 18.0f));
                    ImGui::InvisibleButton(
                        "mk", ImVec2(12.0f + ImGui::CalcTextSize(mk.name.c_str()).x, 17.0f));
                    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                        ImGui::SetTooltip("%s\n클릭: 이 위치로 이동 · 드래그: 옮기기 · "
                                          "우클릭: 이름/삭제",
                                          mk.name.c_str());
                    if (ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
                        if (!mkSnapDone) {
                            state.snapshot();
                            mkSnapDone = true;
                        }
                        const float mxp = ImGui::GetIO().MousePos.x;
                        trackViewEdgeScroll(mxp);
                        mk.tick = (uint32_t)std::max(0.0, (double)(mxp - p0.x) / zoom);
                    }
                    if (ImGui::IsItemDeactivated()) {
                        if (!mkSnapDone) seekTo(state, mk.tick); // 움직임 없는 클릭 = 점프
                        mkSnapDone = false;
                        const uint32_t keepTick = mk.tick; // 정렬 후에도 선택 유지
                        std::stable_sort(state.song.markers.begin(), state.song.markers.end(),
                                         [](const seq::SectionMarker& a,
                                            const seq::SectionMarker& b2) {
                                             return a.tick < b2.tick;
                                         });
                        for (int m2 = 0; m2 < (int)state.song.markers.size(); ++m2)
                            if (state.song.markers[(std::size_t)m2].tick == keepTick) {
                                state.selectedMarker = m2;
                                break;
                            }
                    }
                    if (ImGui::IsItemActivated()) state.selectedMarker = k; // 클릭 = 선택
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        std::snprintf(mkName, sizeof(mkName), "%s", mk.name.c_str());
                        tempoMarkerTookRightClick = true; // 레인 메뉴가 겹치지 않게
                        ImGui::OpenPopup("mkctx");
                    }
                    if (ImGui::BeginPopup("mkctx")) {
                        ImGui::SetNextItemWidth(150);
                        if (ImGui::InputText("이름##mk", mkName, sizeof(mkName)) &&
                            mkName[0] != '\0')
                            mk.name = mkName;
                        // 구간 복제: 이 마커 ~ 다음 마커(없으면 내용 끝)를 뒤에 삽입
                        if (ImGui::MenuItem("구간 복제 (뒤에 삽입)")) {
                            uint32_t secEnd = 0;
                            for (const auto& m2 : state.song.markers)
                                if (m2.tick > mk.tick && (secEnd == 0 || m2.tick < secEnd))
                                    secEnd = m2.tick;
                            if (secEnd == 0) { // 다음 마커가 없으면 내용 끝(마디 올림)
                                const uint32_t tpbM =
                                    songTicksPerBar(state);
                                const uint32_t cend = contentTicksWithAudio(state);
                                secEnd = cend > mk.tick
                                             ? (cend + tpbM - 1) / tpbM * tpbM
                                             : mk.tick + tpbM * 4;
                            }
                            // 주의: markers 벡터가 바뀌므로 mk 참조는 더 쓰지 않는다
                            duplicateSection(state, state.song.markers[(std::size_t)k].tick,
                                             secEnd);
                            ImGui::CloseCurrentPopup();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "이 마커부터 다음 마커(없으면 곡 끝)까지 —\n"
                                "모든 트랙의 노트/클립/오토메이션/템포를 통째로\n"
                                "바로 뒤에 복사하고, 그 뒤 내용은 그만큼 밀립니다.");
                        if (ImGui::MenuItem("삭제")) removeMk = k;
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                if (removeMk >= 0) {
                    state.snapshot();
                    state.song.markers.erase(state.song.markers.begin() + removeMk);
                    state.selectedMarker = -1;
                }
                ImGui::SetCursorScreenPos(p0);
            }

            // ── MIDI 클립 위젯 (제목 띠): 레인 버튼보다 먼저 제출해 클릭을 선점.
            // 드래그 = 박 단위로 이동 (구간 안 노트/CC가 함께 움직인다), 우클릭 = 메뉴.
            if (state.autoLane == 0 && !track.midiClips.empty()) {
                static int s_mcTrack = -1, s_mcIdx = -1; // 드래그 상태
                static int s_mcMode = 0; // 0=이동, 1=왼끝 리사이즈, 2=오른끝 리사이즈
                static float s_mcDownX = 0.0f;
                static uint32_t s_mcOrigStart = 0;
                static uint32_t s_mcOrigEnd = 0;
                int removeMc = -1;   // -1 없음, 이번 프레임에 삭제할 인덱스
                bool removeNotes = false;
                static char s_mcName[64] = "";
                for (int mi = 0; mi < (int)track.midiClips.size(); ++mi) {
                    auto& mc = track.midiClips[(std::size_t)mi];
                    const float x0 = p0.x + (float)mc.startTick * zoom;
                    const float x1 = p0.x + (float)mc.endTick * zoom;
                    if (x1 < dl->GetClipRectMin().x - 40.0f ||
                        x0 > dl->GetClipRectMax().x + 40.0f)
                        continue;
                    ImGui::PushID(9000 + mi);
                    ImGui::SetCursorScreenPos(ImVec2(x0, p0.y));
                    // 몸통 전체가 손잡이다 (위 14px 띠만으로는 잡기 불편해서)
                    ImGui::InvisibleButton("##mclip",
                                           ImVec2(std::max(10.0f, x1 - x0), laneH - 2.0f));
                    const float mxNow = ImGui::GetIO().MousePos.x;
                    const bool nearL = std::fabs(mxNow - x0) <= 6.0f;
                    const bool nearR = std::fabs(mxNow - x1) <= 6.0f;
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor((nearL || nearR) ? ImGuiMouseCursor_ResizeEW
                                                               : ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s\n드래그: 이동 (위/아래 = 다른 트랙으로) · "
                                          "끝 드래그: 범위 조절\n클릭: 재생 위치 · "
                                          "우클릭: 이름/복제/삭제",
                                          mc.name.c_str());
                    }
                    if (ImGui::IsItemActivated()) {
                        state.selectedTrack = i;
                        if (ImGui::GetIO().KeyCtrl) {
                            // Ctrl+드래그: 클립 위에서도 "구간 선택"이 되게 넘겨준다
                            state.clipRange.active = true;
                            state.clipRange.track = i;
                            state.clipRange.clip = -1;
                            state.clipRange.t0 = state.clipRange.t1 = (uint32_t)std::max(
                                0.0, (double)(mxNow - p0.x) / zoom);
                        } else {
                            state.snapshot(); // 드래그 전체 = 언두 1회
                            state.selMidiClipTrack = i;
                            state.selMidiClipIndex = mi;
                            s_mcTrack = i;
                            s_mcIdx = mi;
                            s_mcMode = nearL ? 1 : (nearR ? 2 : 0);
                            s_mcDownX = mxNow;
                            s_mcOrigStart = mc.startTick;
                            s_mcOrigEnd = mc.endTick;
                        }
                    }
                    if (ImGui::IsItemActive() && s_mcTrack == i && s_mcIdx == mi &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                        trackViewEdgeScroll(mxNow);
                        const float dx = mxNow - s_mcDownX;
                        const long beat = (long)state.song.ppqn;
                        const long dSnap =
                            (long)std::lround((double)dx / zoom / (double)beat) * beat;
                        if (s_mcMode == 1) { // 왼끝: 시작만 조절 (박 스냅, 노트는 그대로)
                            long ns = (long)s_mcOrigStart + dSnap;
                            if (ns < 0) ns = 0;
                            if (ns > (long)mc.endTick - beat) ns = (long)mc.endTick - beat;
                            mc.startTick = (uint32_t)(ns < 0 ? 0 : ns);
                        } else if (s_mcMode == 2) { // 오른끝: 끝만 조절
                            long ne = (long)s_mcOrigEnd + dSnap;
                            if (ne < (long)mc.startTick + beat)
                                ne = (long)mc.startTick + beat;
                            mc.endTick = (uint32_t)ne;
                        } else {
                            // 이동: 드래그 중에는 박스만 움직인다 (노트는 놓을 때 한 번에
                            // — 지나가는 길의 "남의 노트"를 쓸어 담지 않기 위해서).
                            long target = (long)s_mcOrigStart + dSnap;
                            if (target < 0) target = 0;
                            mc.startTick = (uint32_t)target;
                            mc.endTick =
                                (uint32_t)(target + (long)(s_mcOrigEnd - s_mcOrigStart));
                            // 위/아래로 끌면 대상 트랙을 테두리로 알려준다
                            const float myv = ImGui::GetIO().MousePos.y;
                            for (int li = 0; li < (int)state.laneRects.size() &&
                                             li < (int)state.song.tracks.size();
                                 ++li) {
                                if (li == i) continue;
                                if (myv >= state.laneRects[(std::size_t)li].y0 &&
                                    myv < state.laneRects[(std::size_t)li].y1)
                                    dl->AddRect(
                                        ImVec2(p0.x, state.laneRects[(std::size_t)li].y0 + 1),
                                        ImVec2(p0.x + timelineW,
                                               state.laneRects[(std::size_t)li].y1 - 1),
                                        IM_COL32(120, 180, 255, 220), 0.0f, 0, 2.0f);
                            }
                        }
                    }
                    if (ImGui::IsItemDeactivated() && s_mcTrack == i && s_mcIdx == mi) {
                        bool movedAway = false;
                        if (s_mcMode == 0) {
                            const long dT = (long)mc.startTick - (long)s_mcOrigStart;
                            // 위/아래로 끌었으면 다른 트랙으로 내용째 이동
                            int targetLane = i;
                            const float myv = ImGui::GetIO().MousePos.y;
                            for (int li = 0; li < (int)state.laneRects.size() &&
                                             li < (int)state.song.tracks.size();
                                 ++li)
                                if (myv >= state.laneRects[(std::size_t)li].y0 &&
                                    myv < state.laneRects[(std::size_t)li].y1)
                                    targetLane = li;
                            if (targetLane != i &&
                                ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                                auto& dst = state.song.tracks[(std::size_t)targetLane];
                                // 멤버 노트(+원 범위 CC)만 대상 트랙으로 — 남의 노트는
                                // 절대 함께 가지 않는다 (소유 개념)
                                seq::MidiClip movedClip = mc; // 박스는 이미 새 위치
                                seq::moveMidiClipToTrack(track, dst, movedClip,
                                                         s_mcOrigStart, s_mcOrigEnd, dT);
                                track.midiClips.erase(track.midiClips.begin() + mi);
                                dst.midiClips.push_back(movedClip);
                                std::stable_sort(dst.midiClips.begin(), dst.midiClips.end(),
                                                 [](const seq::MidiClip& a,
                                                    const seq::MidiClip& b) {
                                                     return a.startTick < b.startTick;
                                                 });
                                state.selectedTrack = targetLane;
                                state.selMidiClipTrack = targetLane;
                                state.selMidiClipIndex = -1;
                                movedAway = true; // 벡터가 바뀌었으니 루프를 빠져나간다
                            } else if (dT != 0) {
                                // 같은 트랙: 멤버 노트(+원 범위 CC)만 총 이동량만큼
                                seq::shiftMidiClip(track, mc, s_mcOrigStart, s_mcOrigEnd,
                                                   dT);
                            } else if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left,
                                                              4.0f)) {
                                // 안 끌고 놓았다 = 일반 클릭: 재생 위치 이동
                                const uint32_t t =
                                    mxNow > p0.x ? (uint32_t)((mxNow - p0.x) / zoom) : 0;
                                seekTo(state, t, /*scrollView=*/false);
                            }
                        }
                        s_mcTrack = s_mcIdx = -1;
                        refreshPlaybackIfPlaying(state);
                        if (movedAway) {
                            ImGui::PopID();
                            break;
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        std::snprintf(s_mcName, sizeof(s_mcName), "%s", mc.name.c_str());
                        state.selMidiClipTrack = i;
                        state.selMidiClipIndex = mi;
                        tempoMarkerTookRightClick = true; // 레인 메뉴와 겹치지 않게
                        ImGui::OpenPopup("mclipctx");
                    }
                    if (ImGui::BeginPopup("mclipctx")) {
                        ImGui::SetNextItemWidth(150);
                        if (ImGui::InputText("이름##mc", s_mcName, sizeof(s_mcName)) &&
                            s_mcName[0] != '\0')
                            mc.name = s_mcName;
                        if (ImGui::MenuItem("복제 (바로 뒤에)")) {
                            state.snapshot();
                            const uint32_t len = mc.endTick - mc.startTick;
                            // 멤버 노트만 복사 (겹쳐 있는 남의 노트는 안 복사된다)
                            const seq::MidiClip nc = seq::copyMidiClip(track, mc, len);
                            track.midiClips.push_back(nc);
                            std::stable_sort(track.midiClips.begin(), track.midiClips.end(),
                                             [](const seq::MidiClip& a,
                                                const seq::MidiClip& b) {
                                                 return a.startTick < b.startTick;
                                             });
                            refreshPlaybackIfPlaying(state);
                            ImGui::CloseCurrentPopup();
                        }
                        if (ImGui::MenuItem("범위 안 노트 다시 담기")) {
                            // 피아노 롤에서 새로 그렸거나 옮긴 노트를 멤버로 갱신
                            state.snapshot();
                            seq::adoptMidiClipMembers(track, mc);
                            state.statusMessage = "클립 멤버 갱신: " +
                                                  std::to_string(mc.members.size()) + "개";
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "지금 범위 안에 있는 노트들을 이 클립의 소유로 다시\n"
                                "담습니다. (클립은 만들 때 담은 노트만 갖고 다니므로,\n"
                                "안에 새로 그린 노트는 이걸로 넣어주세요)");
                        if (ImGui::MenuItem("클립 해제 (노트 유지)")) removeMc = mi;
                        if (ImGui::MenuItem("노트와 함께 삭제")) {
                            removeMc = mi;
                            removeNotes = true;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                if (removeMc >= 0 && removeMc < (int)track.midiClips.size()) {
                    state.snapshot();
                    if (removeNotes) {
                        const auto rm = track.midiClips[(std::size_t)removeMc];
                        // 멤버 노트만 삭제 — 겹쳐 있는 남의 노트는 남는다
                        seq::eraseMidiClip(track, rm);
                        refreshPlaybackIfPlaying(state);
                    }
                    track.midiClips.erase(track.midiClips.begin() + removeMc);
                    state.selMidiClipTrack = state.selMidiClipIndex = -1;
                    state.selectedNotes.clear();
                }
                ImGui::SetCursorScreenPos(p0); // 레인 버튼 위치 복원
            }

            ImGui::InvisibleButton("lane", ImVec2(timelineW, laneH),
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonRight);
            if (i < (int)state.laneRects.size())
                state.laneRects[i] = {p0.y, p0.y + laneH};
            if (ImGui::IsItemClicked() && state.autoLane == 0) {
                state.selectedTrack = i; // 레인 클릭 -> 선택
                // Shift+클릭 = 이 트랙을 타브 창 표시 목록에 토글 (클릭 순서대로 쌓임)
                if (ImGui::GetIO().KeyShift) toggleTabTrack(state, i);
                // 드럼 트랙(채널 10)을 클릭했는데 드럼 에디터가 닫혀 있으면 띄운다
                if ((track.channel & 0x0F) == 9) state.showDrums = true;
                if (track.isGuitar) state.showTab = true; // 기타 트랙 = 타브 창
                state.selectedTempoMarker = -1; // 마커 밖 클릭 = 마커 선택 해제
                state.selectedMarker = -1;
                // 클립이 없는 빈 곳 클릭이면 플레이헤드를 그 위치로 (뷰는 안 움직임).
                // Ctrl(구간 선택)·Shift(타브 트랙 토글)일 때는 건드리지 않는다.
                const float mxc = ImGui::GetIO().MousePos.x;
                if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift &&
                    clipHitTest(track.clips, mxc, p0.x, zoom, state.song) < 0) {
                    const uint32_t t = mxc > p0.x ? (uint32_t)((mxc - p0.x) / zoom) : 0;
                    seekTo(state, t, /*scrollView=*/false);
                    state.selClipTrack = state.selClipIndex = -1; // 빈 곳 = 클립 선택 해제
                    state.selClips.clear();
                    state.clipRange = AppState::ClipRangeSel{};
                }
            }

            // 오토메이션 그리기 모드: 레인 왼쪽 드래그로 곡선을 깔거나 덮어쓴다.
            // 1/2 = 볼륨/팬(AutoPoint 곡선), 3~6 = CC(실제 CC 이벤트를 심는다).
            if (state.autoLane != 0) {
                static constexpr int kCcForLane[4] = {1, 11, 64, 74};
                static const char* kAutoNames[6] = {"볼륨",     "팬",       "모듈레이션",
                                                    "익스프레션", "서스테인", "필터"};
                // 빠른 드래그 보간용 직전 위치 (한 번에 한 스트로크만 있어 static으로 충분)
                static uint32_t s_lastTk = 0;
                static float s_lastRel = 0.0f;
                static bool s_stroke = false;
                if (ImGui::IsItemActivated()) {
                    state.selectedTrack = i;
                    state.snapshot(); // 스트로크 1회 = 언두 1회
                    s_stroke = false;
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const float mxa = ImGui::GetIO().MousePos.x;
                    const float mya = ImGui::GetIO().MousePos.y;
                    trackViewEdgeScroll(mxa);
                    const uint32_t tk = mxa > p0.x ? (uint32_t)((mxa - p0.x) / zoom) : 0;
                    const float rel = std::clamp(1.0f - (mya - p0.y) / laneH, 0.0f, 1.0f);
                    const uint32_t win = (uint32_t)std::max(1, state.song.ppqn / 16);
                    if (state.autoLane <= 2) {
                        auto& pts = state.autoLane == 1 ? track.volAuto : track.panAuto;
                        const float val =
                            state.autoLane == 1 ? rel * 1.5f : rel * 2.0f - 1.0f;
                        pts.erase(std::remove_if(pts.begin(), pts.end(),
                                                 [&](const seq::Track::AutoPoint& p) {
                                                     return p.tick + win >= tk &&
                                                            p.tick <= tk + win;
                                                 }),
                                  pts.end());
                        pts.push_back({tk, val});
                        std::sort(pts.begin(), pts.end(),
                                  [](const seq::Track::AutoPoint& a,
                                     const seq::Track::AutoPoint& b2) {
                                      return a.tick < b2.tick;
                                  });
                    } else {
                        // CC 이벤트 심기. 같은 CC의 근처 이벤트를 걷어내고 새 값을 놓는다.
                        const uint8_t cc = (uint8_t)kCcForLane[state.autoLane - 3];
                        const uint8_t status =
                            (uint8_t)(midi::kStatusControlChange | (track.channel & 0x0F));
                        const auto put = [&](uint32_t ptk, float prel) {
                            auto& evs = track.events;
                            evs.erase(std::remove_if(
                                          evs.begin(), evs.end(),
                                          [&](const seq::MidiEvent& e) {
                                              return e.status == status && e.data1 == cc &&
                                                     e.tick + win >= ptk && e.tick <= ptk + win;
                                          }),
                                      evs.end());
                            seq::MidiEvent e;
                            e.tick = ptk;
                            e.status = status;
                            e.data1 = cc;
                            e.data2 = (uint8_t)std::clamp(
                                (int)std::lround(prel * 127.0f), 0, 127);
                            evs.push_back(e);
                        };
                        // 마우스가 빨리 움직여 건너뛴 구간을 선형 램프로 채운다
                        if (s_stroke && (s_lastTk + win < tk || tk + win < s_lastTk)) {
                            const int64_t d = (int64_t)tk - (int64_t)s_lastTk;
                            const int steps = (int)(std::llabs(d) / (int64_t)win);
                            for (int st = 1; st < steps; ++st) {
                                const float f = (float)st / (float)steps;
                                put((uint32_t)((int64_t)s_lastTk + (int64_t)((double)d * f)),
                                    s_lastRel + (rel - s_lastRel) * f);
                            }
                        }
                        put(tk, rel);
                        track.sortEvents();
                    }
                    s_lastTk = tk;
                    s_lastRel = rel;
                    s_stroke = true;
                }
                // CC는 재생 스냅샷에 들어가므로 스트로크가 끝나면 반영한다
                if (state.autoLane >= 3 && ImGui::IsItemDeactivated())
                    refreshPlaybackIfPlaying(state);
                if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                    ImGui::SetTooltip("드래그: %s 곡선 그리기 (위 = 크게)\n"
                                      "우클릭 메뉴에서 곡선 삭제%s",
                                      kAutoNames[state.autoLane - 1],
                                      state.autoLane >= 3
                                          ? "\nCC는 VST 악기에 적용됩니다 (내장 신스 제외)"
                                          : "");
            }
            // 레인 우클릭 -> 클립 위면 "오디오 삭제"(그 클립만), 빈 곳이면 "트랙 삭제".
            // (클립 위에서 실수로 트랙째 지우는 것을 막는다)
            {
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !tempoMarkerTookRightClick) {
                    state.selectedTrack = i;
                    const float mxr = ImGui::GetIO().MousePos.x;
                    state.laneCtxClip = clipHitTest(track.clips, mxr, p0.x, zoom, state.song);
                    state.laneCtxTick =
                        mxr > p0.x ? (uint32_t)((mxr - p0.x) / zoom) : 0; // 자르기 위치
                    ImGui::OpenPopup("lanectx");
                }
                if (ImGui::BeginPopup("lanectx")) {
                    const int ci = state.laneCtxClip;
                    if (ci >= 0 && ci < (int)track.clips.size() && track.clips[(std::size_t)ci]) {
                        auto& cclip = *track.clips[(std::size_t)ci];
                        ImGui::TextDisabled("%s", cclip.name.c_str());
                        ImGui::Separator();
                        // Ctrl+드래그로 잡은 구간이 이 클립에 있으면 구간 메뉴부터
                        if (state.clipRange.track == i && state.clipRange.clip == ci &&
                            state.clipRange.t1 > state.clipRange.t0) {
                            if (ImGui::MenuItem("선택 구간 복사", "Ctrl+C"))
                                copyClipRange(state);
                            if (ImGui::MenuItem("선택 구간 삭제 (공백 유지)", "Del"))
                                deleteClipRange(state);
                            if (ImGui::MenuItem("선택 구간 잘라내고 당겨 붙이기", "Shift+Del"))
                                deleteClipRange(state, /*closeGap=*/true);
                            ImGui::Separator();
                        }
                        if (ImGui::MenuItem("여기서 자르기 (가위)"))
                            splitTrackClip(state, i, ci, state.laneCtxTick);
                        if (ImGui::MenuItem("클립 복사", "Ctrl+C")) {
                            state.selClipTrack = i;
                            state.selClipIndex = ci;
                            copySelectedClip(state);
                        }
                        if (ImGui::MenuItem("리버스 (역재생)")) {
                            if (auto rev = audio::reverseClip(cclip)) {
                                state.snapshot(); // 포인터 교체 -> Ctrl+Z 복원
                                track.clips[(std::size_t)ci] = std::move(rev);
                                refreshPlaybackIfPlaying(state);
                                state.statusMessage = "클립 리버스";
                            }
                        }
                        // 병합: 배치 그대로 하나의 클립으로 (사이 공백은 무음)
                        {
                            // Shift 다중 선택이 이 트랙에 2개 이상이면 그것부터
                            std::vector<int> selHere;
                            for (const auto& sc : state.selClips)
                                if (sc.first == i && sc.second >= 0 &&
                                    sc.second < (int)track.clips.size())
                                    selHere.push_back(sc.second);
                            if ((int)selHere.size() >= 2) {
                                char mlbl[48];
                                std::snprintf(mlbl, sizeof(mlbl), "선택한 클립 병합 (%d개)",
                                              (int)selHere.size());
                                if (ImGui::MenuItem(mlbl))
                                    mergeTrackClips(state, i, std::move(selHere));
                            }
                            int nextCi = -1;
                            uint32_t bestStart = 0xFFFFFFFFu;
                            for (int k = 0; k < (int)track.clips.size(); ++k) {
                                if (k == ci || !track.clips[(std::size_t)k]) continue;
                                const uint32_t s = track.clips[(std::size_t)k]->startTick;
                                if (s >= cclip.startTick && s < bestStart) {
                                    bestStart = s;
                                    nextCi = k;
                                }
                            }
                            if (nextCi >= 0 && ImGui::MenuItem("다음 클립과 붙이기 (병합)"))
                                mergeTrackClips(state, i, {ci, nextCi});
                            if ((int)track.clips.size() >= 2 &&
                                ImGui::MenuItem("트랙의 모든 클립 병합")) {
                                std::vector<int> all;
                                for (int k = 0; k < (int)track.clips.size(); ++k)
                                    if (track.clips[(std::size_t)k]) all.push_back(k);
                                mergeTrackClips(state, i, std::move(all));
                            }
                        }
                        if (ImGui::MenuItem("오디오 삭제", "Del")) deleteTrackClip(state, i, ci);
                        ImGui::Separator();
                        // 페이드: 슬라이더로 즉시 반영 (재생 중에도 다음 프레임에 적용)
                        float fi = (float)cclip.fadeInSec;
                        float fo = (float)cclip.fadeOutSec;
                        bool fiEdit = false, foEdit = false;
                        const bool fiChg = sliderFloatPM("페이드 인 (초)", &fi, 0.0f, 3.0f, 0.05f,
                                                         "%.2f", 160.0f, &fiEdit);
                        if (fiEdit) state.snapshot(); // 편집 시작(슬라이더 잡기/버튼) 시 1회
                        if (fiChg) cclip.fadeInSec = fi;
                        const bool foChg = sliderFloatPM("페이드 아웃 (초)", &fo, 0.0f, 3.0f, 0.05f,
                                                         "%.2f", 160.0f, &foEdit);
                        if (foEdit) state.snapshot();
                        if (foChg) cclip.fadeOutSec = fo;
                        ImGui::Separator();
                        // 클립 게인: 트랙 볼륨과 별개로 이 클립만 키우거나 줄인다.
                        // 로그 슬라이더라 0.1~4배(-20dB~+12dB)를 고르게 오간다.
                        float cg = cclip.gain;
                        ImGui::SetNextItemWidth(160);
                        const bool cgChg = ImGui::SliderFloat("게인##clipg", &cg, 0.1f, 4.0f,
                                                              "%.2fx", ImGuiSliderFlags_Logarithmic);
                        if (ImGui::IsItemActivated()) state.snapshot();
                        if (cgChg) cclip.gain = cg;
                        if (ImGui::MenuItem("정규화 (피크를 -1dB로)")) {
                            state.snapshot();
                            cclip.gain = audio::normalizeGainFor(cclip);
                            state.statusMessage = "정규화: 게인 " +
                                                  std::to_string(cclip.gain).substr(0, 4) + "x";
                        }
                        if (ImGui::MenuItem("게인 초기화 (1.0x)")) {
                            state.snapshot();
                            cclip.gain = 1.0f;
                        }
                    } else {
                        if (state.clipClipboard && ImGui::MenuItem("클립 붙여넣기 (여기)"))
                            pasteClipAt(state, i, state.laneCtxTick);
                        if (ImGui::MenuItem("트랙 삭제")) deleteTrack(state, i);
                    }
                    // 템포 변경: 우클릭한 바로 그 위치(마디 스냅 없음)에 지점 추가
                    ImGui::Separator();
                    if (ImGui::MenuItem("여기부터 템포 변경 추가")) {
                        state.snapshot();
                        seq::TempoChange tc;
                        tc.tick = state.laneCtxTick;
                        tc.bpm = seq::bpmAtTick(state.song, state.laneCtxTick);
                        state.song.tempoChanges.push_back(tc);
                        std::stable_sort(
                            state.song.tempoChanges.begin(), state.song.tempoChanges.end(),
                            [](const seq::TempoChange& a, const seq::TempoChange& b2) {
                                return a.tick < b2.tick;
                            });
                        state.statusMessage =
                            "템포 지점 추가됨 — 주황 마커를 우클릭해 BPM을 정하세요";
                    }
                    // MIDI 클립 만들기: 구간 선택이 있으면 그 범위, 없으면 이 마디
                    {
                        const bool hasRange = state.clipRange.track == i &&
                                              state.clipRange.t1 > state.clipRange.t0;
                        const char* mcLabel = hasRange ? "선택 구간을 MIDI 클립으로"
                                                       : "이 마디를 MIDI 클립으로";
                        if (ImGui::MenuItem(mcLabel)) {
                            state.snapshot();
                            seq::MidiClip nc;
                            if (hasRange) {
                                nc.startTick = state.clipRange.t0;
                                nc.endTick = state.clipRange.t1;
                                state.clipRange = AppState::ClipRangeSel{};
                            } else {
                                const uint32_t tpbC =
                                    songTicksPerBar(state);
                                nc.startTick = state.laneCtxTick / tpbC * tpbC;
                                nc.endTick = nc.startTick + tpbC;
                            }
                            nc.name =
                                "클립 " + std::to_string((int)track.midiClips.size() + 1);
                            seq::adoptMidiClipMembers(track, nc); // 범위 안 노트 = 소유
                            track.midiClips.push_back(nc);
                            std::stable_sort(
                                track.midiClips.begin(), track.midiClips.end(),
                                [](const seq::MidiClip& a, const seq::MidiClip& b2) {
                                    return a.startTick < b2.startTick;
                                });
                            state.selMidiClipTrack = i;
                            state.selMidiClipIndex = -1; // 정렬로 밀렸을 수 있어 해제
                            state.statusMessage =
                                "MIDI 클립 생성 — 제목 띠를 드래그해 옮기고, 우클릭으로 "
                                "이름/복제/삭제";
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "노트들을 블록으로 묶어 트랙 뷰에서 옮기고 복제할 수 "
                                "있습니다.\n(빈 곳 Ctrl+드래그로 구간을 잡은 뒤 만들면 그 "
                                "범위가 클립이 됩니다)");
                    }
                    if (ImGui::MenuItem("여기에 구간 마커 추가")) {
                        state.snapshot();
                        seq::SectionMarker mk;
                        mk.tick = state.laneCtxTick;
                        mk.name = "구간 " + std::to_string((int)state.song.markers.size() + 1);
                        state.song.markers.push_back(std::move(mk));
                        std::stable_sort(state.song.markers.begin(), state.song.markers.end(),
                                         [](const seq::SectionMarker& a,
                                            const seq::SectionMarker& b2) {
                                             return a.tick < b2.tick;
                                         });
                        state.statusMessage =
                            "구간 마커 추가됨 — 이름표를 우클릭해 이름을 바꾸세요";
                    }
                    // 오토메이션 곡선 삭제 (있을 때만)
                    if (!track.volAuto.empty() &&
                        ImGui::MenuItem("볼륨 오토메이션 지우기")) {
                        state.snapshot();
                        track.volAuto.clear();
                    }
                    if (!track.panAuto.empty() && ImGui::MenuItem("팬 오토메이션 지우기")) {
                        state.snapshot();
                        track.panAuto.clear();
                    }
                    // 현재 CC 레인의 곡선 지우기 (그 CC 이벤트만 걷어낸다)
                    if (state.autoLane >= 3) {
                        static constexpr int kCcForLaneM[4] = {1, 11, 64, 74};
                        const uint8_t ccM = (uint8_t)kCcForLaneM[state.autoLane - 3];
                        const uint8_t statusM =
                            (uint8_t)(midi::kStatusControlChange | (track.channel & 0x0F));
                        char lbl[64];
                        std::snprintf(lbl, sizeof(lbl), "CC%d 곡선 지우기", (int)ccM);
                        if (ImGui::MenuItem(lbl)) {
                            state.snapshot();
                            auto& evs = track.events;
                            evs.erase(std::remove_if(evs.begin(), evs.end(),
                                                     [&](const seq::MidiEvent& e) {
                                                         return e.status == statusM &&
                                                                e.data1 == ccM;
                                                     }),
                                      evs.end());
                            refreshPlaybackIfPlaying(state);
                        }
                    }
                    ImGui::EndPopup();
                }
            }

            // 오디오 클립 이동/좌우 트림/속도 드래그 + 구간 선택.
            // (구간 선택은 클립이 없는 빈 레인에서도 Ctrl+드래그로 시작할 수 있다)
            {
                const double bpm = state.song.bpm;
                const int ppq = state.song.ppqn;
                const float mx = ImGui::GetIO().MousePos.x;
                auto& cd = state.clipDrag;

                // 구간 선택(Ctrl+드래그) 진행/종료
                if (state.clipRange.active && state.clipRange.track == i) {
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        trackViewEdgeScroll(mx);
                        state.clipRange.t1 =
                            (uint32_t)std::max(0.0, (double)(mx - p0.x) / zoom);
                    } else {
                        state.clipRange.active = false;
                        auto& r = state.clipRange;
                        if (r.t1 < r.t0) std::swap(r.t0, r.t1);
                        // 클립 범위로 자르고, 클릭 수준으로 좁으면 무효
                        if (r.clip >= 0 && r.clip < (int)track.clips.size() &&
                            track.clips[(std::size_t)r.clip]) {
                            const auto& rc = *track.clips[(std::size_t)r.clip];
                            const uint32_t cs = rc.startTick;
                            const uint32_t ce = (uint32_t)clipEndTick(rc, state.song);
                            r.t0 = std::clamp(r.t0, cs, ce);
                            r.t1 = std::clamp(r.t1, cs, ce);
                        }
                        if (r.t1 - r.t0 < (uint32_t)state.song.ppqn / 32)
                            r = AppState::ClipRangeSel{};
                    }
                }

                if (cd.active && cd.track == i && cd.clip >= 0 &&
                    cd.clip < (int)track.clips.size() && track.clips[(std::size_t)cd.clip]) {
                    auto& clip = *track.clips[(std::size_t)cd.clip];
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        trackViewEdgeScroll(mx); // 끝쪽으로 끌면 뷰가 옆으로 흐른다
                        const double mouseTick = std::max(0.0, (double)(mx - p0.x) / zoom);
                        if (cd.mode == AppState::ClipDrag::Move) {
                            const long ns = (long)mouseTick - cd.grabTickOffset;
                            clip.startTick = applyTrackSnap(state, tpb, ns); // 격자 스냅

                            // 다른 레인 위로 끌면 대상 트랙을 테두리로 알려준다
                            const float myv = ImGui::GetIO().MousePos.y;
                            for (int li = 0; li < (int)state.laneRects.size() &&
                                             li < (int)state.song.tracks.size();
                                 ++li) {
                                if (li == cd.track) continue;
                                const auto& lr = state.laneRects[(std::size_t)li];
                                if (myv >= lr.y0 && myv < lr.y1)
                                    ImGui::GetForegroundDrawList()->AddRect(
                                        ImVec2(p0.x, lr.y0), ImVec2(p0.x + timelineW, lr.y1),
                                        IM_COL32(120, 200, 255, 220), 0.0f, 0, 2.0f);
                            }
                        } else if (cd.mode == AppState::ClipDrag::Speed) {
                            // 타임스트레치: 오른쪽 끝을 끌어 재생 길이를 바꾼다.
                            // 오른쪽(길게) = 느리게, 왼쪽(짧게) = 빠르게. 음정도 함께 변한다.
                            // 길이 = playLen / (sampleRate * speed)  ->  speed = playLen / (sr * 길이)
                            const double endTk =
                                std::max((double)clip.startTick + 1.0, mouseTick);
                            const double newSec =
                                seq::songTickToSec(state.song, endTk) -
                                seq::songTickToSec(state.song, clip.startTick);
                            clip.speed =
                                std::clamp(clip.speedForDuration(newSec), 0.25, 4.0); // 4x 범위
                        } else if (cd.mode == AppState::ClipDrag::Stretch) {
                            // 음정 유지 스트레치: 드래그 중엔 목표 길이만 표시하고
                            // (WSOLA는 무거워서) 놓는 순간 한 번 처리한다.
                            cd.stretchTargetTicks =
                                std::max(1.0, mouseTick - (double)clip.startTick);
                            const float gx0 = p0.x + (float)clip.startTick * zoom;
                            const float gx1 =
                                p0.x +
                                (float)((double)clip.startTick + cd.stretchTargetTicks) * zoom;
                            ImGui::GetForegroundDrawList()->AddRect(
                                ImVec2(gx0, p0.y + 2.0f), ImVec2(gx1, p0.y + laneH - 2.0f),
                                IM_COL32(120, 255, 170, 230), 4.0f, 0, 2.0f);
                            // 배율을 실시간으로 보여준다 (몇 배로 늘/줄어드는지)
                            const double curTicks =
                                clipEndTick(clip, state.song) - (double)clip.startTick;
                            if (curTicks > 0.0)
                                ImGui::SetTooltip("음정 유지 스트레치: %.2fx (놓으면 처리)",
                                                  cd.stretchTargetTicks / curTicks);
                        } else if (cd.mode == AppState::ClipDrag::TrimR) {
                            const double endTk =
                                std::max((double)clip.startTick + 1.0, mouseTick);
                            const double newSec =
                                seq::songTickToSec(state.song, endTk) -
                                seq::songTickToSec(state.song, clip.startTick);
                            clip.trimLen = std::max<int64_t>(
                                1, (int64_t)(newSec * clip.sampleRate * clip.speed));
                        } else { // TrimL: 왼쪽 끝을 끌어 시작을 트림(오른쪽 끝 고정)
                            const int64_t rightEnd = cd.origTrimStart + cd.origTrimLen;
                            const double dSec =
                                seq::songTickToSec(state.song, std::max(0.0, mouseTick)) -
                                seq::songTickToSec(state.song, cd.origStartTick);
                            int64_t nts = cd.origTrimStart + (int64_t)(dSec * clip.sampleRate * clip.speed);
                            if (nts < 0) nts = 0;
                            if (nts > rightEnd - 1) nts = rightEnd - 1;
                            const double movedSec =
                                (double)(nts - cd.origTrimStart) / ((double)clip.sampleRate * clip.speed);
                            clip.trimStart = nts;
                            clip.trimLen = rightEnd - nts;
                            const double st = seq::songSecToTick(
                                state.song,
                                seq::songTickToSec(state.song, cd.origStartTick) + movedSec);
                            clip.startTick = (uint32_t)(st < 0 ? 0 : st);
                        }
                    } else {
                        cd.active = false; // 놓음 (undo 스냅샷은 시작 때 남김)
                        const float dxc = ImGui::GetIO().MousePos.x - cd.downX;
                        const float dyc = ImGui::GetIO().MousePos.y - cd.downY;
                        if (cd.mode == AppState::ClipDrag::Move && dxc * dxc + dyc * dyc < 16.0f) {
                            // 움직이지 않고 놓았으면(클릭) 클립 위에서도 시크한다
                            const uint32_t t =
                                mx > p0.x ? (uint32_t)((mx - p0.x) / zoom) : 0;
                            seekTo(state, t, /*scrollView=*/false);
                        } else if (cd.mode == AppState::ClipDrag::Stretch) {
                            // 놓는 순간 WSOLA 처리: 새 클립으로 교체 (언두 = 포인터 복원)
                            const double curSec = clip.durationSeconds();
                            const double newSec =
                                seq::songTickToSec(state.song,
                                                   (double)clip.startTick +
                                                       cd.stretchTargetTicks) -
                                seq::songTickToSec(state.song, clip.startTick);
                            const double ratio =
                                curSec > 1e-6 ? std::clamp(newSec / curSec, 0.25, 4.0) : 1.0;
                            if (std::fabs(ratio - 1.0) > 0.02) {
                                auto stretched = audio::stretchClipPitchPreserve(clip, ratio);
                                if (stretched) {
                                    track.clips[(std::size_t)cd.clip] = stretched;
                                    refreshPlaybackIfPlaying(state);
                                    state.statusMessage = "음정 유지 스트레치: " +
                                                          std::to_string(ratio).substr(0, 4) +
                                                          "배 길이";
                                } else {
                                    state.statusMessage =
                                        "스트레치할 수 없습니다 (클립이 너무 짧음)";
                                }
                            }
                        } else if (cd.mode == AppState::ClipDrag::Move) {
                            // 다른 트랙 레인 위에서 놓았으면 그 트랙으로 이동
                            const float my = ImGui::GetIO().MousePos.y;
                            int target = -1;
                            for (int li = 0; li < (int)state.laneRects.size() &&
                                             li < (int)state.song.tracks.size();
                                 ++li)
                                if (my >= state.laneRects[(std::size_t)li].y0 &&
                                    my < state.laneRects[(std::size_t)li].y1)
                                    target = li;
                            if (target >= 0 && target != cd.track && cd.clip >= 0 &&
                                cd.clip < (int)track.clips.size()) {
                                auto moving = track.clips[(std::size_t)cd.clip];
                                track.clips.erase(track.clips.begin() + cd.clip);
                                auto& dst = state.song.tracks[(std::size_t)target];
                                dst.clips.push_back(std::move(moving));
                                state.selectedTrack = target;
                                state.selClipTrack = target;
                                state.selClipIndex = (int)dst.clips.size() - 1;
                                state.selClips.clear();
                                state.selClips.insert({target, state.selClipIndex});
                                refreshPlaybackIfPlaying(state);
                                state.statusMessage = "클립을 '" + dst.name + "' 트랙으로 이동";
                            }
                        }
                    }
                } else if (!cd.active && state.autoLane == 0 && ImGui::IsItemHovered()) {
                    // 마우스가 올라간 클립을 찾는다 (겹치면 위에 보이는 클립 우선)
                    const int hit = clipHitTest(track.clips, mx, p0.x, zoom, state.song);
                    if (hit >= 0) {
                        auto& clip = *track.clips[(std::size_t)hit];
                        float cx0, cx1;
                        clipScreenX(clip, p0.x, zoom, state.song, cx0, cx1);
                        const bool shift = ImGui::GetIO().KeyShift;
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        const bool nearL = std::fabs(mx - cx0) <= 6.0f;
                        const bool nearR = std::fabs(mx - cx1) <= 6.0f;
                        const bool onBody = mx > cx0 && mx < cx1;
                        if (nearL || nearR) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (nearR && shift) {
                            ImGui::SetTooltip(
                                "Shift+드래그: 속도 조절 (음정 변함, 현재 %.2fx)\n"
                                "Ctrl+드래그: 음정 유지 길이 조절",
                                clip.speed);
                        } else if (nearR && ctrl) {
                            ImGui::SetTooltip("Ctrl+드래그: 음정 유지 길이 조절\n"
                                              "(놓는 순간 처리됩니다)");
                        }
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ctrl && onBody &&
                            !nearL && !nearR) {
                            // Ctrl+드래그: 클립 위 "구간 선택" 시작
                            state.clipRange.active = true;
                            state.clipRange.track = i;
                            state.clipRange.clip = hit;
                            state.clipRange.t0 = state.clipRange.t1 =
                                (uint32_t)std::max(0.0, (double)(mx - p0.x) / zoom);
                        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && shift &&
                                   onBody && !nearL && !nearR) {
                            // Shift+클릭 (몸통): 다중 선택 토글. 드래그는 시작하지 않는다.
                            const auto key = std::make_pair(i, hit);
                            if (state.selClips.count(key)) state.selClips.erase(key);
                            else state.selClips.insert(key);
                            state.selClipTrack = i;
                            state.selClipIndex = hit;
                        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                   (nearL || nearR || onBody)) {
                            state.snapshot();
                            cd.active = true;
                            cd.track = i;
                            cd.clip = hit;
                            state.selClipTrack = i; // 클릭 = 클립 선택 (Ctrl+C 대상)
                            state.selClipIndex = hit;
                            state.selClips.clear(); // 일반 클릭 = 단일 선택으로
                            state.selClips.insert({i, hit});
                            state.clipRange = AppState::ClipRangeSel{}; // 구간 선택 해제
                            cd.downX = mx;
                            cd.downY = ImGui::GetIO().MousePos.y;
                            cd.origStartTick = clip.startTick;
                            cd.origTrimStart = clip.trimStart;
                            cd.origTrimLen = clip.playLen();
                            cd.origSpeed = clip.speed;
                            const double mouseTick = std::max(0.0, (double)(mx - p0.x) / zoom);
                            // 오른쪽 끝: Shift=배속(음정 변함), Ctrl=음정 유지, 기본=트림
                            if (nearR && shift) cd.mode = AppState::ClipDrag::Speed;
                            else if (nearR && ctrl) {
                                cd.mode = AppState::ClipDrag::Stretch;
                                cd.stretchTargetTicks =
                                    clipEndTick(clip, state.song) - (double)clip.startTick;
                            } else if (nearL) cd.mode = AppState::ClipDrag::TrimL;
                            else if (nearR) cd.mode = AppState::ClipDrag::TrimR;
                            else {
                                cd.mode = AppState::ClipDrag::Move;
                                cd.grabTickOffset = (int)(mouseTick - (double)clip.startTick);
                            }
                        }
                    } else if (ImGui::GetIO().KeyCtrl &&
                               ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        // 빈 곳 Ctrl+드래그: 클립 없이도 시간축 "구간 선택"을 만든다
                        // (루프 켜기/L 키로 그 구간을 바로 루프로 지정할 수 있다)
                        state.clipRange.active = true;
                        state.clipRange.track = i;
                        state.clipRange.clip = -1;
                        state.clipRange.t0 = state.clipRange.t1 =
                            (uint32_t)std::max(0.0, (double)(mx - p0.x) / zoom);
                    }
                }
            }

            // 프리즈된 트랙은 푸른빛으로 구분한다
            dl->AddRectFilled(p0, ImVec2(p0.x + timelineW, p0.y + laneH),
                              track.frozen ? IM_COL32(34, 48, 62, 255)
                                           : (sel ? IM_COL32(46, 52, 64, 255)
                                                  : IM_COL32(36, 36, 42, 255)));
            // 격자: 마디선(밝게) + 스냅 간격의 보조선(어둡게). 보조선이 너무
            // 촘촘하면(간격 5px 미만) 지저분하므로 그때는 마디선만 그린다.
            const uint32_t sub = trackSnapTicks(state, tpb);
            if (sub > 0 && sub < tpb && (float)sub * zoom >= 5.0f) {
                for (uint32_t t = 0; t <= songLen; t += sub) {
                    if (t % tpb == 0) continue; // 마디선은 아래서 더 밝게 다시 그린다
                    const float x = p0.x + t * zoom;
                    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + laneH),
                                IM_COL32(58, 58, 68, 90));
                }
            }
            for (uint32_t t = 0; t <= songLen; t += tpb) {
                const float x = p0.x + t * zoom;
                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + laneH), IM_COL32(70, 70, 82, 120));
            }
            // (템포/구간 마커는 클립에 가리지 않도록 내용 "위"에 그린다 — 아래 참고)

            // 빈 곳 Ctrl+드래그 구간 선택(클립에 안 걸린 경우, clip=-1) 하이라이트
            if (state.clipRange.track == i && state.clipRange.clip < 0 &&
                (state.clipRange.active || state.clipRange.t1 > state.clipRange.t0)) {
                uint32_t ra = state.clipRange.t0, rb = state.clipRange.t1;
                if (rb < ra) std::swap(ra, rb);
                const float rx0 = p0.x + (float)ra * zoom;
                const float rx1 = p0.x + (float)rb * zoom;
                dl->AddRectFilled(ImVec2(rx0, p0.y + 2.0f), ImVec2(rx1, p0.y + laneH - 2.0f),
                                  IM_COL32(150, 200, 255, 60));
                dl->AddRect(ImVec2(rx0, p0.y + 2.0f), ImVec2(rx1, p0.y + laneH - 2.0f),
                            IM_COL32(150, 200, 255, 210), 0.0f, 0, 1.5f);
            }
            // 내용: 오디오 클립들(어두운 블록 + 파형) + MIDI 노트 (둘 다, 노트가 위)
            if (!track.clips.empty()) {
                for (int ci2 = 0; ci2 < (int)track.clips.size(); ++ci2) {
                    const auto& cp = track.clips[(std::size_t)ci2];
                    if (!cp) continue;
                    drawClipBlock(dl, *cp, p0.x, p0.y, laneH, zoom, state.song, sel);
                    drawWaveform(dl, *cp, p0.x, p0.y, laneH, zoom, state.song,
                                 IM_COL32(230, 160, 110, 235));
                    // 구간 선택(Ctrl+드래그) 하이라이트
                    if (state.clipRange.track == i && state.clipRange.clip == ci2 &&
                        (state.clipRange.active || state.clipRange.t1 > state.clipRange.t0)) {
                        uint32_t ra = state.clipRange.t0, rb = state.clipRange.t1;
                        if (rb < ra) std::swap(ra, rb);
                        const float rx0 = p0.x + (float)ra * zoom;
                        const float rx1 = p0.x + (float)rb * zoom;
                        dl->AddRectFilled(ImVec2(rx0, p0.y + 2.0f),
                                          ImVec2(rx1, p0.y + laneH - 2.0f),
                                          IM_COL32(150, 200, 255, 60));
                        dl->AddRect(ImVec2(rx0, p0.y + 2.0f),
                                    ImVec2(rx1, p0.y + laneH - 2.0f),
                                    IM_COL32(150, 200, 255, 210), 0.0f, 0, 1.5f);
                    }
                    // 선택된 클립(단일/Shift 다중)은 흰 테두리로 표시
                    if (state.selClips.count({i, ci2}) ||
                        (i == state.selClipTrack && ci2 == state.selClipIndex)) {
                        float scx0, scx1;
                        clipScreenX(*cp, p0.x, zoom, state.song, scx0, scx1);
                        dl->AddRect(ImVec2(scx0 - 1.5f, p0.y + 1.0f),
                                    ImVec2(scx1 + 1.5f, p0.y + laneH - 1.0f),
                                    IM_COL32(255, 255, 255, 225), 4.0f, 0, 2.0f);
                        // 페이드 핸들 점 (위 모서리, 좌 = 인 / 우 = 아웃)
                        if (i == state.selClipTrack && ci2 == state.selClipIndex &&
                            cp->sampleRate > 0) {
                            const double s2 = seq::songTickToSec(state.song, cp->startTick);
                            const double e2 = s2 + cp->durationSeconds();
                            const float hx0 =
                                p0.x +
                                (float)seq::songSecToTick(state.song, s2 + cp->fadeInSec) *
                                    zoom;
                            const float hx1 =
                                p0.x +
                                (float)seq::songSecToTick(state.song, e2 - cp->fadeOutSec) *
                                    zoom;
                            dl->AddCircleFilled(ImVec2(hx0, p0.y + 7.0f), 3.5f,
                                                IM_COL32(255, 255, 255, 235));
                            dl->AddCircleFilled(ImVec2(hx1, p0.y + 7.0f), 3.5f,
                                                IM_COL32(255, 255, 255, 235));
                        }
                    }
                }
            }
            // MIDI 클립 블록: 반투명 배경 + 상단 제목 띠 (노트 미리보기 아래 깔림)
            for (int mi = 0; mi < (int)track.midiClips.size(); ++mi) {
                const auto& mc = track.midiClips[(std::size_t)mi];
                const float mx0 = p0.x + (float)mc.startTick * zoom;
                const float mx1 = p0.x + (float)mc.endTick * zoom;
                if (mx1 < dl->GetClipRectMin().x - 40.0f ||
                    mx0 > dl->GetClipRectMax().x + 40.0f)
                    continue;
                const bool mcSel =
                    (state.selMidiClipTrack == i && state.selMidiClipIndex == mi);
                dl->AddRectFilled(ImVec2(mx0, p0.y + 1.0f), ImVec2(mx1, p0.y + laneH - 1.0f),
                                  IM_COL32(80, 120, 190, mcSel ? 66 : 40), 4.0f);
                dl->AddRect(ImVec2(mx0, p0.y + 1.0f), ImVec2(mx1, p0.y + laneH - 1.0f),
                            mcSel ? IM_COL32(255, 255, 255, 220)
                                  : IM_COL32(120, 165, 235, 170),
                            4.0f, 0, mcSel ? 2.0f : 1.2f);
                // 제목 띠 (드래그 손잡이)
                dl->AddRectFilled(ImVec2(mx0, p0.y + 1.0f), ImVec2(mx1, p0.y + 15.0f),
                                  IM_COL32(75, 120, 195, 210), 4.0f,
                                  ImDrawFlags_RoundCornersTop);
                dl->PushClipRect(ImVec2(mx0, p0.y), ImVec2(mx1, p0.y + 15.0f), true);
                dl->AddText(ImVec2(mx0 + 5.0f, p0.y + 1.0f), IM_COL32(235, 242, 255, 255),
                            mc.name.c_str());
                dl->PopClipRect();
            }
            {
                // 피아노 롤 미니 미리보기: 트랙의 실제 음역에 맞춰 세로를 채워
                // 멜로디 윤곽이 보이게 한다 (음역이 좁으면 최소 2옥타브 확보).
                // 캐시 사용: 트랙이 안 바뀌면 매 프레임 추출을 반복하지 않는다.
                const auto& notes = cachedNotes(track, i);
                if (!notes.empty()) {
                    int nlo = 127, nhi = 0;
                    for (const auto& n : notes) {
                        nlo = std::min(nlo, (int)n.note);
                        nhi = std::max(nhi, (int)n.note);
                    }
                    if (nhi - nlo < 24) {
                        const int pad = (24 - (nhi - nlo)) / 2;
                        nlo = std::max(0, nlo - pad);
                        nhi = std::min(127, nlo + 24);
                    }
                    const float noteH =
                        std::clamp((laneH - 6.0f) / (float)(nhi - nlo + 1), 2.0f, 6.0f);
                    for (const auto& n : notes) {
                        const float x0 = p0.x + n.startTick * zoom;
                        const float x1 = p0.x + n.endTick * zoom;
                        const float ny = p0.y + 3.0f +
                                         (laneH - 6.0f) *
                                             (1.0f - (float)(n.note - nlo) / (float)(nhi - nlo));
                        dl->AddRectFilled(ImVec2(x0, ny - noteH * 0.5f),
                                          ImVec2(std::max(x1 - 1.0f, x0 + 2.0f), ny + noteH * 0.5f),
                                          IM_COL32(120, 190, 255, 235), 1.0f);
                    }
                }
            }
            // 템포 변경 지점 — 클립/노트 위에 그려 가려지지 않게 한다
            // (라벨은 첫 트랙에만 — 줄마다 반복하면 시끄럽다)
            drawTempoMarkers(dl, state.song, p0.x, zoom, p0.y, p0.y + laneH, i == 0,
                             state.selectedTempoMarker);

            // 구간 마커: 청록 세로선 + 이름표 (템포 라벨과 겹치지 않게 둘째 줄).
            // 선택된 마커는 흰 테두리 (Del 키로 삭제 가능 표시)
            for (int mk2 = 0; mk2 < (int)state.song.markers.size(); ++mk2) {
                const auto& mk = state.song.markers[(std::size_t)mk2];
                const float mx2 = p0.x + (float)mk.tick * zoom;
                if (mx2 < dl->GetClipRectMin().x - 120.0f ||
                    mx2 > dl->GetClipRectMax().x + 120.0f)
                    continue;
                const bool mkSel = (mk2 == state.selectedMarker);
                dl->AddLine(ImVec2(mx2, p0.y), ImVec2(mx2, p0.y + laneH),
                            IM_COL32(80, 200, 230, mkSel ? 255 : 180), mkSel ? 2.5f : 1.5f);
                if (i == 0) {
                    const ImVec2 ts = ImGui::CalcTextSize(mk.name.c_str());
                    const ImVec2 r0(mx2 + 2.0f, p0.y + 18.0f);
                    const ImVec2 r1(mx2 + ts.x + 10.0f, p0.y + 18.0f + ts.y + 4.0f);
                    dl->AddRectFilled(r0, r1, IM_COL32(14, 66, 80, 235), 3.0f);
                    if (mkSel) dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 235), 3.0f, 0, 1.8f);
                    dl->AddText(ImVec2(mx2 + 6.0f, p0.y + 20.0f),
                                IM_COL32(170, 235, 250, 255), mk.name.c_str());
                }
            }

            // 볼륨/팬 오토메이션 곡선 (있으면 항상 표시, 편집 중이면 굵게+점)
            {
                const auto drawAuto = [&](const std::vector<seq::Track::AutoPoint>& pts,
                                          bool isVol, bool editing) {
                    if (pts.empty() && !editing) return;
                    const ImU32 col = isVol ? IM_COL32(120, 235, 150, editing ? 255 : 150)
                                            : IM_COL32(110, 200, 250, editing ? 255 : 150);
                    const auto valY = [&](float v) {
                        const float rel = isVol ? v / 1.5f : (v + 1.0f) * 0.5f;
                        return p0.y + laneH * (1.0f - std::clamp(rel, 0.0f, 1.0f));
                    };
                    const float fb = isVol ? track.volume : track.pan;
                    float prevX = p0.x;
                    float prevY = valY(pts.empty() ? fb : pts.front().value);
                    for (const auto& ap : pts) {
                        const float x = p0.x + (float)ap.tick * zoom;
                        const float y = valY(ap.value);
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), col,
                                    editing ? 2.0f : 1.2f);
                        if (editing) dl->AddCircleFilled(ImVec2(x, y), 2.4f, col);
                        prevX = x;
                        prevY = y;
                    }
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(p0.x + timelineW, prevY), col,
                                editing ? 2.0f : 1.2f);
                };
                drawAuto(track.volAuto, true, state.autoLane == 1);
                drawAuto(track.panAuto, false, state.autoLane == 2);
            }
            // CC 곡선 (해당 CC 레인 편집 중일 때만): 이벤트를 계단 곡선으로 표시
            if (state.autoLane >= 3) {
                static constexpr int kCcForLaneD[4] = {1, 11, 64, 74};
                const uint8_t ccD = (uint8_t)kCcForLaneD[state.autoLane - 3];
                const uint8_t statusD =
                    (uint8_t)(midi::kStatusControlChange | (track.channel & 0x0F));
                const ImU32 ccCol = IM_COL32(205, 140, 255, 255);
                float prevX = p0.x, prevY = -1.0f;
                for (const auto& e : track.events) {
                    if (e.status != statusD || e.data1 != ccD) continue;
                    const float x = p0.x + (float)e.tick * zoom;
                    const float y = p0.y + laneH * (1.0f - (float)e.data2 / 127.0f);
                    if (prevY >= 0.0f) { // CC는 다음 값까지 유지되므로 계단으로
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, prevY), ccCol, 1.6f);
                        dl->AddLine(ImVec2(x, prevY), ImVec2(x, y), ccCol, 1.6f);
                    }
                    dl->AddCircleFilled(ImVec2(x, y), 2.2f, ccCol);
                    prevX = x;
                    prevY = y;
                }
                if (prevY >= 0.0f)
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(p0.x + timelineW, prevY), ccCol,
                                1.6f);
            }

            // 루프 구간: 파란 반투명 띠 — 클립/노트 "위"에 그려 항상 보이게 한다
            if (state.loopEnabled) {
                const float lx0 = p0.x + (float)state.loopStartTick * zoom;
                const float lx1 = p0.x + (float)state.loopEndTick * zoom;
                dl->AddRectFilled(ImVec2(lx0, p0.y), ImVec2(lx1, p0.y + laneH),
                                  IM_COL32(90, 150, 240, 38));
                dl->AddLine(ImVec2(lx0, p0.y), ImVec2(lx0, p0.y + laneH),
                            IM_COL32(120, 180, 255, 150), 1.0f);
                dl->AddLine(ImVec2(lx1, p0.y), ImVec2(lx1, p0.y + laneH),
                            IM_COL32(120, 180, 255, 150), 1.0f);
                if (i == 0) { // 위쪽 두꺼운 띠(이동 손잡이) + 양끝 핸들
                    dl->AddRectFilled(ImVec2(lx0, p0.y), ImVec2(lx1, p0.y + 4.0f),
                                      IM_COL32(90, 160, 255, 220));
                    dl->AddRectFilled(ImVec2(lx0 - 2.0f, p0.y),
                                      ImVec2(lx0 + 2.0f, p0.y + 16.0f),
                                      IM_COL32(150, 205, 255, 245));
                    dl->AddRectFilled(ImVec2(lx1 - 2.0f, p0.y),
                                      ImVec2(lx1 + 2.0f, p0.y + 16.0f),
                                      IM_COL32(150, 205, 255, 245));
                }
            }

            // 재생 헤드
            const float hx = p0.x + state.playPosTick * zoom;
            dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + laneH), IM_COL32(255, 90, 90, 220), 1.5f);

            ImGui::PopID();
        }

        // 재생 따라가기 + 시크 시 타임라인이 플레이헤드로 스크롤되게 한다.
        const bool playing = state.player && state.player->isPlaying();
        if (state.scrollToPlayhead || (playing && state.followPlayhead)) {
            if (ImGuiTable* tbl = ImGui::GetCurrentTable())
                if (ImGuiWindow* inner = tbl->InnerWindow) {
                    const float target = (float)state.playPosTick * zoom - inner->Size.x * 0.4f;
                    ImGui::SetScrollX(inner, std::max(0.0f, target));
                }
        }
        // 방향키 스크롤 (←→ 가로, ↑↓ 세로)
        if (state.keyScrollX != 0.0f || state.keyScrollY != 0.0f)
            if (ImGuiTable* tbl = ImGui::GetCurrentTable())
                if (ImGuiWindow* inner = tbl->InnerWindow) {
                    if (state.keyScrollX != 0.0f)
                        ImGui::SetScrollX(inner,
                                          std::max(0.0f, inner->Scroll.x + state.keyScrollX));
                    if (state.keyScrollY != 0.0f)
                        ImGui::SetScrollY(inner,
                                          std::max(0.0f, inner->Scroll.y + state.keyScrollY));
                }
        // 미니맵/휠 연동: 요청이 있으면 적용하고, 현재 스크롤/보이는 폭을 기록한다
        if (ImGuiTable* tbl = ImGui::GetCurrentTable())
            if (ImGuiWindow* inner = tbl->InnerWindow) {
                if (state.tvScrollReq >= 0.0f) {
                    ImGui::SetScrollX(inner, state.tvScrollReq);
                    state.tvScrollX = state.tvScrollReq;
                    state.tvScrollReq = -1.0f;
                } else {
                    state.tvScrollX = inner->Scroll.x;
                }
                if (state.tvScrollYDelta != 0.0f) { // Shift+휠 (트랙 뷰 전용)
                    ImGui::SetScrollY(inner,
                                      std::max(0.0f, inner->Scroll.y + state.tvScrollYDelta));
                    state.tvScrollYDelta = 0.0f;
                }
                state.tvVisibleW = std::max(0.0f, inner->Size.x - kTrackHdrW()); // 헤더 열 제외
            }
        ImGui::EndTable();
    }

    // 트랙 순서 드래그 확정: 마우스를 놓은 프레임에, 표 순회가 끝난 뒤 안전하게
    if (state.trackReorderFrom >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (state.trackReorderTo != state.trackReorderFrom)
            moveTrackTo(state, state.trackReorderFrom, state.trackReorderTo);
        state.trackReorderFrom = state.trackReorderTo = -1;
    }

    if (state.song.tracks.empty())
        ImGui::TextDisabled("트랙이 없습니다. '+ 트랙'을 누르거나 빈 곳을 우클릭하세요.");

    // 빈 공간 우클릭 -> 트랙 생성 메뉴.
    // 표(테이블)가 창을 거의 다 덮어서 예전 NoOpenOverItems 방식으로는 표 안
    // 빈 공간(마지막 트랙 아래)에서 열리지 않았다 — 트랙 레인 위가 아니면
    // 어디서든 직접 연다. (레인 위 우클릭은 레인 자체 메뉴가 우선)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        bool overLane = false;
        for (const auto& lr : state.laneRects)
            if (mp.y >= lr.y0 && mp.y < lr.y1) { overLane = true; break; }
        if (!overLane) ImGui::OpenPopup("tvctx");
    }
    if (ImGui::BeginPopup("tvctx")) {
        ImGui::TextDisabled("새 트랙 만들기");
        ImGui::Separator();
        if (ImGui::MenuItem("일반 트랙")) addTrack(state);
        if (ImGui::MenuItem("드럼 트랙")) addDrumTrack(state);
        if (ImGui::MenuItem("기타 연습 트랙")) addGuitarTrack(state);
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace midipro::gui
