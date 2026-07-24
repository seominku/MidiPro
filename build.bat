@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [ERROR] vcvars64 failed & exit /b 1 )
if not exist build mkdir build

set INC=/I external\rtmidi /I external\rtaudio /I external\imgui /I external\imgui\backends /I external\vst3sdk /I external\dr_libs /I external\miniz /I external\asiosdk\common /I external\asiosdk\host /I external\asiosdk\host\pc /I src
set CFLAGS=/nologo /EHsc /std:c++17 /utf-8 /O2 /W3 /D__WINDOWS_MM__ /D__WINDOWS_WASAPI__ /D__WINDOWS_ASIO__ /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS /D_CRT_SECURE_NO_WARNINGS /DRELEASE=1

REM ASIO SDK 호스트 소스 (저지연 오디오; RtAudio가 __WINDOWS_ASIO__로 사용)
set ASIO=external\asiosdk\common\asio.cpp external\asiosdk\host\asiodrivers.cpp external\asiosdk\host\pc\asiolist.cpp

set VS=external\vst3sdk
set VSTSDK=%VS%\pluginterfaces\base\funknown.cpp %VS%\pluginterfaces\base\coreiids.cpp %VS%\pluginterfaces\base\conststringtable.cpp %VS%\pluginterfaces\base\ustring.cpp %VS%\base\source\fstring.cpp %VS%\base\source\fbuffer.cpp %VS%\base\source\fdebug.cpp %VS%\base\source\fobject.cpp %VS%\base\source\updatehandler.cpp %VS%\base\source\baseiids.cpp %VS%\base\source\fstreamer.cpp %VS%\base\source\fdynlib.cpp %VS%\base\source\timer.cpp %VS%\base\thread\source\flock.cpp %VS%\base\thread\source\fcondition.cpp %VS%\public.sdk\source\common\memorystream.cpp %VS%\public.sdk\source\common\commoniids.cpp %VS%\public.sdk\source\common\pluginview.cpp %VS%\public.sdk\source\common\commonstringconvert.cpp %VS%\public.sdk\source\common\threadchecker_win32.cpp %VS%\public.sdk\source\vst\vstinitiids.cpp %VS%\public.sdk\source\vst\utility\stringconvert.cpp %VS%\public.sdk\source\vst\hosting\module.cpp %VS%\public.sdk\source\vst\hosting\module_win32.cpp %VS%\public.sdk\source\vst\hosting\hostclasses.cpp %VS%\public.sdk\source\vst\hosting\eventlist.cpp %VS%\public.sdk\source\vst\hosting\parameterchanges.cpp %VS%\public.sdk\source\vst\hosting\processdata.cpp %VS%\public.sdk\source\vst\hosting\connectionproxy.cpp %VS%\public.sdk\source\vst\hosting\pluginterfacesupport.cpp
set VST=src\vst\Vst3Host.cpp src\vst\PluginScanner.cpp src\vst\PluginCache.cpp %VSTSDK%

set SEQ=src\sequencer\TimeBase.cpp src\sequencer\Track.cpp src\sequencer\Song.cpp src\sequencer\SmfFile.cpp src\sequencer\Player.cpp src\sequencer\TabImport.cpp
set PDFTAB=src\pdf\PdfTab.cpp external\miniz\miniz.c
set MIDI=src\midi\MidiMessage.cpp src\midi\RtMidiDevice.cpp external\rtmidi\RtMidi.cpp src\midi2\Ump.cpp
set AUDIO=src\audio\Synth.cpp src\audio\SynthPreset.cpp src\audio\BuiltinFx.cpp src\audio\AudioClip.cpp src\audio\WavFile.cpp src\audio\Mp3Writer.cpp src\audio\RtAudioEngine.cpp external\rtaudio\RtAudio.cpp %ASIO%
set GUITAR=src\guitar\Fretboard.cpp
set MAPPING=src\mapping\MidiMap.cpp
set CORE=src\core\CrashLog.cpp
set PROJECT=src\project\Project.cpp
set IMGUI=external\imgui\imgui.cpp external\imgui\imgui_draw.cpp external\imgui\imgui_tables.cpp external\imgui\imgui_widgets.cpp external\imgui\backends\imgui_impl_win32.cpp external\imgui\backends\imgui_impl_dx11.cpp
set GUILIBS=winmm.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib user32.lib d3d11.lib dxgi.lib d3dcompiler.lib comdlg32.lib dwmapi.lib mfplat.lib mfreadwrite.lib mfuuid.lib windowscodecs.lib dbghelp.lib

