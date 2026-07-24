// =============================================================
// MidiPro - gui/PanelsGuitarTab.cpp
// 타브(TAB) 악보 창: 기타 트랙의 노트를 6줄 타브로 표시한다.
// 위에서부터 1번줄(높은 E) ~ 6번줄(낮은 E), 표준 튜닝(EADGBE).
//
// 줄/프렛 배정: 각 노트마다 "프렛 번호가 가장 작은 줄"을 고른다
// (로우 포지션 우선 — 자동 타브의 일반적인 휴리스틱).
// 편집은 피아노 롤에서 하고, 이 창은 보기/이동 전용이다.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "audio/AudioClip.h"
#include "audio/PitchDetect.h"
#include "pdf/PdfTab.h"
#include "sequencer/TabImport.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"

#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace midipro::gui {

namespace {

// 파일 열기 대화상자. 취소하면 빈 문자열. (한글 경로 지원 — 와이드 API)
std::string openFileDialogW(const wchar_t* filter, const wchar_t* defExt) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    char utf8[MAX_PATH * 4] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return std::string(utf8);
}

std::string openPdfDialog() {
    return openFileDialogW(L"PDF 악보 (*.pdf)\0*.pdf\0모든 파일\0*.*\0", L"pdf");
}

// 연습 반주 음원 열기
std::string openAudioDialog() {
    return openFileDialogW(L"오디오 (*.mp3;*.wav;*.flac)\0*.mp3;*.wav;*.flac\0"
                           L"모든 파일\0*.*\0",
                           L"mp3");
}

