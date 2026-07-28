// =============================================================
// MidiPro - gui/PanelsDrums.cpp
// 드럼 트랙 에디터: 피아노 롤과 같은 타임라인이지만 세로축이 음정이
// 아니라 드럼 소리(위=크래시 ... 아래=킥)다. 격자에 스냅해 클릭/드래그로
// 노트를 깔고, 우클릭으로 지운다.
//
// 데이터는 드럼 트랙(채널 10)의 일반 노트 이벤트 그대로다 — 피아노 롤과
// 완전히 호환된다.
// =============================================================

#include "gui/DrumClassify.h"
#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "audio/WavFile.h"
#include "core/PathUtf8.h"
#include "midi/MidiConstants.h"
#include "sequencer/TimeBase.h"
#include "sequencer/DrumPattern.h"
#include "sequencer/Track.h"

#include "imgui.h"
#include "imgui_internal.h" // FindWindowByName (피아노 롤 옆에 도킹)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace midipro::gui {

namespace {
// GM 드럼 맵에서 자주 쓰는 소리들 (위=심벌, 아래=킥)
struct DrumRow {
    uint8_t note;
    const char* name;
};
constexpr DrumRow kRows[] = {
    {49, "크래시"},   {51, "라이드"},  {46, "오픈 햇"},  {42, "클로즈드 햇"},
    {50, "하이 탐"},  {47, "미드 탐"}, {45, "로우 탐"},  {39, "클랩"},
    {38, "스네어"},   {36, "킥"},
};
constexpr int kNumRows = (int)(sizeof(kRows) / sizeof(kRows[0]));
constexpr float kRowH = 24.0f;
constexpr float kLabelW = 100.0f;

int rowForNote(uint8_t note) {
    for (int r = 0; r < kNumRows; ++r)
        if (kRows[r].note == note) return r;
    return -1;
}

// 드럼 샘플 라이브러리 기본 위치 (실행 폴더 기준)
constexpr const char* kDrumLibDir = "src/Drum";

// ---- 악기별 분류 ----
// 규칙 자체는 gui/DrumClassify.h 에 있다 (GUI 없이 단위 테스트하려고 뺐다).
constexpr const char* kBucketNames[9] = {"킥",     "스네어", "클랩",   "클로즈드 햇",
                                         "오픈 햇", "탐",     "크래시", "라이드",
                                         "기타/퍼커션"};
// 드럼 행 -> 기본 버킷 인덱스 (kRows 순서와 1:1)
constexpr int kRowBucket[kNumRows] = {6, 7, 4, 3, 5, 5, 5, 2, 1, 0};

// 라이브러리 전체 스캔 캐시 (처음 열 때 한 번)
struct DrumLibEntry {
    std::string display; // "머신 / 파일" 표시용
    std::string full;    // 배정용 전체 경로 (UTF-8)
    uint16_t mask;
};
std::vector<DrumLibEntry> g_drumLib;
bool g_drumLibScanned = false;

void scanDrumLib(const std::string& rootUtf8) {
    namespace fs = std::filesystem;
    g_drumLib.clear();
    g_drumLibScanned = true;
    std::error_code ec;
    const fs::path root = core::pathFromUtf8(rootUtf8);
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = core::pathToUtf8(it->path().extension());
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".wav") continue;
        const std::string rel =
            core::pathToUtf8(fs::relative(it->path(), root, ec));
        DrumLibEntry e;
        e.full = core::pathToUtf8(it->path());
        e.mask = classifyDrumPath(rel);
        // 표시: 아카이브 폴더는 빼고 "머신 / (하위) / 파일"로 줄인다
        std::string disp = rel;
        for (auto& c : disp) if (c == '\\') c = '/';
        const auto firstSlash = disp.find('/');
        if (firstSlash != std::string::npos) disp = disp.substr(firstSlash + 1);
        e.display = std::move(disp);
        g_drumLib.push_back(std::move(e));
    }
    std::sort(g_drumLib.begin(), g_drumLib.end(),
              [](const DrumLibEntry& a, const DrumLibEntry& b) { return a.display < b.display; });
}
} // namespace

// WAV 파일을 읽어 이 노트의 드럼 샘플로 배정한다 (utf8Path 비면 신스로 복귀).
// 프로젝트 로드 복원에도 쓰여 외부 공개 (PanelsInternal.h).
bool assignDrumSample(AppState& state, int note, const std::string& utf8Path) {
    if (!state.audioClips || note < 0 || note > 127) return false;
    if (utf8Path.empty()) {
        state.audioClips->setDrumSample((uint8_t)note, nullptr);
        state.drumSamplePaths.erase(note);
        return true;
    }
    auto clip = audio::readWavFile(core::pathFromUtf8(utf8Path), "drum");
    if (!clip || clip->frames() == 0) {
        state.statusMessage = "WAV를 읽지 못했습니다: " + utf8Path;
        return false;
    }
    // 원샷이 비정상적으로 길면 10초로 자른다
    const std::size_t maxN = (std::size_t)clip->sampleRate * 10 * (std::size_t)clip->channels;
    if (clip->pcm.size() > maxN) clip->pcm.resize(maxN);
    state.audioClips->setDrumSample((uint8_t)note, clip);
    state.drumSamplePaths[note] = utf8Path;
    return true;
}