echo === Building GUI app (MidiPro.exe) ===
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 ( echo [ERROR] rc failed & exit /b 1 )
cl %CFLAGS% %INC% src\main_gui.cpp src\gui\App.cpp src\gui\Panels.cpp src\gui\PanelsMixer.cpp src\gui\PanelsTransport.cpp src\gui\PanelsTrackView.cpp src\gui\PanelsPianoRoll.cpp src\gui\PanelsDrums.cpp src\gui\PanelsArrange.cpp src\gui\PanelsGuitarTab.cpp src\gui\Theme.cpp src\gui\BackgroundImage.cpp src\gui\UiSkin.cpp src\gui\Settings.cpp %CORE% %MIDI% %AUDIO% %SEQ% %PDFTAB% %GUITAR% %MAPPING% %PROJECT% %VST% %IMGUI% %GUILIBS% build\app.res /Fe:build\MidiPro.exe /Fo:build\ /link /SUBSYSTEM:WINDOWS
if errorlevel 1 ( echo [ERROR] GUI build failed & exit /b 1 )

echo === Building console app (MidiProConsole.exe) ===
cl %CFLAGS% %INC% src\main.cpp %MIDI% winmm.lib /Fe:build\MidiProConsole.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] console build failed & exit /b 1 )

echo === Building + running unit tests ===
cl %CFLAGS% %INC% tests\test_midi_message.cpp src\midi\MidiMessage.cpp /Fe:build\MidiProTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] midi test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_sequencer.cpp %SEQ% %PDFTAB% %GUITAR% /Fe:build\MidiProSeqTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] seq test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_synth.cpp src\audio\Synth.cpp src\audio\SynthPreset.cpp src\audio\BuiltinFx.cpp /Fe:build\MidiProSynthTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] synth test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_mapping.cpp %MAPPING% /Fe:build\MidiProMapTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] mapping test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_undo.cpp src\sequencer\Track.cpp src\sequencer\Song.cpp src\sequencer\TimeBase.cpp /Fe:build\MidiProUndoTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] undo test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_ump.cpp src\midi2\Ump.cpp /Fe:build\MidiProUmpTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] ump test build failed & exit /b 1 )
cl %CFLAGS% %INC% tests\test_project.cpp src\project\Project.cpp src\audio\SynthPreset.cpp src\audio\WavFile.cpp src\audio\AudioClip.cpp src\audio\Mp3Writer.cpp src\mapping\MidiMap.cpp src\sequencer\Track.cpp src\sequencer\Song.cpp src\sequencer\TimeBase.cpp ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib windowscodecs.lib dbghelp.lib /Fe:build\MidiProProjectTests.exe /Fo:build\
if errorlevel 1 ( echo [ERROR] project test build failed & exit /b 1 )
build\MidiProTests.exe
if errorlevel 1 ( echo [ERROR] midi tests failed & exit /b 1 )
build\MidiProSeqTests.exe
if errorlevel 1 ( echo [ERROR] seq tests failed & exit /b 1 )
build\MidiProSynthTests.exe
if errorlevel 1 ( echo [ERROR] synth tests failed & exit /b 1 )
build\MidiProMapTests.exe
if errorlevel 1 ( echo [ERROR] mapping tests failed & exit /b 1 )
build\MidiProUndoTests.exe
if errorlevel 1 ( echo [ERROR] undo tests failed & exit /b 1 )
build\MidiProUmpTests.exe
if errorlevel 1 ( echo [ERROR] ump tests failed & exit /b 1 )
build\MidiProProjectTests.exe
if errorlevel 1 ( echo [ERROR] project tests failed & exit /b 1 )

echo [OK] build\MidiPro.exe
endlocal
