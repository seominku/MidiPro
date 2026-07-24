// =============================================================
// MidiPro - main_gui.cpp (GUI 진입점, 조립 지점)
//
// 계층 규칙 (Rule 1):
//   구체 타입(RtMidiOutput, RtAudioEngine, RtMidiInput)은 여기서만
//   생성한다. App/Player/Panels에는 인터페이스(IMidiInput,
//   MidiOutputRouter=IMidiOutput, ISynthControl) 참조로만 넘긴다.
//
//   출력 라우터에 두 대상을 등록한다:
//     0) 하드웨어 MIDI (RtMidi) — 예: Windows GS Wavetable
//     1) 내장 신디사이저 (RtAudio) — Phase 3에서 추가
// =============================================================

#include "audio/RtAudioEngine.h"
#include "core/CrashLog.h"
#include "gui/App.h"
#include "midi/MidiOutputRouter.h"
#include "midi/RtMidiDevice.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // 무엇보다 먼저. VST 플러그인은 우리 프로세스 안에서 도므로, 플러그인이
    // 죽으면 우리도 같이 죽는다 — 그때 아무 흔적 없이 사라지지 않게 한다.
    midipro::core::installCrashHandler();

    // 설치 프로그램이 "지금 켜져 있다"를 알아보는 표식.
    // 이름은 installer\MidiPro.iss의 AppMutexName과 같아야 한다. 실행 중에
    // 업데이트를 하면 설치 프로그램이 이걸 보고 종료를 먼저 요청한다.
    // (핸들은 프로세스가 끝날 때 OS가 알아서 닫는다)
    CreateMutexW(nullptr, FALSE, L"MidiPro.SingleInstance.Mutex");

    midipro::midi::RtMidiInput input;
    midipro::midi::RtMidiOutput hardwareOut;
    midipro::audio::RtAudioEngine synthEngine;

    midipro::midi::MidiOutputRouter router;
    router.addTarget("하드웨어 MIDI", &hardwareOut);
    router.addTarget("내장 신디사이저", &synthEngine);
    router.setActiveTarget(1); // 기본 출력 대상 = 내장 신디사이저

    midipro::gui::App app(input, router, synthEngine, synthEngine, synthEngine, synthEngine,
                          synthEngine);
    return app.run();
}
