#pragma once
// =============================================================
// MidiPro - gui/App.h
// 애플리케이션 셸: Win32 창 + D3D11 + ImGui 수명주기와 메인 루프.
//
// 이 클래스만 렌더링 백엔드(DirectX/Win32/ImGui)를 안다.
// 패널(Panels.cpp)과 시퀀서/MIDI 계층은 백엔드를 모른다 (Rule 1).
// =============================================================

#include "gui/AppState.h"

namespace midipro::gui {

class App {
public:
    App(midi::IMidiInput& input, midi::MidiOutputRouter& output, audio::ISynthControl& synth,
        audio::IVstHostControl& vst, audio::IMidi2Input& midi2);
    ~App();

    int run(); // 창 생성 후 메인 루프 진입. 종료 코드 반환.

private:
    AppState m_state;
};

} // namespace midipro::gui
