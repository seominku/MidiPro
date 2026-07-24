#pragma once
// =============================================================
// MidiPro - gui/Settings.h
// 프로그램을 껐다 켜도 남아야 하는 자잘한 선택들.
//
// 왜 이름으로 저장하는가:
//   MIDI 포트 번호는 장치를 꽂았다 뺐다 하면 밀린다. 번호를 저장하면
//   다음에 엉뚱한 장치가 열린다. 그래서 "이름"으로 저장하고, 켤 때
//   같은 이름을 찾아 연다 (없으면 조용히 넘어간다).
//
// 저장 위치: %LOCALAPPDATA%\MidiPro\settings.ini
// =============================================================

#include <filesystem>
#include <string>

namespace midipro::gui {

struct AppSettings {
    std::string midiInPort;    // 마지막으로 열어 둔 MIDI 입력 장치 이름
    bool midiInAutoOpen = true; // 시작할 때 자동으로 열까
    std::string midiOutPort;   // 마지막으로 열어 둔 MIDI 출력 장치 이름
    bool midiOutAutoOpen = true;
    bool softThru = true;
};

bool saveSettings(const AppSettings& s, const std::filesystem::path& path);
bool loadSettings(AppSettings& s, const std::filesystem::path& path);

} // namespace midipro::gui
