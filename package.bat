@echo off
setlocal
REM =============================================================
REM MidiPro packaging: build + tests, then create dist\MidiPro.zip
REM Static CRT (/MT default) so the exe runs on other PCs as-is.
REM NOTE: keep this file ASCII-only (cmd mangles UTF-8 Korean).
REM =============================================================

call .\build.bat
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )

if not exist dist mkdir dist
if exist dist\MidiPro rmdir /s /q dist\MidiPro
mkdir dist\MidiPro

copy /y build\MidiPro.exe dist\MidiPro\ >nul
if errorlevel 1 ( echo [ERROR] copy failed & exit /b 1 )

REM user manual (Korean filename, so use wildcard to keep this file ASCII)
if exist docs\*.md copy /y docs\*.md dist\MidiPro\ >nul

(
echo MidiPro - MIDI sequencer + audio recording + VST3 host
echo.
echo Run: double-click MidiPro.exe
echo Settings/autosave: %%LOCALAPPDATA%%\MidiPro
echo Shortcut help: press F1 in the app
) > dist\MidiPro\README.txt

powershell -NoProfile -Command "Compress-Archive -Path 'dist\MidiPro\*' -DestinationPath 'dist\MidiPro.zip' -Force"
if errorlevel 1 ( echo [ERROR] zip failed & exit /b 1 )

echo [OK] dist\MidiPro.zip
endlocal