// 드럼 킷(샘플 배정 세트) 저장 폴더: %LOCALAPPDATA%\MidiPro\drumkits
static std::filesystem::path drumKitDir() {
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path p = la ? std::filesystem::path(la) : std::filesystem::path(".");
    return p / "MidiPro" / "drumkits";
}

// 킷 저장/불러오기 팝업 (툴바 "킷" 버튼). 파일 형식: 줄마다 "노트번호 경로".
static void drawDrumKitPopup(AppState& state) {
    if (!ImGui::BeginPopup("drumkit")) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    static char kitName[64] = "";
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##kitname", "킷 이름", kitName, sizeof(kitName));
    ImGui::SameLine();
    const bool canSave = kitName[0] != '\0' && !state.drumSamplePaths.empty();
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("저장##kit")) {
        fs::create_directories(drumKitDir(), ec);
        const fs::path f = drumKitDir() / core::pathFromUtf8(std::string(kitName) + ".kit");
        std::ofstream out(f);
        for (const auto& ds : state.drumSamplePaths) out << ds.first << " " << ds.second << "\n";
        state.statusMessage = std::string("드럼 킷 저장: ") + kitName;
        kitName[0] = '\0';
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !canSave)
        ImGui::SetTooltip("이름을 쓰고, 샘플이 하나 이상 배정돼 있어야 합니다");
    ImGui::Separator();
    ImGui::TextDisabled("저장된 킷 (클릭 = 불러오기)");
    int shown = 0;
    if (fs::exists(drumKitDir(), ec)) {
        int id = 0;
        for (const auto& e : fs::directory_iterator(drumKitDir(), ec)) {
            if (e.path().extension() != L".kit") continue;
            ++shown;
            ImGui::PushID(id++);
            const std::string nm = core::pathToUtf8(e.path().stem());
            if (ImGui::SmallButton("x")) { // 킷 삭제
                fs::remove(e.path(), ec);
                ImGui::PopID();
                continue;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("이 킷 삭제");
            ImGui::SameLine();
            if (ImGui::Selectable(nm.c_str())) {
                // 현재 배정을 전부 이 킷으로 교체
                if (state.audioClips)
                    for (int n = 0; n < 128; ++n)
                        state.audioClips->setDrumSample((uint8_t)n, nullptr);
                state.drumSamplePaths.clear();
                std::ifstream in(e.path());
                std::string line;
                int loaded = 0;
                while (std::getline(in, line)) {
                    std::istringstream ls(line);
                    int note = -1;
                    ls >> note;
                    std::string p;
                    std::getline(ls, p);
                    if (!p.empty() && p[0] == ' ') p.erase(0, 1);
                    if (note >= 0 && note <= 127 && !p.empty() &&
                        assignDrumSample(state, note, p))
                        ++loaded;
                }
                state.statusMessage =
                    "드럼 킷 불러옴: " + nm + " (" + std::to_string(loaded) + "개)";
            }
            ImGui::PopID();
        }
    }
    if (shown == 0) ImGui::TextDisabled("(없음)");
    ImGui::EndPopup();
}

// 라이브러리 루트 결정.
//  1) 실행 폴더의 src/Drum
//  2) 한 단계 위 (exe를 build\에서 바로 실행할 때)
//  3) %LOCALAPPDATA%\MidiPro\Drum — 설치본은 Program Files 아래라 폴더를 넣으려면
//     관리자 권한이 필요하므로, 샘플을 나중에 넣을 수 있는 자리를 하나 더 둔다.
static std::string drumLibRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(kDrumLibDir, ec)) return kDrumLibDir;
    if (fs::exists("../src/Drum", ec)) return "../src/Drum";
    if (const char* la = std::getenv("LOCALAPPDATA")) {
        const std::string user = std::string(la) + "\\MidiPro\\Drum";
        if (fs::exists(user, ec)) return user;
    }
    return kDrumLibDir;
}

