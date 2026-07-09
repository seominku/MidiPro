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
#include "gui/App.h"
#include "midi/MidiOutputRouter.h"
#include "midi/RtMidiDevice.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    midipro::midi::RtMidiInput input;
    midipro::midi::RtMidiOutput hardwareOut;
    midipro::audio::RtAudioEngine synthEngine;

    midipro::midi::MidiOutputRouter router;
    router.addTarget("하드웨어 MIDI", &hardwareOut);
    router.addTarget("내장 신디사이저", &synthEngine);

    midipro::gui::App app(input, router, synthEngine, synthEngine, synthEngine);
    return app.run();
}