// UTF-8 경로의 파일을 바이트로 읽는다 (한글 경로 — 와이드 경로 사용).
bool readFileBytes(const std::string& utf8, std::vector<uint8_t>& out) {
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath((std::size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wpath[0], wlen);
    std::ifstream f(wpath.c_str(), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    if (size <= 0) return false;
    out.resize((std::size_t)size);
    f.seekg(0);
    f.read((char*)out.data(), size);
    return (bool)f;
}

// 표준 튜닝 개방현 (위 = 1번줄 high E). 표기 라벨은 타브 관례(e B G D A E).
constexpr int kOpen[6] = {64, 59, 55, 50, 45, 40};
constexpr const char* kStringLabel[6] = {"e", "B", "G", "D", "A", "E"};
constexpr int kMaxFret = 24;
constexpr float kStringGap = 22.0f; // 줄 간격 (프렛 숫자가 들어갈 높이)
constexpr float kLabelW = 34.0f;

// 노트 -> (줄, 프렛). 프렛이 가장 작은 줄을 고른다. 음역 밖이면 false.
bool assignString(int note, int& strOut, int& fretOut) {
    int bestS = -1, bestF = 999;
    for (int s = 0; s < 6; ++s) {
        const int f = note - kOpen[s];
        if (f >= 0 && f <= kMaxFret && f < bestF) {
            bestF = f;
            bestS = s;
        }
    }
    if (bestS < 0) return false;
    strOut = bestS;
    fretOut = bestF;
    return true;
}
} // namespace

void drawGuitarTab(AppState& state) {
    if (!state.showTab) return;
    ImGui::SetNextWindowSize(ImVec2(960, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("기타 연습", &state.showTab)) {
        ImGui::End();
        return;
    }
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // ── 연습 트랙 목록 (곡의 MIDI 트랙과 완전히 분리) ──
    std::vector<int> pracIdx;
    for (int i = 0; i < (int)state.song.tracks.size(); ++i)
        if (state.song.tracks[(std::size_t)i].practice) pracIdx.push_back(i);
    // 선택 보정: 없어졌거나 연습 트랙이 아니면 첫 연습 트랙으로
    if (state.practiceTrack >= (int)state.song.tracks.size() ||
        (state.practiceTrack >= 0 &&
         !state.song.tracks[(std::size_t)state.practiceTrack].practice))
        state.practiceTrack = -1;
    if (state.practiceTrack < 0 && !pracIdx.empty()) state.practiceTrack = pracIdx[0];

    if (pracIdx.empty()) {
        ImGui::TextWrapped(
            "연습 트랙이 없습니다. 트랙을 만들고 타브(PDF/텍스트)를 가져오면 "
            "악보를 보며 연습할 수 있습니다.\n"
            "여기의 트랙은 곡 작업용 MIDI 트랙과 완전히 별개입니다.");
        ImGui::Spacing();
        if (ImGui::Button("연습 트랙 만들기", ImVec2(160, 0))) addPracticeTrack(state);
        ImGui::End();
        return;
    }

    // ── 왼쪽: 연습 트랙 패널 (선택 / 추가·삭제 / 녹음 / 입력 채널) ──
    ImGui::BeginChild("prac_tracks", ImVec2(186, 0), ImGuiChildFlags_Borders);
    if (ImGui::Button("+ 트랙", ImVec2(82, 0))) addPracticeTrack(state);
    ImGui::SameLine();
    if (ImGui::Button("삭제", ImVec2(82, 0)) && state.practiceTrack >= 0) {
        deleteTrack(state, state.practiceTrack);
        state.practiceTrack = -1;
        ImGui::EndChild();
        ImGui::End();
        return; // 인덱스가 바뀌었다 — 다음 프레임에 다시 그린다
    }
    ImGui::Separator();
    for (int idx : pracIdx) {
        auto& t = state.song.tracks[(std::size_t)idx];
        ImGui::PushID(idx);
        const bool sel = (idx == state.practiceTrack);
        const bool inTab =
            std::find(state.tabTracks.begin(), state.tabTracks.end(), idx) !=
            state.tabTracks.end();
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "%s%s", inTab ? "* " : "  ", t.name.c_str());
        if (ImGui::Selectable(lbl, sel)) {
            if (ImGui::GetIO().KeyShift)
                toggleTabTrack(state, idx); // Shift+클릭 = 같이 띄우기
            else
                state.practiceTrack = idx;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nShift+클릭: 여러 트랙 동시 표시(*)", t.name.c_str());
        // 녹음 준비/진행 (오디오 입력 → 이 트랙에 클립으로)
        const bool recThis = (state.audioRecTrack == idx);
        if (recThis) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 60, 60, 255));
        if (ImGui::SmallButton(recThis ? "■ 정지" : "● 녹음")) {
            if (recThis)
                stopAudioRecording(state);
            else
                startAudioRecording(state, idx);
        }
        if (recThis) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("오디오 입력(ASIO/일반)을 이 트랙에 녹음합니다.\n"
                              "연주를 녹음해 다시 들어볼 수 있습니다.");
        // ASIO 저지연 모니터 — 판정에 쓰는 입력을 여는 스위치이기도 하다.
        // (ASIO 듀플렉스 스트림은 하나뿐이라 한 번에 한 트랙만)
        if (state.audioInput) {
            auto* in = state.audioInput;
            const bool asioThis = (state.asioTrack == idx);
            ImGui::SameLine();
            if (asioThis) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 130, 90, 255));
                if (ImGui::SmallButton("ASIO 중지")) {
                    in->stopAsio();
                    state.asioTrack = -1;
                    refreshPlaybackIfPlaying(state);
                }
                ImGui::PopStyleColor();
            } else {
                if (ImGui::SmallButton("ASIO")) {
                    // ASIO가 이미 켜져 있으면 스트림을 재시작하지 않고 소유권만
                    // 넘긴다 — 드라이버 재개폐가 시스템을 멈추게 하던 원인 제거.
                    // (startAsio는 같은 장치면 채널 모드만 갱신하고 그대로 둔다)
                    if (in->startAsio(state.asioDeviceIndex, t.inputChannelMode)) {
                        state.asioTrack = idx; // 모니터 버스는 매 프레임 이걸 따라간다
                        state.statusMessage = "ASIO 입력 = 이 트랙 (저지연)";
                    } else {
                        state.statusMessage =
                            "ASIO를 열 수 없습니다 (설정 > 개인설정 > ASIO에서 장치 확인)";
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ASIO 저지연 입력을 이 트랙으로 잡습니다.\n"
                                      "연습 판정에 쓸 입력도 이걸로 열립니다.\n"
                                      "(ASIO 스트림은 하나뿐 — 다른 트랙에서 누르면 옮겨집니다)");
            }
        }
        ImGui::SetNextItemWidth(90);
        const char* kIn[] = {"입력 1+2", "입력 1", "입력 2"};
        int im = std::clamp(t.inputChannelMode, 0, 2);
        if (ImGui::Combo("##in", &im, kIn, 3)) {
            t.inputChannelMode = im;
            // 이 트랙이 지금 입력을 잡고 있으면 즉시 반영 (안 하면 이전 채널로 계속 됨)
            if (state.audioInput && state.asioTrack == idx)
                state.audioInput->setInputChannelMode(im);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ASIO 입력 채널 선택 (기타를 꽂은 채널).\n"
                              "이 트랙이 ASIO 입력 중이면 바로 적용됩니다.");
        // 볼륨 + 레벨 미터 (이 트랙 버스의 신호 — 포스트 FX·페이더)
        ImGui::SetNextItemWidth(104);
        ImGui::SliderFloat("##vol", &t.volume, 0.0f, 1.5f, "%.2f");
        if (ImGui::IsItemActivated()) state.snapshot();
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetTooltip("볼륨 %.0f%%", t.volume * 100.0f);
        ImGui::SameLine();
        {
            const int bus = t.channel & 0x0F;
            miniMeterH("##lvl", state.meterBus[bus], state.busPeakCache[bus], 46.0f,
                       ImGui::GetFrameHeight() * 0.62f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("입력/재생 레벨 (이펙트·볼륨 적용 후).\n"
                                  "클릭: 클리핑 표시 해제");
        }
        // FX 체인: 앰프 시뮬·오버드라이브 등을 걸어 연습 톤을 만든다.
        // (판정은 이펙트 앞의 드라이 신호로 하므로 톤을 바꿔도 정확도에 영향 없음)
        if (state.vst) drawTrackFxChain(state, idx, 160.0f, 74.0f, /*withInstrument=*/false);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup(); // 오른쪽: 툴바 + 타브

    // ── 표시할 트랙들: state.tabTracks (비어 있으면 선택한 연습 트랙 하나) ──
    std::vector<int> shown;
    for (int idx : state.tabTracks)
        if (idx >= 0 && idx < (int)state.song.tracks.size() &&
            state.song.tracks[(std::size_t)idx].practice &&
            std::find(shown.begin(), shown.end(), idx) == shown.end())
            shown.push_back(idx);
    if (shown.empty()) shown.push_back(state.practiceTrack);

    // ── 연습 모드 상태 (리듬 게임식 판정) ──
    // 레인에는 "음 단위"로 기록한다 — 코드는 구성음마다 울렸는지/빠졌는지 보여준다.
    struct PracNote {
        uint32_t tick;
        uint8_t note;  // 0 = 음정 불명
        int8_t strIdx; // 표시할 줄 (-1 = 모름)
        uint8_t state; // 0=빨강(틀림/안 울림) 1=주황(GOOD) 2=초록(GREAT)
    };
    static bool s_practice = false;
    static int s_great = 0, s_good = 0, s_bad = 0;
    static std::vector<PracNote> s_hits;
    static uint32_t s_lastCur = 0;
    static int s_lastJudge = -1;
    static double s_lastJudgeAt = -10.0;
    // ── 악보 주도 청취 (score-driven listening) ──
    // 온셋 감지 없이, 각 악보 이벤트마다 "그 시각 주변에서 기대 음의 배음
    // 에너지가 85ms 전 대비 뛰어올랐는가"를 직접 확인한다 (noteRiseAt).
    // 잔향·맥놀이·스트로크 중복·아르페지오가 원리적으로 문제가 안 된다.
    static uint32_t s_lastWp = 0;
    static std::size_t s_evIdx = 0; // pnotes에서 다음 활성화할 이벤트의 시작 인덱스
    struct ActiveEv {
        std::size_t beg, end; // pnotes[beg..end) = 같은 틱의 코드 구성음들
        uint32_t tick;
        bool late; // 1차(±140ms)에서 못 찾아 늦은 타격(+450ms)까지 재탐색 중
    };
    static std::vector<ActiveEv> s_activeEv;
    static float s_sens = 1.0f;    // 판정 감도 (작을수록 민감 = 상승 문턱 낮음)
    static float s_inLevel = 0.0f; // 입력 레벨 표시용
    static double s_lastInputAt = -10.0;
    static float s_judgeOfsMs = 0.0f;  // 수동 판정 오프셋 (+면 타격을 더 앞으로)
    static float s_lastDeltaMs = 0.0f; // 최근 타격과 악보 음의 시간차 (진단)
    static bool s_haveDelta = false;
    // 자동 지연 보정: 최근 타격들의 Δ 중앙값이 일관되게 치우쳐 있으면
    // 그만큼을 오프셋에 흡수한다 (장치 지연은 사용자가 알 수 없으므로 자동으로).
    static float s_autoOfsMs = 0.0f;
    static std::vector<float> s_deltaHist;
    // ── 자동 맞춤(보정) 모드 ──
    // 카운트인 4클릭(킥) 후 100BPM 클릭 16번(햇)에 맞춰 3번줄 개방현(G)을 친다.
    // 박마다 타격 시각과 "버틸 수 있는 최대 상승 배율"을 재서
    // 오프셋(시간차 중앙값)과 감도(여유 배율)를 자동으로 정한다.
    static bool s_calOpen = false; // 자동 맞춤 창
    static int s_calState = 0;   // 0=꺼짐 1=카운트인 2=측정
    static double s_calT0 = 0.0; // 첫 측정 박의 예정 시각 (GUI 시계)
    static int s_calClicked = 0; // 다음 클릭 인덱스 (음수 = 카운트인)
    static uint32_t s_calBeatWp[16] = {};
    static int s_calAnalyzed = 0;
    static int s_calBeatState[16] = {}; // 박별 결과: 0=대기 1=감지 2=놓침
    static std::vector<float> s_calDeltas, s_calRises;
    static char s_calMsg[160] = "";
    static int s_pracTrack = -1;
    static std::size_t s_pracNoteCount = 0;
    static int s_pracChoice = -1; // 사용자가 고른 연습 트랙 (-1 = 자동: 첫 표시 트랙)
    auto pracReset = [&]() {
        s_great = s_good = s_bad = 0;
        s_hits.clear();
        s_lastJudge = -1;
        s_haveDelta = false;
        s_deltaHist.clear(); // 자동 보정값 자체는 장치 속성이므로 유지
        s_evIdx = 0;
        s_activeEv.clear();
    };

    // ── 툴바 ── (표시 트랙 선택은 왼쪽 트랙 패널의 Shift+클릭이 담당)
    if (shown.size() == 1)
        ImGui::TextUnformatted(state.song.tracks[(std::size_t)shown[0]].name.c_str());
    else
        ImGui::Text("트랙 %d개 표시", (int)shown.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("확대##tab", &state.tabZoom, 0.02f, 0.5f, "%.2f px/tick");
    ImGui::SameLine();
    static bool s_importOpen = false;
    if (ImGui::Button("타브 가져오기")) s_importOpen = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PDF 악보를 열거나, 텍스트 타브(ASCII TAB)를 붙여넣어\n"
                          "노트로 변환합니다.\n"
                          "(스캔·사진 악보는 글자 정보가 없어 지원하지 않습니다)");
    ImGui::SameLine();
    if (ImGui::Button(s_practice ? "연습 끝" : "연습")) {
        s_practice = !s_practice;
        if (s_practice) {
            pracReset();
            // 판정은 입력 스트림이 열려 있어야 한다. 안 열려 있으면 ASIO(저지연)를
            // 먼저 시도하고, 안 되면 일반 입력이라도 연다 — 사용자가 '연습'만
            // 눌러도 바로 칠 수 있게. (실패하면 아래 경고가 뜬다)
            if (state.audioInput && !state.audioInput->asioActive() &&
                !state.audioInput->inputActive()) {
                auto& t0 = state.song.tracks[(std::size_t)shown[0]];
                if (state.audioInput->startAsio(state.asioDeviceIndex,
                                                t0.inputChannelMode)) {
                    state.asioTrack = shown[0];
                    state.statusMessage = "연습 시작 — ASIO 모니터를 켰습니다";
                } else if (state.audioInput->startInput()) {
                    state.statusMessage =
                        "연습 시작 — 일반 입력을 켰습니다 (ASIO가 지연이 훨씬 적습니다)";
                }
            }
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("기타를 오디오 입력에 연결하고 재생과 함께 따라 쳐 보세요.\n"
                          "누르면 입력(ASIO 우선)이 자동으로 열립니다.\n"
                          "음정과 박자를 판정합니다: GREAT(±60ms) / GOOD(±140ms) / BAD.\n"
                          "친 음은 맨 아래 '연주' 줄에 표시됩니다.");
    if (s_practice && state.audioInput && !state.audioInput->asioActive() &&
        !state.audioInput->inputActive()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "입력 안 열림!");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("오디오 입력을 열지 못했습니다 — 판정이 동작하지 않습니다.\n"
                              "설정 > 개인설정 > ASIO에서 장치를 확인하거나,\n"
                              "왼쪽 트랙의 [ASIO] 버튼을 눌러 보세요.");
    }
    if (s_practice) {
        // 연습 대상 트랙 고르기 (연습 트랙만 — 곡 트랙은 여기 나오지 않는다)
        if (s_pracChoice >= (int)state.song.tracks.size() ||
            (s_pracChoice >= 0 && !state.song.tracks[(std::size_t)s_pracChoice].practice))
            s_pracChoice = -1;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        const char* preview =
            s_pracChoice < 0 ? "자동 (첫 표시 트랙)"
                             : state.song.tracks[(std::size_t)s_pracChoice].name.c_str();
        if (ImGui::BeginCombo("##practrack", preview)) {
            if (ImGui::Selectable("자동 (첫 표시 트랙)", s_pracChoice < 0)) s_pracChoice = -1;
            for (int i : pracIdx) {
                ImGui::PushID(i);
                if (ImGui::Selectable(state.song.tracks[(std::size_t)i].name.c_str(),
                                      s_pracChoice == i))
                    s_pracChoice = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("어느 트랙의 악보를 기준으로 판정할지 고릅니다");

        // 정확도 + 마지막 판정
        const int total = s_great + s_good + s_bad;
        const int acc = total > 0 ? (int)((100.0 * s_great + 60.0 * s_good) / total) : 100;
        ImGui::SameLine();
        ImGui::Text("정확도 %d%%", acc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("GREAT %d · GOOD %d · BAD %d (놓친 음 포함)", s_great, s_good,
                              s_bad);
        if (s_lastJudge >= 0 && ImGui::GetTime() - s_lastJudgeAt < 1.2) {
            ImGui::SameLine();
            static const char* kJ[3] = {"BAD", "GOOD", "GREAT!"};
            static const ImVec4 kJC[3] = {ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                          ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                                          ImVec4(0.4f, 0.9f, 0.4f, 1.0f)};
            ImGui::TextColored(kJC[s_lastJudge], "%s", kJ[s_lastJudge]);
        }
        // 입력 레벨 + 감도 — "인식이 안 될" 때 원인을 눈으로 확인.
        // 이건 판정이 실제로 듣는 드라이 입력이다 (이펙트·볼륨 적용 전) —
        // 왼쪽 트랙의 미터(포스트 FX)와 달라도 정상이다.
        ImGui::SameLine();
        ImGui::TextUnformatted("입력");
        ImGui::SameLine();
        static AppState::MeterView s_inMeter;
        miniMeterH("##inlvl", s_inMeter, s_inLevel, 80.0f, ImGui::GetFrameHeight() * 0.62f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("판정이 듣는 입력 레벨 (드라이 — 이펙트 앞).\n"
                              "칠 때 움직여야 정상입니다. 클릭: 클리핑 표시 해제");
        if (ImGui::GetTime() - s_lastInputAt > 1.0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "입력 없음!");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("오디오 입력이 열려 있지 않습니다.\n"
                                  "트랙의 ASIO 모니터 또는 녹음 입력을 켜세요.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("감도##prac", &s_sens, 0.5f, 2.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("음 인정 문턱값 배율 — 낮출수록 관대해집니다.\n"
                              "제대로 쳤는데 빨강이 많으면 낮추고, 안 쳐도 인정되면 올리세요.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("오프셋##prac", &s_judgeOfsMs, -100.0f, 300.0f, "%.0fms");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("판정 시각 미세조정 — 장치 지연은 자동 보정이 흡수하므로\n"
                              "보통 0으로 두면 됩니다. 자동 보정 후에도 어긋날 때만 조정하세요.");
        ImGui::SameLine();
        if (ImGui::Button("자동 맞춤##calopen")) s_calOpen = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("창을 열어 클릭에 맞춰 기준음(3번줄 개방현)을 치면\n"
                              "오프셋과 감도를 자동으로 맞춥니다.");
        if (s_haveDelta) {
            ImGui::SameLine();
            const bool okD = std::fabs(s_lastDeltaMs) < 60.0f;
            ImGui::TextColored(okD ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                   : ImVec4(1.0f, 0.75f, 0.4f, 1.0f),
                               "Δ%+.0fms", s_lastDeltaMs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("최근 타격이 가장 가까운 악보 음보다 얼마나 늦었나(+)/빨랐나(-).\n"
                                  "치우침이 일관되면 자동 보정이 알아서 흡수합니다.");
        }
        if (s_autoOfsMs != 0.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("자동 %+.0fms", s_autoOfsMs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("자동으로 학습한 입력 장치 지연 보정입니다.\n"
                                  "박에 맞춰 몇 번 치면 저절로 맞춰집니다 — 슬라이더는 미세조정용.");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("표준 튜닝 EADGBE · 로우 포지션 자동 배정");

    // ── 트랜스포트 (연습 창 안에서 완결되게: 재생 · 템포 · 메트로놈) ──
    {
        const bool playingNow = state.player && state.player->isPlaying();
        if (ImGui::Button(playingNow ? "■ 정지##pracplay" : "▶ 재생##pracplay",
                          ImVec2(78, 0))) {
            if (playingNow) stopTransport(state);
            else startPlayback(state);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("재생/정지 (스페이스바와 같음)");
        ImGui::SameLine();
        if (ImGui::Button("|◀ 처음##pracrew")) seekTo(state, 0, /*scrollView=*/false);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        float bpmF = (float)state.song.bpm;
        if (ImGui::DragFloat("템포##prac", &bpmF, 0.5f, 20.0f, 300.0f, "%.0f BPM")) {
            if (ImGui::IsItemActivated()) state.snapshot();
            state.song.bpm = std::clamp((double)bpmF, 20.0, 300.0);
            refreshPlaybackIfPlaying(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("곡 템포. 느리게 잡고 연습하다 조금씩 올리세요.\n"
                              "반주를 물렸다면 [템포에 맞추기]를 다시 눌러야 합니다.");
        ImGui::SameLine();
        if (ImGui::Checkbox("메트로놈##prac", &state.metronome))
            state.statusMessage = state.metronome ? "메트로놈 켬" : "메트로놈 끔";
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("재생 중 박마다 클릭. 마디 첫 박은 강조음입니다.\n"
                              "소리·음량은 설정 > 개인설정에서 바꿀 수 있습니다.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("클릭##pracvol", &state.metroVolume, 0.0f, 1.5f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("메트로놈 음량");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        const char* kSig[] = {"4/4", "3/4", "6/8"};
        int sig = std::clamp(state.metroSigIndex, 0, 2);
        if (ImGui::Combo("박자##prac", &sig, kSig, 3)) state.metroSigIndex = sig;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("박자표 — 메트로놈 강조와 마디선 기준입니다.\n"
                              "타브를 가져오면 악보의 박자로 자동 설정됩니다.");
        ImGui::SameLine();
        ImGui::Checkbox("카운트인##prac", &state.countIn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("녹음 전 한 마디 클릭을 세고 시작합니다");

        // ── 루프 (마디 단위) ── 어려운 구간만 반복해서 연습한다
        const uint32_t tpbL = songTicksPerBar(state);
        ImGui::Checkbox("루프##prac", &state.loopEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("아래 구간을 반복 재생합니다 — 어려운 마디만 집중 연습.\n"
                              "루프가 돌 때마다 판정 기록이 새로 시작됩니다.");
        ImGui::SameLine();
        int sb = (int)(state.loopStartTick / tpbL) + 1;
        int eb = (int)((state.loopEndTick + tpbL - 1) / tpbL);
        ImGui::SetNextItemWidth(84);
        if (ImGui::InputInt("##loopsb", &sb)) {
            state.loopStartTick = (uint32_t)std::max(0, sb - 1) * tpbL;
            if (state.loopEndTick <= state.loopStartTick)
                state.loopEndTick = state.loopStartTick + tpbL;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("루프 시작 마디");
        ImGui::SameLine();
        ImGui::TextUnformatted("~");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84);
        if (ImGui::InputInt("마디##loopeb", &eb)) {
            state.loopEndTick = (uint32_t)std::max(1, eb) * tpbL;
            if (state.loopEndTick <= state.loopStartTick)
                state.loopEndTick = state.loopStartTick + tpbL;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("루프 끝 마디 (이 마디까지 포함)");
        ImGui::SameLine();
        if (ImGui::Button("현재 마디##loopcur")) {
            // 재생 위치가 든 마디 하나를 루프로 — 막히는 마디에서 바로 누른다
            const uint32_t bar = state.playPosTick / tpbL;
            state.loopStartTick = bar * tpbL;
            state.loopEndTick = (bar + 1) * tpbL;
            state.loopEnabled = true;
            state.statusMessage = "루프 = " + std::to_string(bar + 1) + "마디";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("재생 위치의 마디 하나를 루프로 잡습니다.\n"
                              "막히는 마디에서 누르면 바로 반복 연습이 됩니다.");
        ImGui::SameLine();
        if (ImGui::Button("+2마디##loopgrow")) {
            state.loopEndTick += 2 * tpbL;
            state.loopEnabled = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("루프 끝을 2마디 늘립니다 — 되면 조금씩 넓혀가세요");
        ImGui::SameLine();
        ImGui::TextDisabled("반복 %d회", state.loopCount);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("이 루프를 몇 바퀴 돌았는지");
    }

    // ── 반주 (MP3/WAV) ──
    // 반주는 선택한 연습 트랙 위의 보통 클립이다 — 아래 레인에서 파형을 보며
    // 드래그해 악보와 맞춘다. 원래 BPM을 알려주면 곡 템포를 낮췄을 때
    // [템포에 맞추기]로 음정을 유지한 채 같이 늘려준다.
    {
        const double songBpm = state.song.bpm > 1.0 ? state.song.bpm : 120.0;
        // 트랙에서 반주 클립이 사라졌으면(삭제 등) 슬롯도 비운다
        if (state.practiceBackingClip) {
            bool onTrack = false;
            for (const auto& t : state.song.tracks)
                for (const auto& c : t.clips)
                    if (c == state.practiceBackingClip) onTrack = true;
            if (!onTrack) {
                state.practiceBackingClip.reset();
                state.practiceBacking.reset();
                state.practiceBackingMadeRatio = 0.0;
            }
        }
        if (!state.practiceBackingClip) {
            if (ImGui::Button("반주 열기 (MP3/WAV)")) {
                const std::string path = openAudioDialog();
                if (!path.empty()) {
                    std::vector<uint8_t> bytes;
                    if (readFileBytes(path, bytes)) {
                        std::string nm = path;
                        const std::size_t sl = nm.find_last_of("/\\");
                        if (sl != std::string::npos) nm = nm.substr(sl + 1);
                        auto clip = audio::decodeAudioAuto(bytes.data(), bytes.size(), nm);
                        if (clip && state.practiceTrack >= 0) {
                            state.snapshot();
                            clip->startTick = 0;
                            // 재생본은 원본과 별개 객체 — 나중에 다시 늘릴 때
                            // 원본이 그대로 남아 있어야 겹쳐 늘리지 않는다.
                            auto play = std::make_shared<audio::AudioClip>(*clip);
                            play->buildPeaks();
                            state.song.tracks[(std::size_t)state.practiceTrack]
                                .clips.push_back(play);
                            state.practiceBacking = clip; // 원본 보관 (재스트레치용)
                            state.practiceBackingClip = play;
                            state.practiceBackingMadeRatio = 1.0;
                            rebuildAudioMix(state);
                            refreshPlaybackIfPlaying(state);
                            state.statusMessage =
                                "반주 추가: " + nm +
                                " — 원래 BPM을 입력하고 파형을 드래그해 맞추세요";
                        } else if (!clip) {
                            state.statusMessage = "반주 디코드 실패: " + nm;
                        } else {
                            state.statusMessage = "연습 트랙을 먼저 고르세요";
                        }
                    } else {
                        state.statusMessage = "반주 파일을 읽지 못했습니다";
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("연습용 반주를 선택한 연습 트랙에 클립으로 넣습니다.\n"
                                  "아래 레인에서 파형을 보며 드래그해 악보와 맞추세요.");
        } else {
            ImGui::TextUnformatted("반주:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                               state.practiceBackingClip->name.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96);
            float obpm = (float)state.practiceBackingBpm;
            if (ImGui::DragFloat("원본 BPM", &obpm, 0.5f, 30.0f, 300.0f, "%.0f"))
                state.practiceBackingBpm = std::clamp((double)obpm, 30.0, 300.0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("이 음원의 원래 템포입니다.\n"
                                  "곡 템포와 이 값의 비율만큼 반주를 늘이거나 줄입니다.");
            // 지금 재생본이 필요한 배율로 만들어져 있는가?
            // (곡 템포·원본 BPM 둘 중 뭘 고쳐도 배율이 달라져 다시 잡힌다)
            const double ratio = state.practiceBackingBpm / songBpm; // >1 = 느리게(길게)
            const bool fresh = std::fabs(state.practiceBackingMadeRatio - ratio) < 0.001;
            ImGui::SameLine();
            ImGui::BeginDisabled(fresh || !state.practiceBacking);
            if (ImGui::Button("템포에 맞추기")) {
                // 항상 원본에서 늘린다 (스트레치를 겹쳐 걸면 음질이 무너진다).
                // 드래그로 맞춰둔 위치(startTick)는 그대로 유지한다.
                std::shared_ptr<audio::AudioClip> out;
                if (std::fabs(ratio - 1.0) < 0.001)
                    out = std::make_shared<audio::AudioClip>(*state.practiceBacking);
                else
                    out = audio::stretchClipPitchPreserve(*state.practiceBacking, ratio);
                if (out) {
                    state.snapshot();
                    out->name = state.practiceBacking->name;
                    out->startTick = state.practiceBackingClip->startTick;
                    out->gain = state.practiceBackingClip->gain;
                    out->buildPeaks();
                    // 트랙 위의 옛 재생본을 새 것으로 바꿔 끼운다
                    for (auto& t : state.song.tracks)
                        for (auto& c : t.clips)
                            if (c == state.practiceBackingClip) c = out;
                    state.practiceBackingClip = out;
                    state.practiceBackingMadeRatio = ratio;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "반주를 %.0f BPM에 맞춤 (%.2f배%s)",
                                  songBpm, ratio,
                                  (ratio < 0.5 || ratio > 2.0) ? " — 음질 저하 구간"
                                                               : ", 음정 유지");
                    state.statusMessage = msg;
                    rebuildAudioMix(state);
                    refreshPlaybackIfPlaying(state);
                } else {
                    state.statusMessage = "반주 스트레치 실패 (음원이 너무 짧습니다)";
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(fresh ? "이미 현재 곡 템포에 맞춰져 있습니다"
                                        : "지금 곡 템포에 맞게 반주를 늘입니다 (음정 유지).\n"
                                          "맞춰둔 위치는 그대로 두고, 몇 초 걸릴 수 있습니다.");
            // 상태: 맞음(초록) / 어긋남(주황, 몇 배가 필요한지)
            ImGui::SameLine();
            if (fresh) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%.0f BPM", songBpm);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%.0f BPM (%.2f배 필요)",
                                   songBpm, ratio);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "반주가 지금 템포에 안 맞습니다 — [템포에 맞추기]를 누르세요.");
            }
            if (ratio < 0.5 || ratio > 2.0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "(!)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("원본의 절반~두 배를 벗어나면 소리가 뭉개집니다.\n"
                                      "템포를 덜 극단적으로 잡아 주세요.");
            }
            // 시작 위치: 레인 드래그가 주 조작이고, 여기선 초 단위 미세조정
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            const double spt = 60.0 / (songBpm * (double)state.song.ppqn); // 초/틱
            float startSec = (float)((double)state.practiceBackingClip->startTick * spt);
            if (ImGui::DragFloat("시작", &startSec, 0.005f, 0.0f, 600.0f, "%.3fs")) {
                state.practiceBackingClip->startTick =
                    (uint32_t)std::max(0.0, (double)startSec / spt);
                rebuildAudioMix(state);
                refreshPlaybackIfPlaying(state);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("반주가 시작하는 위치 (초).\n"
                                  "아래 레인에서 파형을 드래그하는 게 더 편합니다.");
            ImGui::SameLine();
            if (ImGui::Button("반주 빼기")) {
                state.snapshot();
                for (auto& t : state.song.tracks)
                    t.clips.erase(std::remove(t.clips.begin(), t.clips.end(),
                                              state.practiceBackingClip),
                                  t.clips.end());
                state.practiceBackingClip.reset();
                state.practiceBacking.reset();
                state.practiceBackingMadeRatio = 0.0;
                rebuildAudioMix(state);
                refreshPlaybackIfPlaying(state);
                state.statusMessage = "반주 제거";
            }
        }
    }

    // ── 연습 판정 엔진 (매 프레임) ──
    // 연습 대상: 사용자가 고른 트랙, 없으면 첫 번째 표시 트랙
    const int pracTarget =
        !s_practice ? -1
                    : ((s_pracChoice >= 0 && s_pracChoice < (int)state.song.tracks.size())
                           ? s_pracChoice
                           : shown[0]);
    if (s_practice && state.audioClips && state.player) {
        const int pracTrack = pracTarget;
        auto& ptr = state.song.tracks[(std::size_t)pracTrack];
        const auto& pnotes = cachedNotes(ptr, pracTrack);
        // 트랙/악보가 바뀌면 판정 기록 초기화
        if (s_pracTrack != pracTrack || s_pracNoteCount != pnotes.size()) {
            s_pracTrack = pracTrack;
            s_pracNoteCount = pnotes.size();
            pracReset();
        }
        const double sr = state.audioClips->engineSampleRate();
        const double bpm = state.song.bpm > 1.0 ? state.song.bpm : 120.0;
        const bool playing = state.player->isPlaying();
        const uint32_t curTick = state.player->currentTick();

        // 되감기/루프를 감지하면 그 구간을 다시 판정할 수 있게 이벤트 커서를 되돌린다
        if (playing && curTick + 1920 < s_lastCur) {
            s_activeEv.clear();
            s_evIdx = 0;
            while (s_evIdx < pnotes.size() && pnotes[s_evIdx].startTick < curTick)
                ++s_evIdx;
        }
        s_lastCur = curTick;

        // 입력 레벨/입력 감시 (경고 표시용)
        {
            const uint32_t wp = state.audioClips->inputTapWritePos();
            if (wp != s_lastWp) {
                s_lastWp = wp;
                s_lastInputAt = ImGui::GetTime();
            }
            static std::vector<float> lv(512);
            if (state.audioClips->readInputTapRange(wp - 512, lv.data(), 512)) {
                float pk = 0.0f;
                for (float v : lv) pk = std::max(pk, std::fabs(v));
                s_inLevel = std::max(pk, s_inLevel * 0.92f);
            }
        }

        // ── 자동 맞춤 진행 (정지 상태 전용) ──
        if (s_calState > 0 && sr > 0.0) {
            constexpr int kCalBeats = 16;
            const double beatSec = 60.0 / 100.0; // 100BPM
            const double now = ImGui::GetTime();
            if (!s_calOpen) s_calState = 0; // 창을 닫으면 중단
            if (playing) {
                s_calState = 0;
                std::snprintf(s_calMsg, sizeof(s_calMsg), "재생이 시작돼 취소했습니다");
            }
            // 클릭 스케줄: 음수 인덱스 = 카운트인(킥), 0..15 = 측정(햇)
            while (s_calState > 0 && s_calClicked < kCalBeats &&
                   now >= s_calT0 + beatSec * (double)s_calClicked) {
                const bool countIn = s_calClicked < 0;
                triggerNote(state, 9, countIn ? (uint8_t)36 : (uint8_t)42,
                            countIn ? (uint8_t)112 : (uint8_t)120, 0.05);
                if (!countIn)
                    s_calBeatWp[s_calClicked] = state.audioClips->inputTapWritePos();
                ++s_calClicked;
                if (s_calClicked == 0) s_calState = 2;
            }
            // 박마다 창이 완성되는 대로 분석 (입력 링이 짧아 미루면 덮인다).
            // 판정 엔진과 같은 게이트로 G3(=55)의 상승을 찾고, 그 프레임이
            // "버틸 수 있는 최대 상승 배율"(여유)을 함께 잰다.
            if (s_calState == 2 && s_calAnalyzed < s_calClicked) {
                const int kHalf = 2048, kPre = 4096, kHop = 1024, kFr = 4096;
                const int early = (int)(0.25 * sr); // 박 앞 250ms부터
                const int lateE = (int)(0.40 * sr); // 박 뒤 400ms까지
                const int tailS = kPre + kHalf;
                const uint32_t wp2 = state.audioClips->inputTapWritePos();
                const uint32_t bw = s_calBeatWp[s_calAnalyzed];
                if ((int)(int32_t)(wp2 - bw) >= lateE + tailS) {
                    const uint32_t start = bw - (uint32_t)(early + kHalf + kPre);
                    const int n = early + kHalf + kPre + lateE + tailS;
                    static std::vector<float> cspan;
                    cspan.resize((std::size_t)n);
                    if (state.audioClips->readInputTapRange(start, cspan.data(), n)) {
                        const int nPos = (n - kFr) / kHop + 1;
                        static std::vector<double> tv, gv, evv;
                        static std::vector<int> sv;
                        tv.assign((std::size_t)nPos, 0.0);
                        gv.assign((std::size_t)nPos, 0.0);
                        evv.assign((std::size_t)nPos, 0.0);
                        sv.assign((std::size_t)nPos, 0);
                        for (int i = 0; i < nPos; ++i) {
                            double t = 0.0, g = 0.0;
                            int st = 0;
                            audio::noteBandPower(cspan.data() + i * kHop, kFr, sr, 55,
                                                 &t, &g, &st);
                            double e = 0.0;
                            const float* q = cspan.data() + i * kHop;
                            for (int k = 0; k < kFr; ++k) e += (double)q[k] * q[k];
                            tv[(std::size_t)i] = t;
                            gv[(std::size_t)i] = g;
                            evv[(std::size_t)i] = e;
                            sv[(std::size_t)i] = st;
                        }
                        double bestR = 0.0;
                        int firstI = -1;
                        for (int p = kPre; p + kPre + kFr <= n; p += kHop) {
                            const int i = p / kHop;
                            if (tv[(std::size_t)i] < 1e-6) continue;
                            if (gv[(std::size_t)i] >= 0.0 &&
                                tv[(std::size_t)i] < gv[(std::size_t)i] * 1.2)
                                continue;
                            if (sv[(std::size_t)i] == 0 || sv[(std::size_t)i] == 1)
                                continue;
                            if (tv[(std::size_t)i] <
                                evv[(std::size_t)i] * (kFr / 8) * 0.01)
                                continue;
                            const std::size_t ip = (std::size_t)(i - 4);
                            const std::size_t in2 = (std::size_t)(i + 4);
                            const double tp = std::max(tv[ip], 1e-12);
                            if (tv[in2] < tp * 1.6) continue;
                            if (tv[in2] < tv[(std::size_t)i] * 0.25) continue;
                            if (sv[in2] == 0 || sv[in2] == 1) continue;
                            if (tv[in2] < evv[in2] * (kFr / 8) * 0.01) continue;
                            const double r = std::min(
                                std::min(tv[(std::size_t)i] / tp, tv[in2] / (0.7 * tp)),
                                50.0);
                            if (r < 1.5) continue;
                            if (firstI < 0) firstI = i;
                            bestR = std::max(bestR, r);
                        }
                        if (firstI >= 0) {
                            const int center = firstI * kHop + kFr / 2;
                            s_calDeltas.push_back(
                                (float)((double)(int32_t)(start + (uint32_t)center -
                                                          bw) /
                                        sr * 1000.0));
                            s_calRises.push_back((float)bestR);
                        }
                        s_calBeatState[s_calAnalyzed] = firstI >= 0 ? 1 : 2;
                    } else {
                        s_calBeatState[s_calAnalyzed] = 2;
                    }
                    ++s_calAnalyzed;
                    // 마무리: 오프셋 = 시간차 중앙값, 감도 = 여유 배율의 하위 25% × 0.45
                    if (s_calAnalyzed >= kCalBeats) {
                        s_calState = 0;
                        const int det = (int)s_calDeltas.size();
                        if (det < 12) {
                            std::snprintf(s_calMsg, sizeof(s_calMsg),
                                          "실패: %d/16만 감지 — 클릭에 맞춰 3번줄 개방현을 또렷하게 다시",
                                          det);
                        } else {
                            std::vector<float> d(s_calDeltas);
                            std::nth_element(d.begin(), d.begin() + d.size() / 2,
                                             d.end());
                            const float med = d[d.size() / 2];
                            s_judgeOfsMs = std::min(
                                300.0f, std::max(-100.0f, std::round(med)));
                            s_autoOfsMs = 0.0f; // 수동+측정값으로 대체
                            s_deltaHist.clear();
                            std::vector<float> r(s_calRises);
                            std::sort(r.begin(), r.end());
                            const float r25 = r[r.size() / 4];
                            const double rise =
                                std::min(3.6, std::max(1.3, (double)r25 * 0.45));
                            s_sens = std::min(
                                2.0f, std::max(0.5f, (float)(rise / 1.9)));
                            std::snprintf(s_calMsg, sizeof(s_calMsg),
                                          "완료: 오프셋 %+.0fms · 감도 %.1f (감지 %d/16)",
                                          s_judgeOfsMs, s_sens, det);
                        }
                    }
                }
            }
        }
        if (playing && sr > 0.0) {

            // 판정 카운터/표시 공통부
            auto commit = [&](int judge) {
                if (judge == 2)
                    ++s_great;
                else if (judge == 1)
                    ++s_good;
                else
                    ++s_bad;
                s_lastJudge = judge;
                s_lastJudgeAt = ImGui::GetTime();
            };
            // 이 음을 어느 줄에 그릴까: 가져온 악보의 운지 힌트 우선, 없으면 자동 배정
            auto strOf = [&](uint32_t tick, uint8_t note) -> int8_t {
                for (const auto& h : ptr.tabHints)
                    if (h.tick == tick && h.note == note) return (int8_t)h.strIdx;
                int s = 0, f = 0;
                if (assignString((int)note, s, f)) return (int8_t)s;
                return -1;
            };
            auto pushNote = [&](uint32_t tick, uint8_t note, int8_t strIdx, uint8_t st) {
                s_hits.push_back({tick, note, strIdx, st});
                if (s_hits.size() > 8192) s_hits.erase(s_hits.begin(), s_hits.begin() + 1024);
            };
            // Δ = 타격과 악보 음의 부호 있는 시간차(ms). 최근 Δ의 중앙값이
            // 일관되게 치우쳐 있으면 자동 오프셋에 흡수한다 — 장치 지연을
            // 사용자가 잴 필요 없이 박에 맞춰 몇 번 치면 맞는다.
            auto feedDelta = [&](float ms) {
                s_lastDeltaMs = ms;
                s_haveDelta = true;
                s_deltaHist.push_back(ms);
                if (s_deltaHist.size() > 12) s_deltaHist.erase(s_deltaHist.begin());
                if (s_deltaHist.size() < 4) return;
                std::vector<float> v(s_deltaHist);
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                const float med = v[v.size() / 2];
                // 일관성 확인: 3/4 이상이 중앙값 ±60ms 안이어야 "체계적 지연"
                int close = 0;
                for (float d : s_deltaHist)
                    if (std::fabs(d - med) < 60.0f) ++close;
                if (close * 4 < (int)s_deltaHist.size() * 3 || std::fabs(med) <= 25.0f)
                    return;
                s_autoOfsMs += med;
                s_autoOfsMs = std::min(400.0f, std::max(-150.0f, s_autoOfsMs));
                s_deltaHist.clear();
                // 큰 첫 보정이면 그동안의 판정은 엉터리였다 — 점수만 리셋
                if (std::fabs(med) > 100.0f) {
                    s_great = s_good = s_bad = 0;
                    s_hits.clear();
                    s_lastJudge = -1;
                }
            };

            const uint32_t wp = state.audioClips->inputTapWritePos();
            const double tps = bpm / 60.0 * (double)state.song.ppqn;
            const double ofsSec = state.audioClips->inputLatencySeconds() +
                                  (double)(s_judgeOfsMs + s_autoOfsMs) / 1000.0;
            // 악보 틱 T의 소리가 입력 링에 나타나는 샘플 위치
            auto posOf = [&](uint32_t T) {
                const double behind = ((double)curTick - (double)T) / tps - ofsSec;
                return (uint32_t)((long long)wp - std::llround(behind * sr));
            };
            // noteRiseAt의 내부 상수(프레임 4096, 비교 간격 4096)와 맞춘 여유
            const int kHalf = 2048, kPre = 4096;
            const int wGoodS = (int)(0.140 * sr); // 판정창 ±140ms
            const int wLateS = (int)(0.450 * sr); // 늦은 타격 재탐색 +450ms
            const int tailS = kPre + kHalf;       // 탐색 끝 이후 필요한 데이터

            // 이벤트 활성화: 1차 판정에 필요한 입력이 쌓였으면 활성 목록으로
            while (s_evIdx < pnotes.size()) {
                const uint32_t T = pnotes[s_evIdx].startTick;
                const int avail = (int)(int32_t)(wp - posOf(T));
                if (avail < wGoodS + tailS) break; // 아직 이르다
                std::size_t j = s_evIdx;
                while (j < pnotes.size() && pnotes[j].startTick == T) ++j;
                // 너무 오래된 이벤트(재생 위치 점프 등)는 판정 없이 건너뛴다
                if (avail < (int)(1.5 * sr))
                    s_activeEv.push_back({s_evIdx, j, T, false});
                s_evIdx = j;
            }

            // 활성 이벤트 평가: 구성음마다 noteRiseAt으로 "새로 울리기 시작한
            // 시각"을 찾는다. 1차(±140ms)에서 절반 이상 울렸으면 타이밍으로
            // GREAT/GOOD, 아니면 +450ms까지 넓혀 늦은 타격을 찾고(BAD지만 Δ는
            // 자동 보정에 쓴다) 그래도 없으면 놓침(BAD)이다.
            static std::vector<float> span;
            for (std::size_t ai = 0; ai < s_activeEv.size();) {
                ActiveEv& ev = s_activeEv[ai];
                const uint32_t posT = posOf(ev.tick);
                const int avail = (int)(int32_t)(wp - posT);
                const int scanEndS = ev.late ? wLateS : wGoodS;
                if (avail < scanEndS + tailS) {
                    ++ai;
                    continue;
                }
                const uint32_t start = posT - (uint32_t)(wGoodS + kHalf + kPre);
                const int n = wGoodS + kHalf + kPre + scanEndS + tailS;
                span.resize((std::size_t)n);
                if (!state.audioClips->readInputTapRange(start, span.data(), n)) {
                    // 입력 기록이 이미 덮였다 (창 최소화 등) — 판정 없이 폐기
                    s_activeEv.erase(s_activeEv.begin() + (long)ai);
                    continue;
                }
                // 사실상 무음이면 (연습 입력 없음) 탐색 없이 곧장 처리
                double eSpan = 0.0;
                for (int i = 0; i < n; ++i)
                    eSpan += (double)span[(std::size_t)i] * span[(std::size_t)i];
                const bool silent = eSpan / n < 1e-7;

                const double rise = 1.9 * (double)s_sens;
                std::vector<float> deltas;      // 울린 구성음들의 시간차(ms)
                std::vector<std::size_t> onIdx; // 울린 구성음의 pnotes 인덱스
                if (!silent) {
                    for (std::size_t k = ev.beg; k < ev.end; ++k) {
                        // 같은 코드의 다른 구성음 배음은 이웃 비교에서 제외 —
                        // 예: G코드에서 B2의 3배음(F#4)이 G4 바로 아래 대역에
                        // 정당하게 얹히는데, 이걸 이웃으로 세면 G3이 기각된다.
                        static std::vector<double> exHz;
                        exHz.clear();
                        for (std::size_t j = ev.beg; j < ev.end; ++j) {
                            if (j == k) continue;
                            const double f0 =
                                440.0 *
                                std::pow(2.0, ((double)pnotes[j].note - 69.0) / 12.0);
                            for (int h = 1; h <= 6; ++h) exHz.push_back(f0 * h);
                        }
                        const int off = audio::noteRiseAt(
                            span.data(), n, kPre, sr, (int)pnotes[k].note, rise,
                            exHz.empty() ? nullptr : exHz.data(), (int)exHz.size());
                        if (off < 0) continue;
                        deltas.push_back(
                            (float)((double)(int32_t)(start + (uint32_t)off - posT) /
                                    sr * 1000.0));
                        onIdx.push_back(k);
                    }
                }
                const int total = (int)(ev.end - ev.beg);
                const bool pass = (int)deltas.size() * 2 >= total && !deltas.empty();
                if (!pass && !ev.late) {
                    ev.late = true; // 늦은 타격까지 넓혀 다음 프레임에 다시 본다
                    ++ai;
                    continue;
                }
                float med = 0.0f;
                if (!deltas.empty()) {
                    std::vector<float> ds(deltas);
                    std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end());
                    med = ds[ds.size() / 2];
                    feedDelta(med);
                }
                // 1차 통과만 GREAT/GOOD — late에서 찾은 건 판정창 밖(BAD)
                const int judge =
                    (pass && !ev.late) ? (std::fabs(med) <= 60.0f ? 2 : 1) : 0;
                for (std::size_t k = ev.beg; k < ev.end; ++k) {
                    bool on = false;
                    for (std::size_t oi : onIdx)
                        if (oi == k) on = true;
                    pushNote(ev.tick, pnotes[k].note, strOf(ev.tick, pnotes[k].note),
                             on ? (uint8_t)(judge == 0 ? 1 : judge) : (uint8_t)0);
                }
                commit(judge);
                s_activeEv.erase(s_activeEv.begin() + (long)ai);
            }
        }
    }

    // ── 자동 맞춤 창 ──
    if (s_calOpen) {
        ImGui::SetNextWindowSize(ImVec2(600, 350), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("자동 맞춤 — 연습 판정 보정", &s_calOpen)) {
            ImGui::TextWrapped(
                "기타 입력의 지연(오프셋)과 인식 문턱(감도)을 자동으로 맞춥니다. "
                "측정을 시작하면 킥 4번(카운트인)이 울린 뒤 클릭이 16번 울립니다 — "
                "아래 악보처럼 클릭 하나마다 3번줄 개방현(G)을 한 번씩 치세요. (♩=100 · 4/4)");
            ImGui::Spacing();
            if (!s_practice)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                                   "연습 모드가 꺼져 있습니다 — 타브 창에서 [연습]을 켜 주세요.");

            // 미니 타브: 4마디 × 4박, 3번줄(G)에 개방현 0. 박마다 결과 색 표시.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float rowH = 13.0f, beatW = 32.0f, left = 30.0f;
                const float top = p0.y + 22.0f;
                const float w = left + beatW * 16.0f + 8.0f;
                // 카운트인 점 4개 (첫 마디 위)
                for (int c = 0; c < 4; ++c) {
                    const float x = p0.x + left + 10.0f + 16.0f * (float)c;
                    const bool onDot = s_calState == 1 && (s_calClicked + 4) > c;
                    dl->AddCircleFilled(ImVec2(x, p0.y + 8.0f), 4.0f,
                                        onDot ? IM_COL32(255, 210, 90, 255)
                                              : IM_COL32(90, 90, 105, 255));
                }
                for (int s = 0; s < 6; ++s) {
                    const float y = top + rowH * (float)s;
                    dl->AddLine(ImVec2(p0.x + left - 14.0f, y), ImVec2(p0.x + w, y),
                                IM_COL32(110, 110, 125, 255), 1.0f);
                    dl->AddText(ImVec2(p0.x + 4.0f, y - 7.0f),
                                IM_COL32(160, 160, 175, 255), kStringLabel[s]);
                }
                for (int b = 0; b <= 4; ++b) {
                    const float x = p0.x + left + beatW * 4.0f * (float)b;
                    dl->AddLine(ImVec2(x, top), ImVec2(x, top + rowH * 5.0f),
                                IM_COL32(140, 140, 155, 255), b == 0 || b == 4 ? 2.0f : 1.0f);
                }
                const float gy = top + rowH * 2.0f; // 3번줄(G)
                for (int k = 0; k < 16; ++k) {
                    const float x = p0.x + left + beatW * (float)k + beatW * 0.5f;
                    ImU32 col = IM_COL32(205, 205, 215, 255);
                    if (s_calBeatState[k] == 1) col = IM_COL32(110, 220, 110, 255);
                    else if (s_calBeatState[k] == 2) col = IM_COL32(255, 95, 85, 255);
                    // 지금 칠 박 강조 (측정 중)
                    if (s_calState == 2 && s_calClicked - 1 == k)
                        dl->AddCircle(ImVec2(x, gy), 9.0f, IM_COL32(255, 220, 100, 255),
                                      0, 2.0f);
                    dl->AddRectFilled(ImVec2(x - 5.0f, gy - 8.0f), ImVec2(x + 5.0f, gy + 8.0f),
                                      IM_COL32(28, 28, 34, 255), 2.0f);
                    dl->AddText(ImVec2(x - 3.5f, gy - 7.0f), col, "0");
                }
                ImGui::Dummy(ImVec2(w, 22.0f + rowH * 5.0f + 10.0f));
                ImGui::TextDisabled("초록 = 감지됨 · 빨강 = 못 잡음 · 노란 테두리 = 지금 칠 박");
            }
            ImGui::Spacing();

            if (s_calState == 0) {
                if (ImGui::Button("측정 시작", ImVec2(120, 0)) && s_practice) {
                    stopTransport(state); // 재생 중이었다면 멈추고 시작
                    s_calState = 1;
                    s_calClicked = -4; // 카운트인 4클릭부터
                    s_calT0 = ImGui::GetTime() + 4.0 * 0.6 + 0.25;
                    s_calAnalyzed = 0;
                    s_calDeltas.clear();
                    s_calRises.clear();
                    for (int k = 0; k < 16; ++k) s_calBeatState[k] = 0;
                    s_calMsg[0] = '\0';
                }
                ImGui::SameLine();
                if (ImGui::Button("닫기##calw")) s_calOpen = false;
            } else {
                if (s_calState == 1)
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                                       "카운트인 — 킥 4번 듣고 준비하세요");
                else
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                                       "클릭에 맞춰 치세요!  %d / 16",
                                       std::min(16, std::max(0, s_calClicked)));
                ImGui::SameLine();
                if (ImGui::Button("중단##calw")) s_calState = 0;
            }
            if (s_calMsg[0]) ImGui::TextWrapped("%s", s_calMsg);

            ImGui::Separator();
            ImGui::TextDisabled("수동 조절 (자동 맞춤 결과가 여기에 반영됩니다)");
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("감도##calw", &s_sens, 0.5f, 2.0f, "%.1f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(170);
            ImGui::SliderFloat("오프셋##calw", &s_judgeOfsMs, -100.0f, 300.0f, "%.0fms");
        }
        ImGui::End();
    }

    // ── 타브 가져오기 창 ──
    if (s_importOpen) {
        ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("타브 가져오기", &s_importOpen)) {
            static std::vector<char> s_buf(262144, '\0'); // PDF 한 곡이 들어갈 만큼
            static int s_stepDiv = 1;                     // 0=8분, 1=16분, 2=32분
            static std::vector<pdf::TabPdfPart> s_pdfParts; // PDF에서 뽑은 파트들
            static int s_pdfPart = 0;
            static bool s_pdfRhythm = false;  // PDF에서 리듬(기둥/빔)까지 읽었는가
            static std::string s_pdfKey;      // 그룹 키 = 악보 파일 이름 (재사용 판별)
            // 직전에 가져온 구간 — 다시 가져오면 그 구간을 지우고 덮어쓴다
            static int s_lastImportTrack = -1;
            static uint32_t s_lastImportStart = 0, s_lastImportEnd = 0;

            auto setBuf = [&](const std::string& s) {
                const std::size_t n = std::min(s.size(), s_buf.size() - 1);
                std::memcpy(s_buf.data(), s.data(), n);
                s_buf[n] = '\0';
            };

            // PDF 열기
            if (ImGui::Button("PDF 열기...")) {
                const std::string path = openPdfDialog();
                if (!path.empty()) {
                    const auto r =
                        pdf::extractTabFromPdf(path, (uint32_t)state.song.ppqn);
                    if (!r.error.empty() || r.parts.empty()) {
                        s_pdfParts.clear();
                        s_pdfRhythm = false;
                        state.statusMessage =
                            "PDF 타브 가져오기 실패: " +
                            (r.error.empty() ? std::string("타브를 찾지 못했습니다") : r.error);
                    } else {
                        s_pdfParts = r.parts;
                        s_pdfPart = 0;
                        s_pdfRhythm = r.hasRhythm;
                        // 그룹 키 = 파일 이름 (폴더·확장자 제거). 같은 악보 재가져오기 판별용.
                        {
                            std::string k = path;
                            const std::size_t slash = k.find_last_of("/\\");
                            if (slash != std::string::npos) k = k.substr(slash + 1);
                            const std::size_t dot = k.find_last_of('.');
                            if (dot != std::string::npos && dot > 0) k = k.substr(0, dot);
                            if (k.size() > 60) k = k.substr(0, 60);
                            s_pdfKey = k;
                        }
                        setBuf(s_pdfParts[0].ascii);
                        s_stepDiv = 2; // 리듬을 못 읽었을 때의 기본 (가장 촘촘한 열 = 16분)

                        // 악보의 박자표를 곡에 반영한다 (마디 그리드·메트로놈이 따라간다)
                        const char* sigName = "";
                        if (r.timeSigNum == 3 && r.timeSigDen == 4) {
                            state.metroSigIndex = 1;
                            sigName = " · 3/4";
                        } else if (r.timeSigNum == 6 && r.timeSigDen == 8) {
                            state.metroSigIndex = 2;
                            sigName = " · 6/8";
                        } else if (r.timeSigNum == 4 && r.timeSigDen == 4) {
                            state.metroSigIndex = 0;
                            sigName = " · 4/4";
                        }
                        // 악보의 템포 표기(♩=164)를 곡 BPM에 반영
                        char tempoMsg[32] = "";
                        if (r.tempoBpm > 0) {
                            state.song.bpm = (double)r.tempoBpm;
                            std::snprintf(tempoMsg, sizeof(tempoMsg), " · ♩=%d", r.tempoBpm);
                        }
                        applyTransportState(state);

                        char rep[96] = "";
                        if (r.repeats > 0 || r.measureRepeats > 0)
                            std::snprintf(rep, sizeof(rep), " · 도돌이표 %d곳 / 마디반복 %d곳 펼침",
                                          r.repeats, r.measureRepeats);
                        char msg[360];
                        std::snprintf(msg, sizeof(msg), "PDF: 타브 보표 %d개 / 파트 %d개%s%s%s%s",
                                      r.tabStaves, (int)r.parts.size(), sigName, tempoMsg, rep,
                                      r.hasRhythm ? " — 리듬(음표)까지 읽었습니다"
                                                  : " — 리듬 표기가 없어 칸 길이로 넣습니다");
                        state.statusMessage = msg;
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("악보 프로그램으로 만든(글자가 들어 있는) PDF에서\n"
                                  "6줄 타브와 음표(기둥·빔·부점)를 읽어 옵니다.\n"
                                  "스캔·사진 악보는 지원하지 않습니다.");

            // 파트가 여럿이면 (기타 1/2 등) 미리보기용으로 골라 본다.
            // 가져올 때는 모든 파트를 한 트랙에 합친다.
            if (s_pdfParts.size() > 1) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150);
                std::string names;
                for (std::size_t i = 0; i < s_pdfParts.size(); ++i)
                    names += "파트 " + std::to_string(i + 1) + " 미리보기" + '\0';
                names += '\0';
                if (ImGui::Combo("##pdfpart", &s_pdfPart, names.c_str()))
                    setBuf(s_pdfParts[(std::size_t)s_pdfPart].ascii);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("아래 칸에 보여줄 파트를 고릅니다.\n"
                                      "가져오기를 누르면 파트마다 트랙을 따로 만들어 넣습니다.");
            }

            ImGui::SameLine();
            ImGui::TextDisabled("또는 텍스트 타브를 붙여넣기 (Ctrl+V)");

            // PDF에서 리듬을 읽었으면 그 리듬을 그대로 쓴다 (칸 길이 선택은 필요 없다)
            bool usePdfRhythm = false;
            if (s_pdfRhythm)
                for (const auto& p : s_pdfParts)
                    if (!p.notes.empty()) usePdfRhythm = true;
            if (usePdfRhythm) {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                                   "악보의 음표(리듬)를 읽었습니다 — 길이·박자 그대로 넣습니다%s",
                                   s_pdfParts.size() > 1 ? " · 파트마다 트랙을 따로 만듭니다" : "");
            } else {
                ImGui::SetNextItemWidth(120);
                const char* kSteps[3] = {"한 칸 = 8분", "한 칸 = 16분", "한 칸 = 32분"};
                ImGui::Combo("##tabstep", &s_stepDiv, kSteps, 3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("텍스트 타브에는 리듬 정보가 없어서\n"
                                      "문자 한 칸을 몇 분음표로 볼지 정합니다");
            }
            ImGui::SameLine();
            const bool canImport = s_buf[0] != '\0';
            ImGui::BeginDisabled(!canImport);
            if (ImGui::Button("가져오기 (재생 위치부터)")) {
                // 파트별로 노트 목록을 만든다. 기타 1·2는 동시에 연주되는 다른 파트이므로
                // 한 트랙에 합치면 안 된다 (같은 음이 겹쳐 소리가 커지고 편집도 못 한다).
                std::vector<std::vector<seq::TabNote>> partNotes;
                if (usePdfRhythm) {
                    for (const auto& p : s_pdfParts) {
                        std::vector<seq::TabNote> v;
                        for (const auto& n : p.notes)
                            v.push_back({n.tick, n.note, n.durTicks, n.strIdx, n.vel, n.artic});
                        if (!v.empty()) partNotes.push_back(std::move(v));
                    }
                } else {
                    const uint32_t step = (uint32_t)state.song.ppqn /
                                          (s_stepDiv == 0 ? 2u : (s_stepDiv == 1 ? 4u : 8u));
                    auto v = seq::parseAsciiTab(s_buf.data(), std::max(1u, step));
                    if (!v.empty()) partNotes.push_back(std::move(v));
                }

                if (partNotes.empty()) {
                    state.statusMessage =
                        "타브를 찾지 못했습니다 — 6줄 텍스트 타브인지 확인하세요";
                } else {
                    state.snapshot();
                    const uint32_t tpbI = songTicksPerBar(state);
                    const uint32_t base = state.playPosTick / tpbI * tpbI; // 현재 마디 시작부터
                    const int firstTrack = state.practiceTrack;

                    // 트랙 배정: 같은 악보(그룹 키)의 같은 파트가 이미 있으면 그 트랙을
                    // 재사용한다 — 꼬리표가 프로젝트에 저장되므로 앱을 껐다 켜도,
                    // 다른 트랙을 선택하고 있어도 트랙이 늘어나지 않는다.
                    // (트랙을 추가하면 tracks 벡터가 재할당되므로, 이 아래로는 참조가 아닌
                    //  인덱스로만 접근하고 이 프레임은 바로 끝낸다.)
                    const std::string groupKey = usePdfRhythm ? s_pdfKey : std::string();
                    auto findTagged = [&](std::size_t part) -> int {
                        if (groupKey.empty()) return -1;
                        for (std::size_t i = 0; i < state.song.tracks.size(); ++i)
                            if (state.song.tracks[i].practice && // 연습 트랙 안에서만
                                state.song.tracks[i].importKey == groupKey &&
                                state.song.tracks[i].importPart == (int)part)
                                return (int)i;
                        return -1;
                    };
                    std::vector<int> targets;
                    for (std::size_t p = 0; p < partNotes.size(); ++p) {
                        int idx = findTagged(p);
                        if (idx < 0) {
                            if (p == 0) {
                                idx = firstTrack; // 파트 1은 지금 보고 있는 트랙에
                            } else {
                                addPracticeTrack(state); // 파트마다 연습 트랙을 늘린다
                                idx = (int)state.song.tracks.size() - 1;
                            }
                        }
                        targets.push_back(idx);
                    }
                    // 꼬리표 + 그룹 이름 ("곡명 · 기타 N") — 트랙 목록에서 한 묶음으로 보인다
                    for (std::size_t p = 0; p < targets.size(); ++p) {
                        auto& tr = state.song.tracks[(std::size_t)targets[p]];
                        if (!groupKey.empty()) {
                            tr.importKey = groupKey;
                            tr.importPart = (int)p;
                            tr.name = targets.size() > 1
                                          ? groupKey + " · 기타 " + std::to_string(p + 1)
                                          : groupKey;
                        }
                    }

                    std::size_t total = 0;
                    bool replaced = false;
                    uint32_t endAll = base;
                    for (std::size_t p = 0; p < partNotes.size(); ++p) {
                        auto& tr = state.song.tracks[(std::size_t)targets[p]];
                        std::vector<std::pair<uint32_t, uint32_t>> dur; // (시작틱, 길이)
                        uint32_t endTick = base;
                        for (const auto& tn : partNotes[p]) {
                            // 소리 길이 = 리듬 길이 × 아티큘레이션 (뮤트=짧게, 레가토=꽉 채움)
                            const uint32_t d =
                                std::max(1u, (uint32_t)((uint64_t)tn.durTicks * tn.artic / 100));
                            dur.push_back({base + tn.tick, d});
                            endTick = std::max(endTick, base + tn.tick + d);
                        }
                        // 겹쳐 쌓이지 않도록 이번에 채울 구간과 직전 가져오기 구간을 먼저 비운다
                        auto eraseHints = [&](uint32_t a, uint32_t b) {
                            tr.tabHints.erase(
                                std::remove_if(tr.tabHints.begin(), tr.tabHints.end(),
                                               [&](const seq::Track::TabHint& h) {
                                                   return h.tick >= a && h.tick < b;
                                               }),
                                tr.tabHints.end());
                        };
                        if (endTick > base) {
                            for (const auto& n : seq::extractNotes(tr))
                                if (n.startTick >= base && n.startTick < endTick) replaced = true;
                            seq::eraseMidiRange(tr, base, endTick);
                            eraseHints(base, endTick);
                        }
                        if (s_lastImportTrack == targets[0] &&
                            s_lastImportEnd > s_lastImportStart) {
                            seq::eraseMidiRange(tr, s_lastImportStart, s_lastImportEnd);
                            eraseHints(s_lastImportStart, s_lastImportEnd);
                            replaced = true;
                        }
                        for (std::size_t k = 0; k < partNotes[p].size(); ++k) {
                            tr.addNote(dur[k].first, dur[k].second, partNotes[p][k].note,
                                       partNotes[p][k].vel);
                            seq::adoptNoteIntoClips(tr, partNotes[p][k].note, dur[k].first);
                            // 악보의 운지(줄)를 힌트로 저장 -> 타브 창이 악보 그대로 표시
                            if (partNotes[p][k].strIdx >= 0 && partNotes[p][k].strIdx < 6)
                                tr.tabHints.push_back({dur[k].first, partNotes[p][k].note,
                                                       (uint8_t)partNotes[p][k].strIdx});
                        }
                        tr.sortEvents();
                        total += partNotes[p].size();
                        endAll = std::max(endAll, endTick);
                    }

                    state.practiceTrack = targets[0];
                    // 파트가 여럿이면 연습 창에 두 기타를 나란히 띄운다
                    if (targets.size() > 1) state.tabTracks = targets;
                    s_lastImportTrack = targets[0];
                    s_lastImportStart = base;
                    s_lastImportEnd = endAll;

                    rebuildAudioMix(state);
                    refreshPlaybackIfPlaying(state);
                    state.statusMessage =
                        std::string(replaced ? "타브 다시 가져오기(이 구간 기존 노트 지움): "
                                             : "타브 가져오기: ") +
                        "노트 " + std::to_string(total) + "개" +
                        (partNotes.size() > 1
                             ? " · 파트 " + std::to_string(partNotes.size()) + "개를 트랙 " +
                                   std::to_string(partNotes.size()) + "개에 나눠 넣음"
                             : "") +
                        (usePdfRhythm ? " (악보 리듬 그대로)" : "");
                    s_buf[0] = '\0';
                    s_pdfParts.clear();
                    s_pdfRhythm = false;
                    s_importOpen = false;

                    // 트랙이 늘어나 tracks 벡터가 재할당됐을 수 있다 -> 이 프레임은 여기서 끝낸다
                    ImGui::EndDisabled();
                    ImGui::End();      // 가져오기 창
                    ImGui::EndGroup(); // 오른쪽 영역 (연습 창 안에서 열려 있다)
                    ImGui::End();      // 기타 연습 창
                    return;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("지우기##tabbuf")) {
                s_buf[0] = '\0';
                s_pdfParts.clear();
                s_pdfRhythm = false;
            }
            ImGui::InputTextMultiline("##tabtext", s_buf.data(), s_buf.size(),
                                      ImVec2(-1.0f, -1.0f));
        }
        ImGui::End();
    }

    // 휠 = 확대, Ctrl+휠 = 가로 스크롤 (다른 뷰와 같은 체계)
    static float s_tabScrollX = 0.0f;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl)
                s_tabScrollX += -wheel * 160.0f;
            else
                state.tabZoom = std::clamp(state.tabZoom * std::pow(1.15f, wheel), 0.02f, 0.5f);
            ImGui::GetIO().MouseWheel = 0.0f;
        }
    }

    const float zoom = state.tabZoom;
    const uint32_t tpb = songTicksPerBar(state);
    const uint32_t songLen = state.timelineBars * tpb;
    // 보표 하나 = 트랙 이름 줄 + 6줄. 여러 트랙이면 아래로 쌓는다 (악보의 단처럼).
    const float kNameH = 18.0f;
    const float staffH = kNameH + kStringGap * 6.0f;
    // 연주 판정 레인: 6줄 미니 타브 — 코드 구성음이 줄별로 울림/빠짐 색으로 보인다
    const float kPracRow = 13.0f;
    const float laneH = s_practice ? (kNameH + kPracRow * 6.0f + 4.0f) : 0.0f;
    // 클립 레인: 위쪽에 트랙별 오디오 클립(반주·녹음)을 파형으로 — 파형을 보며
    // 드래그해 악보와 맞춘다. 클립이 하나도 없으면 자리를 차지하지 않는다.
    bool anyClips = false;
    for (int idx : shown)
        if (idx >= 0 && !state.song.tracks[(std::size_t)idx].clips.empty()) anyClips = true;
    const float kClipLaneH = 54.0f;
    const float clipsH = anyClips ? kClipLaneH * (float)shown.size() : 0.0f;
    const float bodyH = 24.0f + clipsH + staffH * (float)shown.size() + laneH; // 24 = 눈금자
    const float timelineW = (float)songLen * zoom + 40.0f;
    auto clipTop = [&](std::size_t k, float y0) { return y0 + 24.0f + kClipLaneH * (float)k; };
    auto staffTop = [&](std::size_t k, float y0) {
        return y0 + 24.0f + clipsH + staffH * (float)k + kNameH;
    };

    // ── 왼쪽 줄 라벨 (고정) ──
    ImGui::BeginChild("##tablabels", ImVec2(kLabelW, bodyH + 16.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const ImVec2 lp0 = ImGui::GetCursorScreenPos();
        if (anyClips) // 클립 레인 라벨 (반주·녹음 파형이 있는 줄)
            for (std::size_t k = 0; k < shown.size(); ++k)
                ldl->AddText(ImVec2(lp0.x + 2.0f, clipTop(k, lp0.y) + 4.0f),
                             IM_COL32(150, 150, 165, 255), "오디오");
        for (std::size_t k = 0; k < shown.size(); ++k) {
            const float gt = staffTop(k, lp0.y);
            for (int s = 0; s < 6; ++s)
                ldl->AddText(ImVec2(lp0.x + 10.0f,
                                    gt + kStringGap * (float)s + kStringGap * 0.5f - 8.0f),
                             IM_COL32(190, 190, 205, 255), kStringLabel[s]);
        }
        if (s_practice) // 맨 아래 = 내가 친 음
            ldl->AddText(ImVec2(lp0.x + 2.0f, staffTop(shown.size(), lp0.y) + 2.0f),
                         IM_COL32(255, 210, 120, 255), "연주");
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);

    // ── 타임라인 (가로 스크롤) ──
    ImGui::BeginChild("##tabtimeline", ImVec2(0.0f, bodyH + 16.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (s_tabScrollX != 0.0f) {
            ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + s_tabScrollX));
            s_tabScrollX = 0.0f;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tabcanvas", ImVec2(timelineW, bodyH));

        // 눈금자 + 마디선 (마디선은 모든 보표를 관통)
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + timelineW, p0.y + 24.0f),
                          IM_COL32(30, 30, 34, 255));
        // 루프 구간: 반복 중인 마디를 한눈에 (눈금자에 띠 + 본문 옅은 음영)
        if (state.loopEnabled && state.loopEndTick > state.loopStartTick) {
            const float lx0 = p0.x + (float)state.loopStartTick * zoom;
            const float lx1 = p0.x + (float)state.loopEndTick * zoom;
            dl->AddRectFilled(ImVec2(lx0, p0.y + 24.0f), ImVec2(lx1, p0.y + bodyH),
                              IM_COL32(90, 150, 230, 26));
            dl->AddRectFilled(ImVec2(lx0, p0.y), ImVec2(lx1, p0.y + 5.0f),
                              IM_COL32(90, 160, 245, 220));
            dl->AddLine(ImVec2(lx0, p0.y), ImVec2(lx0, p0.y + bodyH),
                        IM_COL32(90, 160, 245, 150));
            dl->AddLine(ImVec2(lx1, p0.y), ImVec2(lx1, p0.y + bodyH),
                        IM_COL32(90, 160, 245, 150));
        }
        for (uint32_t t = 0; t <= songLen; t += tpb) {
            const float x = p0.x + (float)t * zoom;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + bodyH), IM_COL32(70, 70, 82, 160));
            char bn[16];
            std::snprintf(bn, sizeof(bn), "%u", t / tpb + 1);
            dl->AddText(ImVec2(x + 4.0f, p0.y + 4.0f), IM_COL32(150, 150, 165, 255), bn);
        }

        // ── 클립 레인: 오디오 클립(반주·녹음)을 파형으로 + 드래그로 위치 맞추기 ──
        // 반주 시작을 악보에 맞추는 게 목적이라 격자에 스냅하지 않는다 (Shift=마디 스냅).
        if (anyClips) {
            static const audio::AudioClip* s_dragClip = nullptr;
            static float s_dragGrabX = 0.0f; // 클립 시작과 마우스의 간격(px)
            for (std::size_t k = 0; k < shown.size(); ++k) {
                auto& tr = state.song.tracks[(std::size_t)shown[k]];
                const float ly = clipTop(k, p0.y);
                dl->AddRectFilled(ImVec2(p0.x, ly + 1.0f),
                                  ImVec2(p0.x + timelineW, ly + kClipLaneH - 1.0f),
                                  IM_COL32(22, 22, 27, 255));
                for (auto& cp : tr.clips) {
                    if (!cp) continue;
                    const bool isBacking = (cp == state.practiceBackingClip);
                    drawClipBlock(dl, *cp, p0.x, ly + 1.0f, kClipLaneH - 2.0f, zoom,
                                  state.song, isBacking);
                    drawWaveform(dl, *cp, p0.x, ly + 1.0f, kClipLaneH - 2.0f, zoom,
                                 state.song,
                                 isBacking ? IM_COL32(150, 200, 255, 220)
                                           : IM_COL32(170, 170, 190, 200));
                    // 드래그: 클립 몸통을 잡아 좌우로 옮긴다 (= 반주 시작 맞추기)
                    const float cx0 = p0.x + (float)cp->startTick * zoom;
                    const double durTk = cp->durationSeconds() * state.song.bpm / 60.0 *
                                         (double)state.song.ppqn;
                    const float cx1 = cx0 + (float)durTk * zoom;
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    const bool overThis = mp.x >= cx0 && mp.x <= cx1 && mp.y >= ly &&
                                          mp.y <= ly + kClipLaneH;
                    if (overThis && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                        ImGui::IsWindowHovered()) {
                        s_dragClip = cp.get();
                        s_dragGrabX = mp.x - cx0;
                        state.snapshot();
                    }
                    if (s_dragClip == cp.get()) {
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            float nx = mp.x - s_dragGrabX - p0.x;
                            uint32_t nt = (uint32_t)std::max(0.0f, nx / zoom);
                            if (ImGui::GetIO().KeyShift) nt = (nt + tpb / 2) / tpb * tpb;
                            cp->startTick = nt;
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        } else {
                            s_dragClip = nullptr;
                            rebuildAudioMix(state);
                            refreshPlaybackIfPlaying(state);
                        }
                    }
                    if (overThis && ImGui::IsWindowHovered())
                        ImGui::SetTooltip("%s — 드래그로 위치 맞추기 (Shift: 마디 스냅)\n"
                                          "시작 %.3fs",
                                          cp->name.c_str(),
                                          (double)cp->startTick * 60.0 /
                                              (state.song.bpm * (double)state.song.ppqn));
                }
                dl->AddLine(ImVec2(p0.x, ly + kClipLaneH - 1.0f),
                            ImVec2(p0.x + timelineW, ly + kClipLaneH - 1.0f),
                            IM_COL32(60, 60, 72, 255));
            }
        }

        for (std::size_t k = 0; k < shown.size(); ++k) {
            auto& tr = state.song.tracks[(std::size_t)shown[k]];
            const float gridTop = staffTop(k, p0.y);
            // 트랙 이름 (보표 위, 스크롤을 따라오도록 화면 왼쪽에 고정)
            {
                const float nx = std::max(p0.x + 4.0f, dl->GetClipRectMin().x + 4.0f);
                const bool drum = (tr.channel & 0x0F) == 9;
                const ImU32 nameCol = drum ? IM_COL32(255, 163, 64, 255)
                                           : (tr.isGuitar ? IM_COL32(115, 217, 115, 255)
                                                          : IM_COL32(190, 190, 205, 255));
                dl->AddText(ImVec2(nx, gridTop - kNameH + 2.0f), nameCol, tr.name.c_str());
                if (shown[k] == pracTarget) { // 이 보표가 연습 판정 대상
                    const float nw = ImGui::CalcTextSize(tr.name.c_str()).x;
                    dl->AddText(ImVec2(nx + nw + 8.0f, gridTop - kNameH + 2.0f),
                                IM_COL32(255, 210, 120, 255), "◀ 연습 중");
                }
            }
            // 6줄
            for (int s = 0; s < 6; ++s) {
                const float y = gridTop + kStringGap * (float)s + kStringGap * 0.5f;
                dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + timelineW, y),
                            IM_COL32(120, 120, 135, 200), 1.0f);
            }

            // 악보에서 가져온 운지 힌트 (같은 음도 줄이 다르면 다른 운지다)
            std::unordered_map<uint64_t, int> hintMap;
            hintMap.reserve(tr.tabHints.size());
            for (const auto& h : tr.tabHints)
                hintMap[((uint64_t)h.tick << 8) | h.note] = (int)h.strIdx;

            // 노트 -> 프렛 숫자 (겹침 방지용 어두운 배경 박스 + 숫자)
            const auto& notes = cachedNotes(tr, shown[k]);
            for (const auto& n : notes) {
                const float x = p0.x + (float)n.startTick * zoom;
                if (x < dl->GetClipRectMin().x - 30.0f || x > dl->GetClipRectMax().x + 30.0f)
                    continue;
                int s = 0, fret = 0;
                char buf[8];
                ImU32 col = IM_COL32(255, 205, 120, 255);
                bool assigned = false;
                {
                    const auto it = hintMap.find(((uint64_t)n.startTick << 8) | n.note);
                    if (it != hintMap.end() && it->second >= 0 && it->second < 6) {
                        const int f = (int)n.note - kOpen[it->second];
                        if (f >= 0 && f <= kMaxFret) {
                            s = it->second;
                            fret = f;
                            assigned = true;
                        }
                    }
                }
                if (assigned) {
                    std::snprintf(buf, sizeof(buf), "%d", fret);
                } else if (!assignString(n.note, s, fret)) {
                    // 기타 음역 밖: 가장 가까운 줄에 "?"로 표시
                    s = n.note < kOpen[5] ? 5 : 0;
                    std::snprintf(buf, sizeof(buf), "?");
                    col = IM_COL32(255, 120, 110, 255);
                } else {
                    std::snprintf(buf, sizeof(buf), "%d", fret);
                }
                const float y = gridTop + kStringGap * (float)s + kStringGap * 0.5f;
                const ImVec2 ts = ImGui::CalcTextSize(buf);
                dl->AddRectFilled(ImVec2(x - 2.0f, y - ts.y * 0.5f - 1.0f),
                                  ImVec2(x + ts.x + 2.0f, y + ts.y * 0.5f + 1.0f),
                                  IM_COL32(24, 24, 30, 235), 2.0f);
                dl->AddText(ImVec2(x, y - ts.y * 0.5f), col, buf);
            }
        }

        // ── 연습: 맨 아래 '연주' 미니 타브 — 코드 구성음이 줄별로 보인다 ──
        if (s_practice) {
            const float laneTop = staffTop(shown.size(), p0.y) + 2.0f;
            for (int s = 0; s < 6; ++s) {
                const float y = laneTop + kPracRow * (float)s + kPracRow * 0.5f;
                dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + timelineW, y),
                            IM_COL32(80, 80, 95, 150), 1.0f);
            }
            static const ImU32 kSCol[3] = {IM_COL32(255, 90, 80, 255),   // 빨강: 틀림/안 울림
                                           IM_COL32(255, 165, 60, 255),  // 주황: GOOD
                                           IM_COL32(110, 220, 110, 255)}; // 초록: GREAT
            for (const auto& h : s_hits) {
                const float x = p0.x + (float)h.tick * zoom;
                if (x < dl->GetClipRectMin().x - 30.0f || x > dl->GetClipRectMax().x + 30.0f)
                    continue;
                char pb[8];
                int row = h.strIdx;
                float y;
                if (h.note > 0 && row >= 0 && row < 6 && (int)h.note - kOpen[row] >= 0 &&
                    (int)h.note - kOpen[row] <= kMaxFret) {
                    std::snprintf(pb, sizeof(pb), "%d", (int)h.note - kOpen[row]);
                    y = laneTop + kPracRow * (float)row + kPracRow * 0.5f;
                } else if (h.note > 0) {
                    int s = 0, f = 0;
                    row = assignString(h.note, s, f) ? s : 5;
                    std::snprintf(pb, sizeof(pb), "%d", f);
                    y = laneTop + kPracRow * (float)row + kPracRow * 0.5f;
                } else {
                    // 음정 불명/후보 없음 — 특정 줄이 아니므로 레인 한가운데에
                    std::snprintf(pb, sizeof(pb), "x");
                    y = laneTop + kPracRow * 3.0f;
                }
                const ImVec2 ts = ImGui::CalcTextSize(pb);
                dl->AddRectFilled(ImVec2(x - 2.0f, y - ts.y * 0.5f - 1.0f),
                                  ImVec2(x + ts.x + 2.0f, y + ts.y * 0.5f + 1.0f),
                                  IM_COL32(24, 24, 30, 235), 2.0f);
                dl->AddText(ImVec2(x, y - ts.y * 0.5f), kSCol[h.state], pb);
            }
        }

        // 재생 헤드 + 클릭 = 그 위치로 이동.
        // 클립 레인 안의 클릭은 클립 드래그용이므로 재생 위치를 옮기지 않는다.
        const float hx = p0.x + (float)state.playPosTick * zoom;
        dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + bodyH), IM_COL32(255, 90, 90, 220),
                    1.5f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool inClipLane =
                anyClips && mp.y >= p0.y + 24.0f && mp.y < p0.y + 24.0f + clipsH;
            if (!inClipLane) {
                const uint32_t t = mp.x > p0.x ? (uint32_t)((mp.x - p0.x) / zoom) : 0;
                seekTo(state, t, /*scrollView=*/false);
            }
        }
        // 재생 따라가기 (다른 편집기와 동일)
        const bool playing = state.player && state.player->isPlaying();
        if ((playing && state.followPlayhead) || state.scrollToPlayhead) {
            const float target =
                (float)state.playPosTick * zoom - ImGui::GetWindowSize().x * 0.4f;
            ImGui::SetScrollX(std::max(0.0f, target));
        }
    }
    ImGui::EndChild();
    ImGui::EndGroup(); // 오른쪽(툴바 + 타브) 끝 — 왼쪽 트랙 패널과 나란히
    ImGui::End();
}

} // namespace midipro::gui
