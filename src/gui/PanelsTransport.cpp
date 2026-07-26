// =============================================================
// MidiPro - gui/PanelsTransport.cpp
// 트랜스포트(재생/템포/루프/메트로놈/카운트인/분기) + 버전 트리 + 클릭 소리
// 편집기. Panels.cpp에서 분리 (동작 동일).
// =============================================================

#include "gui/Panels.h"
#include "gui/Icons.h"
#include "gui/PanelsInternal.h"

#include "sequencer/TimeBase.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace midipro::gui {

// 클릭 소리 편집기 (kind: 0=메트로놈 일반, 1=카운트인 일반, 2=메트로놈 강조,
// 3=카운트인 강조). 신스 음 높이 또는 WAV/MP3 샘플. 좁은 칸에 맞춘 컴팩트 배치.
static void drawClickSoundEditor(AppState& state, int kind) {
    static const char* kIds[4] = {"mt", "ci", "ac", "cia"};
    int* const kNotes[4] = {&state.metroClickNote, &state.countInClickNote,
                            &state.accentClickNote, &state.countInAccentClickNote};
    std::string* const kPaths[4] = {&state.metroSamplePath, &state.countInSamplePath,
                                    &state.accentSamplePath, &state.countInAccentSamplePath};
    bool* const kReqs[4] = {&state.metroSampleLoadRequested, &state.countInSampleLoadRequested,
                            &state.accentSampleLoadRequested,
                            &state.countInAccentSampleLoadRequested};
    kind = std::clamp(kind, 0, 3);
    ImGui::PushID(kIds[kind]);
    int& note = *kNotes[kind];
    std::string& path = *kPaths[kind];
    const bool usingSample = !path.empty();

    if (usingSample) {
        const std::size_t slash = path.find_last_of("\\/");
        const std::string fname = slash == std::string::npos ? path : path.substr(slash + 1);
        ImGui::Text("샘플: %s", fname.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        if (ImGui::SmallButton("미리듣기"))
            triggerNote(state, 9, (uint8_t)std::clamp(note, 0, 127), 112, 0.2);
        ImGui::SameLine();
        if (ImGui::SmallButton("기본음으로")) {
            path.clear();
            if (state.audioClips) state.audioClips->setClickSample(kind, nullptr);
        }
    } else {
        // 신스 음 높이 + 음이름 (±1 반음 버튼으로 정확히 고른다)
        sliderIntPM("##note", &note, 36, 108, 1, "음 %d", 120.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", noteName((uint8_t)std::clamp(note, 0, 127)).c_str());
        if (ImGui::SmallButton("미리듣기"))
            triggerNote(state, 9, (uint8_t)std::clamp(note, 0, 127), 112, 0.2);
        ImGui::SameLine();
        if (ImGui::SmallButton("파일 사용...")) *kReqs[kind] = true;
    }
    ImGui::PopID();
}

// ---------------------------------------------------------
// 버전 분기 (트랜스포트의 "분기" 칸)
// ---------------------------------------------------------

// 현재 곡을 새 버전 노드로 저장한다. 부모 = 현재 노드 -> 옛 노드를 불러온
// 상태에서 체크인하면 자동으로 가지가 갈라진다.
// "생성" 버튼을 눌렀을 때만 호출된다 — 팝업을 그냥 닫으면 노드가 안 생긴다.
static void checkinVersion(AppState& state, const char* name, const char* note) {
    AppState::VersionNode n;
    n.id = state.versionNextId++;
    n.parent = state.versionCurrent;
    n.name = (name && name[0]) ? name : "V" + std::to_string(n.id);
    n.note = note ? note : "";
    n.snap = AppState::makeSongSnapshot(state.song);
    state.versionCurrent = n.id;
    state.statusMessage = "체크인: " + n.name;
    state.versions.push_back(std::move(n));
}

// 노드의 곡을 불러온다 (Ctrl+Z로 직전 상태 복귀 가능). 이후 체크인은
// 이 노드의 자식으로 붙는다 = 여기서 가지가 뻗는다.
static void checkoutVersion(AppState& state, int id) {
    for (const auto& n : state.versions) {
        if (n.id != id) continue;
        stopTransport(state);
        state.snapshot(); // 불러오기 직전 작업을 되돌릴 수 있게
        AppState::applySongSnapshot(state.song, n.snap);
        clampSelection(state);
        rebuildAudioMixAt(state, state.playPosTick);
        // 클립/마커 개수가 달라졌을 수 있으니 선택을 비운다
        state.selClips.clear();
        state.selClipTrack = state.selClipIndex = -1;
        state.clipRange = AppState::ClipRangeSel{};
        state.selectedTempoMarker = -1;
        state.selectedMarker = -1;
        state.versionCurrent = id;
        state.statusMessage = "버전 불러옴: " + n.name + " (이어서 체크인하면 가지가 뻗습니다)";
        return;
    }
}

// 오른쪽으로 뻗는 트리: 깊이 = x, 잎부터 행을 배정하고 부모는 자식들 가운데.
static void drawVersionTree(AppState& state, float height) {
    ImGui::BeginChild("vertree", ImVec2(0.0f, height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const int n = (int)state.versions.size();
    if (n == 0) {
        ImGui::TextDisabled("체크인하면 여기에\n버전 노드가 생깁니다");
        ImGui::EndChild();
        return;
    }

    // id -> 배열 인덱스 (부모는 항상 먼저 만들어지므로 앞에 있다)
    auto idxOf = [&](int id) {
        for (int i = 0; i < n; ++i)
            if (state.versions[i].id == id) return i;
        return -1;
    };
    std::vector<std::vector<int>> kids(n);
    std::vector<int> roots, depth(n, 0), row(n, -1);
    for (int i = 0; i < n; ++i) {
        const int p = idxOf(state.versions[i].parent);
        if (p < 0) roots.push_back(i);
        else {
            kids[p].push_back(i);
            depth[i] = depth[p] + 1;
        }
    }
    // 후위 순회(명시적 스택)로 잎에 행을 주고 부모는 첫/끝 자식의 가운데
    int nextRow = 0;
    std::vector<std::pair<int, bool>> st;
    for (int r = (int)roots.size() - 1; r >= 0; --r) st.push_back({roots[r], false});
    while (!st.empty()) {
        auto [i, done] = st.back();
        st.pop_back();
        if (!done) {
            st.push_back({i, true});
            for (int k = (int)kids[i].size() - 1; k >= 0; --k) st.push_back({kids[i][k], false});
        } else {
            if (kids[i].empty()) row[i] = nextRow++;
            else row[i] = (row[kids[i].front()] + row[kids[i].back()]) / 2;
        }
    }

    // 네모 노드(안에 버전명) 기준 배치. 한 칸(dx)은 최대 노드 폭 + 여백.
    constexpr float rowH = 24.0f, boxH = 18.0f, padX = 6.0f;
    float maxBoxW = 26.0f;
    for (int i = 0; i < n; ++i)
        maxBoxW = std::max(maxBoxW,
                           ImGui::CalcTextSize(state.versions[i].name.c_str()).x + padX * 2.0f);
    const float dx = maxBoxW + 18.0f;
    const ImVec2 org(ImGui::GetCursorScreenPos().x + 4.0f,
                     ImGui::GetCursorScreenPos().y + 12.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 노드 중심 좌표와 그 노드의 상자 절반 폭
    auto posOf = [&](int i) {
        return ImVec2(org.x + depth[i] * dx + maxBoxW * 0.5f, org.y + row[i] * rowH);
    };
    auto halfW = [&](int i) {
        return std::max(26.0f, ImGui::CalcTextSize(state.versions[i].name.c_str()).x +
                                   padX * 2.0f) *
               0.5f;
    };

    float maxX = 0.0f;
    for (int i = 0; i < n; ++i) {
        const ImVec2 c = posOf(i);
        maxX = std::max(maxX, c.x + halfW(i) - org.x);
        // 부모 오른쪽 변 -> 내 왼쪽 변 연결 곡선 (깃 그래프처럼 부드럽게)
        const int p = idxOf(state.versions[i].parent);
        if (p >= 0) {
            const ImVec2 pc = posOf(p);
            const float x0 = pc.x + halfW(p), x1 = c.x - halfW(i);
            const float mid = (x1 - x0) * 0.55f;
            dl->AddBezierCubic(ImVec2(x0, pc.y), ImVec2(x0 + mid, pc.y), ImVec2(x1 - mid, c.y),
                               ImVec2(x1, c.y), IM_COL32(130, 130, 150, 200), 1.6f);
        }
    }
    for (int i = 0; i < n; ++i) {
        auto& node = state.versions[(std::size_t)i]; // 팝업에서 이름/메모를 수정한다
        const ImVec2 c = posOf(i);
        const float hw = halfW(i);
        const ImVec2 b0(c.x - hw, c.y - boxH * 0.5f);
        const ImVec2 b1(c.x + hw, c.y + boxH * 0.5f);
        ImGui::SetCursorScreenPos(b0);
        ImGui::PushID(node.id);
        ImGui::InvisibleButton("vn", ImVec2(b1.x - b0.x, b1.y - b0.y));
        const bool hov = ImGui::IsItemHovered();
        // 이름/메모 편집용 버퍼: 팝업이 열리는 순간 현재 값으로 채운다
        static char nameBuf[64] = "";
        static char noteBuf[256] = "";
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) checkoutVersion(state, node.id);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            state.versionCtxNode = node.id;
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
            std::snprintf(noteBuf, sizeof(noteBuf), "%s", node.note.c_str());
            ImGui::OpenPopup("verctx");
        }
        const bool cur = node.id == state.versionCurrent;
        // 네모 노드: 현재 = 노란 배경 + 어두운 글씨, 나머지 = 파란 계열 + 흰 글씨
        dl->AddRectFilled(b0, b1,
                          cur ? IM_COL32(240, 200, 90, 255)
                              : IM_COL32(70, 110, 170, hov ? 255 : 225),
                          4.0f);
        dl->AddRect(b0, b1,
                    (cur || hov) ? IM_COL32(255, 255, 255, 200) : IM_COL32(20, 22, 28, 180), 4.0f,
                    0, 1.4f);
        const ImVec2 ts = ImGui::CalcTextSize(node.name.c_str());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    cur ? IM_COL32(40, 32, 8, 255) : IM_COL32(240, 244, 250, 255),
                    node.name.c_str());
        // 메모가 있는 노드는 오른쪽 위에 작은 점 (호버하면 메모가 툴팁에 보임)
        if (!node.note.empty())
            dl->AddCircleFilled(ImVec2(b1.x - 3.5f, b0.y + 3.5f), 2.2f,
                                IM_COL32(255, 235, 150, 255));
        if (hov) {
            if (node.note.empty())
                ImGui::SetTooltip("%s%s\n클릭: 불러오기 · 우클릭: 이름/메모·가지 뻗기·삭제",
                                  node.name.c_str(), cur ? " (현재)" : "");
            else
                ImGui::SetTooltip("%s%s\n메모: %s\n클릭: 불러오기 · 우클릭: 이름/메모·가지 뻗기·삭제",
                                  node.name.c_str(), cur ? " (현재)" : "", node.note.c_str());
        }
        if (ImGui::BeginPopup("verctx")) {
            const int tid = state.versionCtxNode;
            // 이름/메모: 입력하는 대로 바로 반영된다 (버퍼는 열릴 때 채움)
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("이름", nameBuf, sizeof(nameBuf)) && nameBuf[0] != '\0')
                node.name = nameBuf;
            ImGui::SameLine();
            if (ImGui::Button("확인")) { // 이름/메모 확정 + 팝업 닫기
                if (nameBuf[0] != '\0') node.name = nameBuf;
                node.note = noteBuf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetNextItemWidth(230);
            if (ImGui::InputText("메모", noteBuf, sizeof(noteBuf))) node.note = noteBuf;
            ImGui::Separator();
            if (ImGui::MenuItem("이 버전에서 가지 뻗기 (불러오기)")) checkoutVersion(state, tid);
            if (ImGui::MenuItem("삭제 (하위 가지 포함)")) {
                // 대상과 그 자손을 모두 지운다. 현재 노드가 지워지면 부모로 이동.
                std::vector<int> dead{tid};
                for (std::size_t k = 0; k < dead.size(); ++k)
                    for (const auto& v : state.versions)
                        if (v.parent == dead[k]) dead.push_back(v.id);
                int newCur = state.versionCurrent;
                for (int d : dead)
                    if (d == newCur)
                        for (const auto& v : state.versions)
                            if (v.id == tid) { newCur = v.parent; break; }
                state.versions.erase(
                    std::remove_if(state.versions.begin(), state.versions.end(),
                                   [&](const AppState::VersionNode& v) {
                                       return std::find(dead.begin(), dead.end(), v.id) !=
                                              dead.end();
                                   }),
                    state.versions.end());
                state.versionCurrent = newCur;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    // 스크롤 범위 확보 (트리가 오른쪽/아래로 자랄 수 있게)
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(ImVec2(maxX + dx, nextRow * rowH + 14.0f));
    ImGui::EndChild();
}

// ---------------------------------------------------------
// 트랜스포트
// ---------------------------------------------------------
void drawTransport(AppState& state) {
    if (!state.showTransport) return;
    ImGui::Begin("트랜스포트", &state.showTransport);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    const bool playing = state.player && state.player->isPlaying();

    // 기능을 6개 세트로 나눠 세로로 쌓고, 세로 구분선으로 가로 배치한다.
    // 메트로놈 | 카운트인은 각자 칸을 갖고 일반|강조 소리를 나란히 설정한다.
    const float ds = uiDpiScale(); // 칸 폭도 DPI만큼 (글자가 커진 만큼 넓어야 안 겹친다)
    const float colW[6] = {130.0f * ds, 190.0f * ds, 170.0f * ds,
                           400.0f * ds, 400.0f * ds, 250.0f * ds};
    const float tableW =
        colW[0] + colW[1] + colW[2] + colW[3] + colW[4] + colW[5] + 40.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    if (availW > tableW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - tableW) * 0.5f);
    if (ImGui::BeginTable("transport_sets", 6, ImGuiTableFlags_BordersInnerV,
                          ImVec2(tableW, 0.0f))) {
        ImGui::TableSetupColumn("c0", ImGuiTableColumnFlags_WidthFixed, colW[0]);
        ImGui::TableSetupColumn("c1", ImGuiTableColumnFlags_WidthFixed, colW[1]);
        ImGui::TableSetupColumn("c2", ImGuiTableColumnFlags_WidthFixed, colW[2]);
        ImGui::TableSetupColumn("c3", ImGuiTableColumnFlags_WidthFixed, colW[3]);
        ImGui::TableSetupColumn("c4", ImGuiTableColumnFlags_WidthFixed, colW[4]);
        ImGui::TableSetupColumn("c5", ImGuiTableColumnFlags_WidthFixed, colW[5]);
        ImGui::TableNextRow();

        // ── 세트 1: 재생/녹음 ──
        ImGui::TableSetColumnIndex(0);
        sectionHeader("재생");
        if (playing) {
            if (ImGui::Button(ICON_STOP " 정지", uiVec(110, 0))) {
                stopTransport(state);
                state.statusMessage = state.recording ? "녹음 정지" : "정지";
            }
        } else {
            if (ImGui::Button(ICON_PLAY " 재생", uiVec(110, 0))) startPlayback(state);
        }
        if (state.recording) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
            if (ImGui::Button(ICON_RECORD " 녹음중", uiVec(110, 0))) {
                stopTransport(state);
                state.statusMessage = "녹음 정지";
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button(ICON_RECORD " 녹음", uiVec(110, 0))) {
                if (state.song.tracks.empty()) { // 기록할 트랙이 없으면 하나 만든다
                    seq::Track t;
                    t.name = "Track 1";
                    state.song.tracks.push_back(t);
                    state.selectedTrack = 0;
                    addTrackEq(state, state.song.tracks.back()); // 기본 EQ 장착
                }
                {
                    // MIDI 입력 포트가 없어도 녹음은 시작한다 — 건반 연주(컴퓨터
                    // 키보드)만으로도 기록되기 때문. 포트가 있으면 그대로 함께 기록.
                    const bool noPort = state.input == nullptr || !state.input->isOpen();
                    if (state.output && !state.output->isOpen())
                        state.output->openPort((unsigned)state.selectedOutputPort);
                    state.snapshot(); // 녹음 시작 전 상태를 남겨 한 번에 되돌리기
                    for (auto& o : state.openRecNotes) o.active = false;
                    state.recording = true;
                    if (noPort) state.musicalTyping = true; // 바로 칠 수 있게 켜준다
                    // 카운트인이면 한 마디(박자 설정 기준) 프리롤 후 곡 시작(틱0)부터 기록
                    const uint32_t preRoll = state.countIn ? countInTicks(state) : 0;
                    state.player->play(state.song, 0, /*keepAlive=*/true, preRoll);
                    state.statusMessage =
                        state.countIn
                            ? "카운트인 후 녹음"
                            : (noPort ? "녹음 중 — 건반 연주로 입력하세요 (Z~M)"
                                      : "녹음 중 (연주하세요)");
                }
            }
        }
        if (ImGui::Button(ICON_PREVIOUS " 처음으로", uiVec(110, 0))) {
            stopTransport(state);
            seekTo(state, 0); // 플레이헤드 + 트랙 뷰/피아노 롤 화면도 처음으로
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Space");
        ImGui::Checkbox("건반 연주", &state.musicalTyping);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("컴퓨터 키보드로 연주합니다 (녹음도 됨)\n"
                              "Z X C V B N M = 도~시 · S D G H J = 검은건반\n"
                              ", = 한 옥타브 위 도 · [ ] = 옥타브 이동");
        if (state.musicalTyping) {
            ImGui::Text("옥타브: C%d", state.mtOctave);
            ImGui::SameLine();
            if (ImGui::SmallButton("-##oct")) state.mtOctave = std::max(0, state.mtOctave - 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("+##oct")) state.mtOctave = std::min(8, state.mtOctave + 1);
        }

        // ── 세트 2: 템포·위치 ──
        ImGui::TableSetColumnIndex(1);
        sectionHeader("템포·위치");
        float bpm = (float)state.song.bpm;
        if (sliderFloatPM("BPM", &bpm, 40.0f, 240.0f, 1.0f, "%.0f", 130.0f)) state.song.bpm = bpm;
        const seq::BarBeatTick pos = seq::toBarBeatTick(state.playPosTick, state.song.ppqn);
        ImGui::Text("위치  %d : %d : %03d", pos.bar, pos.beat, pos.tick);
        ImGui::Checkbox("재생 헤드 따라가기", &state.followPlayhead);

        // 템포 변경(곡 중간에 BPM이 바뀌는 지점) 편집. 슬라이더는 시작 템포.
        if (ImGui::SmallButton("템포 변경...")) ImGui::OpenPopup("tempomap");
        if (!state.song.tempoChanges.empty()) {
            ImGui::SameLine();
            // 마커와 같은 주황색 -> "이 색 = 템포 변경"으로 통일해 알아보기 쉽게
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%d곳 · 현재 %.0f BPM",
                               (int)state.song.tempoChanges.size(),
                               seq::bpmAtTick(state.song, state.playPosTick));
        }
        if (ImGui::BeginPopup("tempomap")) {
            ImGui::TextDisabled("지정한 마디부터 새 템포로 재생합니다.\n"
                                "BPM 슬라이더 = 시작 템포. 재생 중 편집은 다음 재생부터.");
            ImGui::Separator();
            const uint32_t tpb2 = songTicksPerBar(state);
            int removeIdx = -1;
            bool changed = false;
            for (int ti = 0; ti < (int)state.song.tempoChanges.size(); ++ti) {
                auto& tc = state.song.tempoChanges[(std::size_t)ti];
                ImGui::PushID(ti);
                int bar = (int)(tc.tick / tpb2) + 1;
                ImGui::SetNextItemWidth(96);
                if (ImGui::InputInt("마디##t", &bar)) {
                    state.snapshot();
                    tc.tick = (uint32_t)std::max(0, bar - 1) * tpb2;
                    changed = true;
                }
                ImGui::SameLine();
                double b = tc.bpm;
                ImGui::SetNextItemWidth(76);
                if (ImGui::InputDouble("BPM##t", &b, 0, 0, "%.1f",
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
                    state.snapshot();
                    tc.bpm = std::clamp(b, 20.0, 300.0);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("삭제")) removeIdx = ti;
                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                state.snapshot();
                state.song.tempoChanges.erase(state.song.tempoChanges.begin() + removeIdx);
                changed = true;
            }
            if (ImGui::Button("+ 추가 (재생 위치 마디)")) {
                state.snapshot();
                seq::TempoChange tc;
                tc.tick = (state.playPosTick / tpb2) * tpb2; // 마디 경계로 스냅
                tc.bpm = seq::bpmAtTick(state.song, state.playPosTick);
                state.song.tempoChanges.push_back(tc);
                changed = true;
            }
            if (changed)
                std::stable_sort(state.song.tempoChanges.begin(), state.song.tempoChanges.end(),
                                 [](const seq::TempoChange& a, const seq::TempoChange& b2) {
                                     return a.tick < b2.tick;
                                 });
            ImGui::EndPopup();
        }

        // ── 세트 3: 루프 ──
        ImGui::TableSetColumnIndex(2);
        sectionHeader("루프");
        if (ImGui::Checkbox("루프 켜기", &state.loopEnabled) && state.loopEnabled &&
            state.clipRange.track >= 0 && state.clipRange.t1 > state.clipRange.t0) {
            // 클립에서 Ctrl+드래그로 잡아둔 구간이 있으면 그 범위가 곧 루프가 된다
            state.loopStartTick = state.clipRange.t0;
            state.loopEndTick = state.clipRange.t1;
            state.statusMessage = "루프 = 선택 구간";
        }
        // 마디 입력 = 마디 스냅 편집기. 세밀한 구간은 트랙 뷰에서 핸들을 드래그.
        {
            const uint32_t tpbT = songTicksPerBar(state);
            int sb = (int)(state.loopStartTick / tpbT) + 1;
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("시작 마디", &sb))
                state.loopStartTick = (uint32_t)std::max(0, sb - 1) * tpbT;
            int eb = (int)((state.loopEndTick + tpbT - 1) / tpbT);
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("끝 마디", &eb))
                state.loopEndTick = (uint32_t)std::max(1, eb) * tpbT;
            if (state.loopEndTick <= state.loopStartTick)
                state.loopEndTick = state.loopStartTick + tpbT;
        }
        // 연습 반복 카운터 (루프가 꺼져 있어도 항상 보인다)
        ImGui::Text("반복: %d회", state.loopCount);
        ImGui::SameLine();
        if (ImGui::SmallButton("초기화##lc")) state.loopCount = 0;

        // ── 세트 4: 메트로놈 (켜기+박자, 일반|강조 나란히) ──
        ImGui::TableSetColumnIndex(3);
        sectionHeader("메트로놈");
        ImGui::Checkbox("켜기##metro", &state.metronome);
        ImGui::SameLine();
        const char* kSigs[3] = {"4/4", "3/4", "6/8"};
        ImGui::SetNextItemWidth(64);
        ImGui::Combo("##metrosig", &state.metroSigIndex, kSigs, 3); // 박자
        ImGui::SameLine();
        sliderFloatPM("음량##mv", &state.metroVolume, 0.0f, 1.5f, 0.05f, "%.2f", 110.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("일반 클릭");
        drawClickSoundEditor(state, 0);
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("강조 (마디 첫 박)");
        drawClickSoundEditor(state, 2);
        ImGui::EndGroup();

        // ── 세트 5: 카운트인 (켜기+횟수, 일반|강조 나란히) ──
        ImGui::TableSetColumnIndex(4);
        sectionHeader("카운트인");
        ImGui::Checkbox("켜기##ci", &state.countIn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("재생/녹음 시작 전에 정한 횟수만큼 클릭을 셉니다.\n"
                              "루프가 켜져 있으면 매 바퀴 되돌아갈 때도 카운트인합니다.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("회##cibeats", &state.countInBeats); // 몇 번 세고 들어갈지
        state.countInBeats = std::clamp(state.countInBeats, 1, 16);
        ImGui::SameLine();
        sliderFloatPM("음량##civ", &state.countInVolume, 0.0f, 1.5f, 0.05f, "%.2f", 110.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("일반 클릭");
        drawClickSoundEditor(state, 1);
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("강조 (첫 클릭)");
        drawClickSoundEditor(state, 3);
        ImGui::EndGroup();

        // ── 세트 6: 분기 (깃 브랜치식 버전 체크인/가지 뻗기) ──
        ImGui::TableSetColumnIndex(5);
        sectionHeader("분기");
        // 체크인: 이름/메모를 적고 "생성"을 눌러야만 노드가 만들어진다.
        static char ciName[64] = "";
        static char ciNote[256] = "";
        if (ImGui::SmallButton("체크인")) {
            std::snprintf(ciName, sizeof(ciName), "V%d", state.versionNextId);
            ciNote[0] = '\0';
            ImGui::OpenPopup("checkin_new");
        }
        if (ImGui::BeginPopup("checkin_new")) {
            ImGui::TextDisabled("현재 곡을 버전 노드로 저장합니다");
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("이름##ci", ciName, sizeof(ciName));
            ImGui::SetNextItemWidth(230);
            ImGui::InputText("메모##ci", ciNote, sizeof(ciNote));
            if (ImGui::Button("생성")) {
                checkinVersion(state, ciName, ciNote);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("취소")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (state.versionCurrent >= 0) {
            // 현재 노드 이름 표시
            const char* curName = "?";
            for (const auto& v : state.versions)
                if (v.id == state.versionCurrent) { curName = v.name.c_str(); break; }
            ImGui::TextDisabled("현재: %s", curName);
        } else {
            ImGui::TextDisabled("(체크인 전)");
        }
        drawVersionTree(state, 86.0f);

        ImGui::EndTable();
    }

    // ── 스크러버: |◀ [위치 바] ▶| (가장 긴 콘텐츠 끝 기준) ──
    const uint32_t endTick = std::max<uint32_t>(1, songEndTicks(state));
    if (ImGui::Button("|◀")) seekTo(state, 0);           // 처음으로
    ImGui::SameLine();
    const float barW = std::max(60.0f, ImGui::GetContentRegionAvail().x - 44.0f);
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float barH = 18.0f;
    ImGui::InvisibleButton("scrubber", ImVec2(barW, barH));
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    const float frac = std::clamp((float)state.playPosTick / (float)endTick, 0.0f, 1.0f);
    sdl->AddRectFilled(bp, ImVec2(bp.x + barW, bp.y + barH), IM_COL32(30, 32, 38, 255), 3.0f);
    sdl->AddRectFilled(bp, ImVec2(bp.x + barW * frac, bp.y + barH), IM_COL32(70, 110, 160, 255),
                       3.0f);
    // 템포 변경 지점: 스크러버 위쪽에 주황 삼각 마커 (호버로 BPM 확인)
    for (const auto& tc : state.song.tempoChanges) {
        const float fx =
            bp.x + barW * std::clamp((float)tc.tick / (float)endTick, 0.0f, 1.0f);
        sdl->AddTriangleFilled(ImVec2(fx - 4.5f, bp.y), ImVec2(fx + 4.5f, bp.y),
                               ImVec2(fx, bp.y + 7.0f), IM_COL32(255, 165, 70, 240));
        if (ImGui::IsMouseHoveringRect(ImVec2(fx - 5.0f, bp.y), ImVec2(fx + 5.0f, bp.y + 8.0f)))
            ImGui::SetTooltip("%d마디부터 %.0f BPM",
                              (int)(tc.tick / (songTicksPerBar(state))) + 1,
                              tc.bpm);
    }
    const float mx = bp.x + barW * frac;
    sdl->AddRectFilled(ImVec2(mx - 2, bp.y - 2), ImVec2(mx + 2, bp.y + barH + 2),
                       IM_COL32(235, 235, 240, 255), 2.0f);
    // 드래그로 스크럽: 재생 중이면 잠깐 멈췄다가 놓을 때 이어서
    if (ImGui::IsItemActivated()) {
        state.seekWasPlaying = state.player && state.player->isPlaying() && !state.recording;
        if (state.seekWasPlaying) {
            state.player->stop();
            if (state.audioClips) state.audioClips->stopAudio();
        }
    }
    if (ImGui::IsItemActive()) {
        const float m = ImGui::GetIO().MousePos.x;
        const float f = std::clamp((m - bp.x) / barW, 0.0f, 1.0f);
        state.playPosTick = (uint32_t)(f * endTick);
        state.scrollToPlayhead = true; // 뷰가 스크러버를 따라 이동
        if (state.audioClips) state.audioClips->seekAudio(tickToFrame(state, state.playPosTick));
    }
    if (ImGui::IsItemDeactivated() && state.seekWasPlaying) {
        state.seekWasPlaying = false;
        startPlayback(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("▶|")) seekTo(state, endTick); // 끝으로 (가장 긴 콘텐츠)

    ImGui::End();
}
} // namespace midipro::gui
