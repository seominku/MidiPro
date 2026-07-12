#pragma once
// =============================================================
// MidiPro - gui/App.h
// 애플리케이션 셸: Win32 창 + D3D11 + ImGui 수명주기와 메인 루프.
//
// 이 클래스만 렌더링 백엔드(DirectX/Win32/ImGui)를 안다.
// 패널(Panels.cpp)과 시퀀서/MIDI 계층은 백엔드를 모른다 (Rule 1).
// =============================================================

#include "gui/AppState.h"
#include "project/Project.h"

#include <filesystem>

namespace midipro::gui {

class App {
public:
    App(midi::IMidiInput& input, midi::MidiOutputRouter& output, audio::ISynthControl& synth,
        audio::IVstHostControl& vst, audio::IMidi2Input& midi2, audio::IAudioClips& audioClips,
        audio::IAudioInput& audioInput);
    ~App();

    int run(); // 창 생성 후 메인 루프 진입. 종료 코드 반환.

private:
    // VST 플러그인 상태(패치)를 사이드카 폴더의 vst_*.bin으로 저장/복원.
    void saveVstStates(const std::filesystem::path& projectPath);
    void loadVstStates(const std::filesystem::path& projectPath);

    // 클릭 샘플(WAV/MP3)을 디코드해 엔진에 설정. kind: 0=일반, 1=카운트인, 2=강조.
    bool loadClickSampleFile(int kind, const std::string& utf8Path);

    // 내보내기: 재생 없이 오프라인으로 렌더해 exportPath에 저장. 결과 메시지 반환.
    std::string runOfflineExport();

    // ---- 프로젝트 저장/불러오기 (메뉴와 자동 저장/복구가 공용) ----
    project::ProjectData buildProjectData() const; // 현재 상태 -> ProjectData
    bool saveProjectTo(const std::filesystem::path& path);
    bool loadProjectFrom(const std::filesystem::path& path);
    // 패키지 내보내기: 프로젝트 + 사용 중 드럼 샘플을 folder에 모은다 (결과 메시지)
    std::string exportPackage(const std::string& folderUtf8);

    // ---- 최근 프로젝트 목록 (recent.txt, 최신이 앞, 최대 8개) ----
    void loadRecentList();
    void addRecentProject(const std::string& utf8Path);

    // ---- 자동 저장 + 크래시 복구 ----
    static std::filesystem::path autosaveDir(); // %LOCALAPPDATA%\MidiPro
    void maybeAutosave();                       // 주기적으로 호출 (변경 시에만 저장)
    double m_lastAutosaveCheck = 0.0;
    std::string m_lastAutosaveText; // 마지막 자동 저장본의 직렬화 텍스트 (변경 감지)

    AppState m_state;
};

} // namespace midipro::gui