// 드럼 샘플 브라우저 창: "악기별"(자동 분류) / "폴더"(직접 탐색) 두 탭.
// WAV 클릭 = 배정 + 바로 미리듣기.
static void drawDrumSampleBrowser(AppState& state) {
    if (state.drumBrowseNote < 0) return;
    namespace fs = std::filesystem;
    const int note = state.drumBrowseNote;
    const int row = rowForNote((uint8_t)note);

    bool open = true;
    char title[128];
    std::snprintf(title, sizeof(title), "드럼 샘플 선택 — %s###drumbrowse",
                  row >= 0 ? kRows[row].name : "드럼");
    ImGui::SetNextWindowSize(ImVec2(460, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin(title, &open);

    // 현재 배정 + 신스 복귀
    const auto it = state.drumSamplePaths.find(note);
    if (it != state.drumSamplePaths.end()) {
        const std::size_t slash = it->second.find_last_of("\\/");
        ImGui::Text("현재: %s",
                    (slash == std::string::npos ? it->second : it->second.substr(slash + 1))
                        .c_str());
    } else {
        ImGui::TextDisabled("현재: 내장 신스 소리");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("신스로 되돌리기")) assignDrumSample(state, note, "");

    // 클릭 = 배정 + 미리듣기 (두 탭 공용)
    const auto pick = [&](const std::string& full) {
        if (assignDrumSample(state, note, full))
            triggerNote(state, 9, (uint8_t)note, 110, 1.0);
    };

    if (ImGui::BeginTabBar("##dsmode")) {
        // ── 악기별: 전체 라이브러리를 킥/스네어/햇...으로 자동 분류 ──
        if (ImGui::BeginTabItem("악기별")) {
            static int s_bucket = 0;
            static int s_lastNote = -1;
            static char s_filter[64] = "";
            if (note != s_lastNote) { // 우클릭한 드럼 줄에 맞는 악기로 시작
                s_lastNote = note;
                if (row >= 0) s_bucket = kRowBucket[row];
                s_filter[0] = '\0';
            }
            if (!g_drumLibScanned) scanDrumLib(drumLibRoot());

            ImGui::SetNextItemWidth(130);
            ImGui::Combo("##bucket", &s_bucket, kBucketNames, 9);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            ImGui::InputTextWithHint("##dsearch", "이름 검색", s_filter, sizeof(s_filter));
            ImGui::SameLine();
            if (ImGui::SmallButton("다시 검색")) scanDrumLib(drumLibRoot());

            // 버킷 + 검색어 필터
            std::string flt = s_filter;
            for (auto& c : flt) c = (char)tolower((unsigned char)c);
            std::vector<int> idx;
            idx.reserve(g_drumLib.size());
            for (int k = 0; k < (int)g_drumLib.size(); ++k) {
                if (!(g_drumLib[(std::size_t)k].mask & (1u << s_bucket))) continue;
                if (!flt.empty()) {
                    std::string d = g_drumLib[(std::size_t)k].display;
                    for (auto& c : d) c = (char)tolower((unsigned char)c);
                    if (d.find(flt) == std::string::npos) continue;
                }
                idx.push_back(k);
            }
            ImGui::TextDisabled("%d개", (int)idx.size());
            ImGui::BeginChild("##dsbybucket", ImVec2(0, 0), ImGuiChildFlags_Borders);
            if (g_drumLib.empty()) {
                ImGui::TextDisabled("라이브러리를 찾지 못했습니다 (src\\Drum)");
            } else {
                ImGuiListClipper clip; // 수천 개도 부드럽게
                clip.Begin((int)idx.size());
                while (clip.Step()) {
                    for (int li = clip.DisplayStart; li < clip.DisplayEnd; ++li) {
                        const auto& e = g_drumLib[(std::size_t)idx[(std::size_t)li]];
                        const bool current = it != state.drumSamplePaths.end() &&
                                             it->second == e.full;
                        ImGui::PushID(idx[(std::size_t)li]);
                        if (ImGui::Selectable(e.display.c_str(), current)) pick(e.full);
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── 폴더: 원래 폴더 구조 그대로 탐색 ──
        if (ImGui::BeginTabItem("폴더")) {
            if (state.drumBrowseDir.empty()) state.drumBrowseDir = drumLibRoot();
            fs::path cur = core::pathFromUtf8(state.drumBrowseDir);
            std::error_code ec;
            if (!fs::exists(cur, ec)) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "폴더가 없습니다: %s",
                                   state.drumBrowseDir.c_str());
                if (ImGui::Button("기본 폴더로")) state.drumBrowseDir = drumLibRoot();
            } else {
                if (ImGui::Button("⬆ 위로")) {
                    const fs::path base = fs::absolute(core::pathFromUtf8(drumLibRoot()), ec);
                    const fs::path curAbs = fs::absolute(cur, ec);
                    if (curAbs != base && curAbs.has_parent_path()) // 라이브러리 밖 금지
                        state.drumBrowseDir = core::pathToUtf8(cur.parent_path());
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", state.drumBrowseDir.c_str());

                ImGui::BeginChild("##drumfiles", ImVec2(0, 0), ImGuiChildFlags_Borders);
                std::vector<fs::path> dirs, wavs;
                for (const auto& e : fs::directory_iterator(cur, ec)) {
                    if (e.is_directory(ec)) {
                        dirs.push_back(e.path());
                    } else {
                        std::string ext = core::pathToUtf8(e.path().extension());
                        for (auto& c : ext) c = (char)tolower((unsigned char)c);
                        if (ext == ".wav") wavs.push_back(e.path());
                    }
                }
                std::sort(dirs.begin(), dirs.end());
                std::sort(wavs.begin(), wavs.end());
                for (const auto& d : dirs) {
                    const std::string nm = "[폴더] " + core::pathToUtf8(d.filename());
                    if (ImGui::Selectable(nm.c_str()))
                        state.drumBrowseDir = core::pathToUtf8(d);
                }
                for (const auto& w : wavs) {
                    const std::string nm = core::pathToUtf8(w.filename());
                    const std::string full = core::pathToUtf8(w);
                    const bool current =
                        it != state.drumSamplePaths.end() && it->second == full;
                    if (ImGui::Selectable(nm.c_str(), current)) pick(full);
                }
                if (dirs.empty() && wavs.empty()) ImGui::TextDisabled("(비어 있음)");
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    if (!open) state.drumBrowseNote = -1;
}

void drawDrums(AppState& state) {
    if (!state.showDrums) return;
    // 세션마다 처음엔 피아노 롤 옆(같은 도크의 탭)에 붙인다. 이후 옮기면 그대로.
    if (ImGuiWindow* pr = ImGui::FindWindowByName("피아노 롤"))
        if (pr->DockId != 0) ImGui::SetNextWindowDockID(pr->DockId, ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("드럼 트랙", &state.showDrums)) {
        ImGui::End();
        return;
    }
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // ── 드럼 트랙 찾기: 선택 트랙이 채널 10이면 그것, 아니면 안내 ──
    int di = -1;
    if (state.selectedTrack >= 0 && state.selectedTrack < (int)state.song.tracks.size() &&
        (state.song.tracks[(std::size_t)state.selectedTrack].channel & 0x0F) == 9)
        di = state.selectedTrack;
    if (di < 0) {
        int firstDrum = -1;
        for (int i = 0; i < (int)state.song.tracks.size(); ++i)
            if ((state.song.tracks[(std::size_t)i].channel & 0x0F) == 9 &&
                !state.song.tracks[(std::size_t)i].practice) {
                firstDrum = i;
                break;
            }
        if (firstDrum >= 0) {
            ImGui::TextDisabled("드럼 트랙(채널 10)을 선택하면 여기서 편집합니다.");
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), "\"%s\" 선택",
                          state.song.tracks[(std::size_t)firstDrum].name.c_str());
            if (ImGui::Button(lbl)) state.selectedTrack = firstDrum;
        } else {
            ImGui::TextDisabled("드럼 트랙이 없습니다.");
            if (ImGui::Button("+ 드럼 트랙 만들기")) addDrumTrack(state);
        }
        ImGui::End();
        return;
    }
    auto& track = state.song.tracks[(std::size_t)di];

    // ── 툴바 ──
    ImGui::Text("드럼 트랙: %s", track.name.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("확대##dz", &state.drumZoom, 0.02f, 1.0f, "%.2f px/tick");
    ImGui::SameLine();
    static const char* kSnaps[3] = {"8분", "16분", "32분"};
    ImGui::SetNextItemWidth(70);
    ImGui::Combo("격자##ds", &state.drumSnap, kSnaps, 3);
    ImGui::SameLine();
    if (ImGui::Button("마디 복제")) {
        // 재생 헤드가 있는 마디의 드럼 노트를 다음 마디에 복사한다
        const uint32_t tpbD = songTicksPerBar(state);
        const uint32_t b0 = (state.playPosTick / tpbD) * tpbD;
        state.snapshot();
        const auto ns = seq::extractNotes(track);
        for (const auto& n : ns) {
            if (n.startTick < b0 || n.startTick >= b0 + tpbD) continue;
            track.addNote(n.startTick + tpbD, n.endTick - n.startTick, n.note, n.velocity);
            seq::adoptNoteIntoClips(track, n.note, n.startTick + tpbD);
        }
        track.sortEvents();
        refreshPlaybackIfPlaying(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("재생 헤드가 있는 마디의 노트를 다음 마디에 복사합니다");
    ImGui::SameLine();
    sliderIntPM("##swing", &state.drumSwing, 0, 100, 1, "스윙 %d%%", 110.0f);
    ImGui::SameLine();
    if (ImGui::Button("적용##swing")) {
        const uint32_t gridS = std::max<uint32_t>(
            1, (uint32_t)state.song.ppqn /
                   (state.drumSnap == 0 ? 2u : (state.drumSnap == 1 ? 4u : 8u)));
        applySwing(state, track, gridS, state.drumSwing);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("엇박(짝수 격자 칸)을 뒤로 밀어 그루브를 만듭니다.\n"
                          "0%%=정직, 100%%=셋잇단 느낌. 선택이 있으면 선택만, 없으면 전체.\n"
                          "언두(Ctrl+Z)로 되돌릴 수 있습니다.");
    ImGui::SameLine();
    if (ImGui::Button("패턴 채우기")) ImGui::OpenPopup("drumpat");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("박자표와 스타일을 고르면 드럼 패턴을 자동으로 채웁니다.\n"
                          "재생 헤드가 있는 마디부터 채우고, 그 구간은 먼저 비웁니다.");
    if (ImGui::BeginPopup("drumpat")) {
        static int s_patSig = -1;   // -1 = 곡 박자표 따라감
        static int s_patStyle = 0;
        static int s_patBars = 4;
        static bool s_patSetSig = true;
        ImGui::TextUnformatted("드럼 패턴 자동 생성");
        ImGui::Separator();
        ImGui::SetNextItemWidth(120);
        const char* kSigs[] = {"곡 박자표", "4/4", "3/4", "6/8"};
        int sigSel = s_patSig + 1; // -1..2 → 0..3
        if (ImGui::Combo("박자표##pat", &sigSel, kSigs, 4)) s_patSig = sigSel - 1;
        ImGui::SetNextItemWidth(120);
        const char* kStyles[] = {seq::drumStyleName(0), seq::drumStyleName(1),
                                 seq::drumStyleName(2)};
        ImGui::Combo("스타일##pat", &s_patStyle, kStyles, seq::drumStyleCount());
        sliderIntPM("마디 수##pat", &s_patBars, 1, 32, 1, "%d마디", 120.0f);
        ImGui::Checkbox("박자표도 이 값으로 설정##pat", &s_patSetSig);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("4/4·3/4·6/8을 고르면 곡의 박자표도 맞춰 바꿉니다\n"
                              "(마디선·메트로놈이 어긋나지 않게).");
        ImGui::Separator();
        if (ImGui::Button("채우기##patgo", ImVec2(120, 0))) {
            const int sig = s_patSig < 0 ? state.metroSigIndex : s_patSig;
            if (s_patSetSig && s_patSig >= 0) state.metroSigIndex = s_patSig;
            state.snapshot();
            const uint32_t tpbP = songTicksPerBar(state);
            const uint32_t startB = (state.playPosTick / tpbP) * tpbP;
            const uint32_t endB = startB + (uint32_t)s_patBars * tpbP;
            // 채울 구간의 기존 드럼 노트를 먼저 지운다 (덮어쓰기)
            seq::eraseMidiRange(track, startB, endB);
            const auto hits = seq::generateDrumPattern(sig, s_patStyle,
                                                       (uint32_t)state.song.ppqn,
                                                       s_patBars, startB);
            const uint32_t nd = std::max<uint32_t>(1, (uint32_t)state.song.ppqn / 4);
            for (const auto& h : hits) {
                track.addNote(h.tick, nd, h.note, h.velocity);
                seq::adoptNoteIntoClips(track, h.note, h.tick);
            }
            track.sortEvents();
            refreshPlaybackIfPlaying(state);
            state.statusMessage = std::string("드럼 패턴 채움: ") +
                                  seq::drumStyleName(s_patStyle) + " · " +
                                  std::to_string(s_patBars) + "마디";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("취소##pat")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("킷##kit")) ImGui::OpenPopup("drumkit");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("지금 배정된 드럼 샘플 세트를 킷으로 저장하거나,\n"
                          "저장해 둔 킷을 불러옵니다 (프로젝트와 무관하게 공용)");
    drawDrumKitPopup(state);
    ImGui::SameLine();
    ImGui::TextDisabled("클릭: 찍기 · 끌기: 이동 · Alt+세로: 세기 · Ctrl+클릭: 길이 반 · "
                        "Ctrl+드래그(빈 곳): 선택 · 우클릭: 지우기 · 이름 우클릭: 샘플");

    // 휠 = 확대/축소, Ctrl+휠 = 가로 스크롤 (트랙 뷰와 동일 체계).
    // 가로 스크롤 요청은 타임라인 child가 소비한다.
    static float s_drumScrollX = 0.0f;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl)
                s_drumScrollX += -wheel * 160.0f;
            else
                state.drumZoom =
                    std::clamp(state.drumZoom * std::pow(1.15f, wheel), 0.02f, 1.0f);
            ImGui::GetIO().MouseWheel = 0.0f;
        }
    }

    const float zoom = state.drumZoom;
    const uint32_t tpb = songTicksPerBar(state);
    const uint32_t songLen = state.timelineBars * tpb;
    const uint32_t grid = std::max<uint32_t>(
        1, (uint32_t)state.song.ppqn /
               (state.drumSnap == 0 ? 2u : (state.drumSnap == 1 ? 4u : 8u)));
    const float timelineW = (float)songLen * zoom + 40.0f;
    const float bodyH = kRowH * kNumRows;

    // ── 왼쪽: 드럼 이름 (고정) ──
    ImGui::BeginChild("##dlabels", ImVec2(kLabelW, bodyH + 16.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const ImVec2 lp0 = ImGui::GetCursorScreenPos();
        for (int r = 0; r < kNumRows; ++r) {
            const float y0 = lp0.y + kRowH * (float)r;
            ImGui::SetCursorScreenPos(ImVec2(lp0.x, y0));
            ImGui::PushID(r);
            if (ImGui::InvisibleButton("##dl", ImVec2(kLabelW - 4.0f, kRowH)))
                triggerNote(state, 9, kRows[r].note, 110, 0.3); // 클릭 = 미리듣기
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                state.drumBrowseNote = kRows[r].note; // 우클릭 = 샘플 선택 창
            }
            const auto spIt = state.drumSamplePaths.find((int)kRows[r].note);
            const bool hasSample = spIt != state.drumSamplePaths.end();
            if (ImGui::IsItemHovered()) {
                if (hasSample) {
                    const std::size_t sl = spIt->second.find_last_of("\\/");
                    ImGui::SetTooltip("클릭: 미리듣기 · 우클릭: 샘플 선택\n샘플: %s",
                                      (sl == std::string::npos ? spIt->second
                                                               : spIt->second.substr(sl + 1))
                                          .c_str());
                } else {
                    ImGui::SetTooltip("클릭: 미리듣기 (내장 신스)\n우클릭: 샘플 선택");
                }
            }
            ldl->AddRectFilled(ImVec2(lp0.x, y0), ImVec2(lp0.x + kLabelW - 4.0f, y0 + kRowH),
                               (r & 1) ? IM_COL32(36, 36, 44, 255) : IM_COL32(42, 42, 50, 255));
            ldl->AddText(ImVec2(lp0.x + 6.0f, y0 + 4.0f),
                         ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255)
                                                : IM_COL32(195, 195, 210, 255),
                         kRows[r].name);
            if (hasSample) // 샘플 배정 표시 (주황 점)
                ldl->AddCircleFilled(ImVec2(lp0.x + kLabelW - 12.0f, y0 + kRowH * 0.5f), 3.0f,
                                     IM_COL32(255, 170, 70, 255));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);

    // ── 오른쪽: 타임라인 (가로 스크롤) ──
    // 휠 자동 스크롤은 끈다 — Shift+휠을 ImGui가 가로 스크롤로 바꾸는 것 방지.
    ImGui::BeginChild("##dtimeline", ImVec2(0.0f, bodyH + 16.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (s_drumScrollX != 0.0f) { // Ctrl+휠 가로 스크롤 소비
            ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + s_drumScrollX));
            s_drumScrollX = 0.0f;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##dcanvas", ImVec2(timelineW, bodyH));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mp = ImGui::GetIO().MousePos;

        // 배경 행 + 박/마디 선
        for (int r = 0; r < kNumRows; ++r) {
            const float y0 = p0.y + kRowH * (float)r;
            dl->AddRectFilled(ImVec2(p0.x, y0), ImVec2(p0.x + timelineW, y0 + kRowH),
                              (r & 1) ? IM_COL32(30, 30, 36, 255) : IM_COL32(35, 35, 42, 255));
        }
        for (uint32_t t = 0; t <= songLen; t += (uint32_t)state.song.ppqn) { // 박
            const float x = p0.x + (float)t * zoom;
            const bool bar = (t % tpb) == 0;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + bodyH),
                        bar ? IM_COL32(85, 85, 100, 200) : IM_COL32(55, 55, 66, 120),
                        bar ? 1.5f : 1.0f);
        }
        // 루프 구간
        if (state.loopEnabled) {
            const float lx0 = p0.x + (float)state.loopStartTick * zoom;
            const float lx1 = p0.x + (float)state.loopEndTick * zoom;
            dl->AddRectFilled(ImVec2(lx0, p0.y), ImVec2(lx1, p0.y + bodyH),
                              IM_COL32(90, 150, 240, 30));
        }

        // 노트 (드럼 = 시작점만 의미 있으니 그리드 폭의 짧은 블록으로).
        // 그리기는 캐시로, 입력 처리는 이 프레임 값의 복사본으로 쓴다
        // (입력이 트랙을 수정하면 캐시가 무효화돼도 이 벡터는 안전하다).
        const std::vector<seq::NoteSpan> notes = cachedNotes(track, di);
        for (const auto& n : notes) {
            const int r = rowForNote(n.note);
            if (r < 0) continue; // 표에 없는 노트는 표시하지 않는다 (피아노 롤에서 편집)
            const float x = p0.x + (float)n.startTick * zoom;
            const float y0 = p0.y + kRowH * (float)r;
            // 실제 노트 길이로 그린다 (Ctrl+클릭 길이 반이 눈에 보이게)
            const float w =
                std::max(4.0f, (float)(n.endTick - n.startTick) * zoom - 1.0f);
            // 벨로시티를 색 밝기로 (여리게=어둡게, 세게=밝게). 강세는 노랗게 도드라진다.
            const float vt = (float)n.velocity / 127.0f;
            const ImU32 noteCol = IM_COL32((int)(150 + 105 * vt), (int)(80 + 95 * vt),
                                           (int)(35 + 40 * vt), (int)(170 + 85 * vt));
            dl->AddRectFilled(ImVec2(x, y0 + 3.0f), ImVec2(x + w, y0 + kRowH - 3.0f), noteCol,
                              3.0f);
            if (state.selectedNotes.count({n.note, n.startTick})) // 선택 = 흰 테두리
                dl->AddRect(ImVec2(x - 1.0f, y0 + 2.0f), ImVec2(x + w + 1.0f, y0 + kRowH - 2.0f),
                            IM_COL32(255, 255, 255, 235), 3.0f, 0, 1.6f);
        }

        // 입력 규칙:
        //  - 빈 칸 클릭/드래그 = 찍기 (페인트), Shift = 강세
        //  - 노트 클릭 = 잡기 -> 끌면 좌우 이동, 안 끌고 놓으면 삭제(토글)
        //  - 우클릭 = 그 칸 비우기
        //  - Ctrl+드래그 = 사각 선택 (선택 잡아끌기 = 무리 이동, Del = 삭제)
        // 겹친 복제본(Ctrl+D 등)이 남지 않도록 삭제/이동은 "칸 전체"를 다룬다.
        static bool s_painting = false;   // 누른 채 페인트 중
        static bool s_selecting = false;  // Ctrl+드래그 사각 선택 중
        static bool s_moving = false;     // 선택 무리 좌우 이동 중
        static bool s_noteDrag = false;   // 단일 노트 잡고 이동 중 (안 움직이면 삭제)
        static bool s_velDrag = false;    // Alt+세로 드래그로 벨로시티 조절 중
        static bool s_scrub = false;      // 재생 헤드(빨간 바) 드래그 중
        static bool s_movedAny = false;
        static uint32_t s_selT0 = 0;      // 선택 시작 (틱/행)
        static int s_selR0 = 0;
        static uint32_t s_lastCell = UINT32_MAX;
        static int s_lastRow = -1;
        static uint32_t s_grabCell = 0;   // 단일 노트를 잡은 칸/행
        static int s_grabRow = -1;
        static float s_velStartY = 0.0f;  // 벨로시티 드래그 기준
        static int s_velBase = 100;
        static uint32_t s_velCell = 0;
        static int s_velRow = -1;

        // 그 칸(행 + 격자 칸)에 걸린 노트 전부 (겹친 복제본 포함)
        const auto cellNotes = [&](int row, uint32_t cellTick) {
            std::vector<seq::NoteSpan> out;
            for (const auto& n : notes)
                if (n.note == kRows[row].note && n.startTick >= cellTick &&
                    n.startTick < cellTick + grid)
                    out.push_back(n);
            return out;
        };
        const auto clearCell = [&](int row, uint32_t cellTick) { // 칸 비우기
            int removed = 0;
            for (const auto& n : cellNotes(row, cellTick)) {
                seq::removeNote(track, n);
                state.selectedNotes.erase({n.note, n.startTick});
                ++removed;
            }
            return removed;
        };

        // 놓는 순간: 단일 노트 드래그가 이동 없이 끝났으면 "삭제"였던 것
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (s_noteDrag && !s_movedAny && s_grabRow >= 0) {
                clearCell(s_grabRow, s_grabCell);
                refreshPlaybackIfPlaying(state);
            } else if (((s_noteDrag || s_moving) && s_movedAny) || s_velDrag) {
                refreshPlaybackIfPlaying(state);
            }
            s_painting = s_selecting = s_moving = s_noteDrag = s_velDrag = s_scrub =
                s_movedAny = false;
            s_lastCell = UINT32_MAX;
            s_lastRow = -1;
            s_grabRow = -1;
        }
        const bool inBody = mp.x >= p0.x && mp.x < p0.x + timelineW && mp.y >= p0.y &&
                            mp.y < p0.y + bodyH;
        // 재생 헤드(빨간 바) 잡아 끌기 — 노트보다 우선한다 (±5px)
        const float headX = p0.x + (float)state.playPosTick * zoom;
        const bool nearHead = std::fabs(mp.x - headX) <= 5.0f;
        if ((hovered && inBody && nearHead) || s_scrub)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (hovered && inBody && nearHead && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            s_scrub = true;
        if (s_scrub && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const uint32_t t = mp.x > p0.x ? (uint32_t)((mp.x - p0.x) / zoom) : 0;
            seekTo(state, t, /*scrollView=*/false);
        }
        if ((hovered && inBody && !s_scrub && !nearHead) || s_painting || s_selecting ||
            s_moving || s_noteDrag || s_velDrag) {
            const int r = std::clamp((int)((mp.y - p0.y) / kRowH), 0, kNumRows - 1);
            const uint32_t rawTick =
                mp.x > p0.x ? (uint32_t)((mp.x - p0.x) / zoom) : 0;
            const uint32_t cell = rawTick / grid * grid; // 격자 스냅
            const auto hits = cellNotes(r, cell);
            const bool ctrl = ImGui::GetIO().KeyCtrl;

            if (hovered && inBody && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                bool hitSelected = false; // 잡은 칸에 선택된 노트가 있는가
                for (const auto& h : hits)
                    if (state.selectedNotes.count({h.note, h.startTick})) hitSelected = true;
                if (ctrl && !hits.empty()) {
                    // Ctrl+클릭 (노트 위): 피아노 롤처럼 길이를 반으로
                    state.snapshot();
                    for (const auto& h : hits) {
                        const uint32_t dur =
                            h.endTick > h.startTick ? h.endTick - h.startTick : 2;
                        const uint32_t nd = dur / 2 > 0 ? dur / 2 : 1;
                        seq::removeNote(track, h);
                        track.addNote(h.startTick, nd, h.note, h.velocity);
                        seq::adoptNoteIntoClips(track, h.note, h.startTick);
                    }
                    track.sortEvents();
                    refreshPlaybackIfPlaying(state);
                } else if (ctrl) {
                    // Ctrl+드래그 (빈 곳): 사각 선택 시작
                    s_selecting = true;
                    s_selT0 = rawTick;
                    s_selR0 = r;
                    state.selectedNotes.clear();
                } else if (ImGui::GetIO().KeyAlt && !hits.empty()) {
                    // Alt+세로 드래그: 이 칸 노트의 벨로시티(세기) 조절
                    state.snapshot();
                    s_velDrag = true;
                    s_velStartY = mp.y;
                    int vb = 1;
                    for (const auto& h : hits) vb = std::max(vb, (int)h.velocity);
                    s_velBase = vb;
                    s_velCell = cell;
                    s_velRow = r;
                } else if (hitSelected) {
                    // 선택 무리를 잡으면 -> 좌우 이동 시작
                    state.snapshot(); // 이동 전체가 언두 1회
                    s_moving = true;
                    s_movedAny = false;
                    s_lastCell = cell;
                } else if (!state.selectedNotes.empty()) {
                    state.selectedNotes.clear(); // 선택이 있으면 첫 클릭은 해제만
                } else if (!hits.empty()) {
                    // 노트 잡기: 끌면 이동, 안 끌고 놓으면 삭제 (놓을 때 판정)
                    state.snapshot();
                    s_noteDrag = true;
                    s_movedAny = false;
                    s_grabCell = cell;
                    s_grabRow = r;
                    s_lastCell = cell;
                } else {
                    state.snapshot();
                    const uint8_t vel = ImGui::GetIO().KeyShift ? 127 : 100;
                    track.addNote(cell, grid * 3 / 4, kRows[r].note, vel);
                    seq::adoptNoteIntoClips(track, kRows[r].note, cell);
                    track.sortEvents();
                    triggerNote(state, 9, kRows[r].note, vel, 0.25);
                    s_painting = true; // 이제 끌면 페인트
                    s_lastCell = cell;
                    s_lastRow = r;
                    refreshPlaybackIfPlaying(state);
                }
            } else if (s_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // 사각 선택 갱신 (틱/행 범위 안의 노트를 선택으로)
                const uint32_t ta = std::min(s_selT0, rawTick);
                const uint32_t tb = std::max(s_selT0, rawTick);
                const int ra = std::min(s_selR0, r), rb = std::max(s_selR0, r);
                state.selectedNotes.clear();
                for (const auto& n : notes) {
                    const int nr = rowForNote(n.note);
                    if (nr < ra || nr > rb) continue;
                    if (n.startTick + grid < ta || n.startTick > tb) continue;
                    state.selectedNotes.insert({n.note, n.startTick});
                }
                // 선택 사각형 표시
                const float sx0 = p0.x + (float)ta * zoom;
                const float sx1 = p0.x + (float)tb * zoom;
                const float sy0 = p0.y + kRowH * (float)ra;
                const float sy1 = p0.y + kRowH * (float)(rb + 1);
                dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                  IM_COL32(150, 200, 255, 40));
                dl->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                            IM_COL32(150, 200, 255, 200), 0.0f, 0, 1.5f);
            } else if (s_velDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // 위로 끌면 세게, 아래로 끌면 여리게 (색 밝기로 바로 보인다)
                const int nv = std::clamp(
                    s_velBase + (int)((s_velStartY - mp.y) * 0.7f), 1, 127);
                for (const auto& h : cellNotes(s_velRow, s_velCell))
                    seq::setNoteVelocity(track, h, (uint8_t)nv);
                ImGui::SetTooltip("세기 %d", nv);
            } else if (s_noteDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                       cell != s_lastCell &&
                       ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                // 4px 이상 끌었을 때만 이동 — 작은 칸(32분)에서 클릭이 옆 칸으로
                // 살짝 새도 삭제(토글)가 이동으로 바뀌지 않는다
                // 단일 노트 이동: 잡은 칸의 노트(겹침 포함)를 대상 칸으로 옮긴다.
                // 대상 칸 시작에 정확히 스냅되므로 격자 밖 복제본도 정리된다.
                const auto grabbed = cellNotes(s_grabRow, s_grabCell);
                if (!grabbed.empty()) {
                    uint8_t vel = 0;
                    for (const auto& g : grabbed) vel = std::max(vel, g.velocity);
                    for (const auto& g : grabbed) seq::removeNote(track, g);
                    clearCell(s_grabRow, cell); // 대상 칸에 이미 있으면 대체
                    track.addNote(cell, grid * 3 / 4, kRows[s_grabRow].note, vel);
                    seq::adoptNoteIntoClips(track, kRows[s_grabRow].note, cell);
                    track.sortEvents();
                    s_grabCell = cell;
                    s_lastCell = cell;
                    s_movedAny = true;
                }
            } else if (s_moving && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                       cell != s_lastCell) {
                // 선택 무리 좌우 이동 (격자 단위). 스냅샷은 시작 때 한 번만 했으니
                // 여기서는 직접 옮긴다 (moveSelectedNotes는 매번 스냅샷을 남긴다).
                const long dT = (long)cell - (long)s_lastCell;
                auto sel = gatherSelected(state, track);
                if (!sel.empty()) {
                    bool ok = true; // 왼쪽 끝을 넘으면 이번 이동은 건너뛴다
                    for (const auto& s : sel)
                        if ((long)s.startTick + dT < 0) ok = false;
                    if (ok) {
                        std::set<std::pair<uint8_t, uint32_t>> newSel;
                        for (const auto& s : sel) seq::removeNote(track, s);
                        for (const auto& s : sel) {
                            // 이동하면서 격자에 반올림 스냅 — 어긋나 있던 노트도
                            // (옛 Ctrl+D 복제본 등) 옮기는 순간 마디에 맞는다
                            long nt = (long)s.startTick + dT;
                            nt = (nt + (long)grid / 2) / (long)grid * (long)grid;
                            if (nt < 0) nt = 0;
                            track.addNote((uint32_t)nt, s.endTick - s.startTick, s.note,
                                          s.velocity);
                            seq::adoptNoteIntoClips(track, s.note, (uint32_t)nt);
                            newSel.insert({s.note, (uint32_t)nt});
                        }
                        track.sortEvents();
                        state.selectedNotes = std::move(newSel);
                        s_movedAny = true;
                        s_lastCell = cell;
                    }
                }
            } else if (s_painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                       (cell != s_lastCell || r != s_lastRow)) {
                // 드래그 페인트: 지나가는 빈 칸마다 노트를 심는다 (한 칸에 하나)
                if (hits.empty()) {
                    const uint8_t vel = ImGui::GetIO().KeyShift ? 127 : 100;
                    track.addNote(cell, grid * 3 / 4, kRows[r].note, vel);
                    seq::adoptNoteIntoClips(track, kRows[r].note, cell);
                    track.sortEvents();
                    triggerNote(state, 9, kRows[r].note, vel, 0.15);
                    refreshPlaybackIfPlaying(state);
                }
                s_lastCell = cell;
                s_lastRow = r;
            } else if (hovered && inBody && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                       !hits.empty()) {
                state.snapshot();
                clearCell(r, cell); // 겹친 복제본까지 한 번에 비운다
                refreshPlaybackIfPlaying(state);
            }
        }

        // 재생 헤드 + 따라가기
        const float hx = p0.x + (float)state.playPosTick * zoom;
        dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + bodyH), IM_COL32(255, 90, 90, 220),
                    1.5f);
        const bool playing = state.player && state.player->isPlaying();
        if ((playing && state.followPlayhead) || state.scrollToPlayhead) {
            const float target =
                (float)state.playPosTick * zoom - ImGui::GetWindowSize().x * 0.4f;
            ImGui::SetScrollX(std::max(0.0f, target));
        }
    }
    ImGui::EndChild();

    ImGui::End();

    drawDrumSampleBrowser(state); // 샘플 선택 창 (라벨 우클릭으로 연다)
}

} // namespace midipro::gui
