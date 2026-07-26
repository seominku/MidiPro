// =============================================================
// MidiPro - gui/Settings.cpp
// =============================================================

#include "gui/Settings.h"

#include <fstream>
#include <sstream>

namespace midipro::gui {

namespace {
// 값은 줄 끝까지 읽는다 (장치 이름에 공백이 흔하다: "MPK mini 3 0")
std::string restOfLine(std::istringstream& ls) {
    std::string v;
    std::getline(ls, v);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}
} // namespace

bool saveSettings(const AppSettings& s, const std::filesystem::path& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "midipro_settings 1\n";
    f << "midi_in_auto " << (s.midiInAutoOpen ? 1 : 0) << '\n';
    if (!s.midiInPort.empty()) f << "midi_in " << s.midiInPort << '\n';
    f << "midi_out_auto " << (s.midiOutAutoOpen ? 1 : 0) << '\n';
    if (!s.midiOutPort.empty()) f << "midi_out " << s.midiOutPort << '\n';
    f << "soft_thru " << (s.softThru ? 1 : 0) << '\n';
    f << "start_screen " << (s.startScreenOnLaunch ? 1 : 0) << '\n';
    return f.good();
}

bool loadSettings(AppSettings& s, const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line.rfind("midipro_settings", 0) != 0) return false;

    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "midi_in") s.midiInPort = restOfLine(ls);
        else if (key == "midi_out") s.midiOutPort = restOfLine(ls);
        else if (key == "midi_in_auto") { int v = 1; ls >> v; s.midiInAutoOpen = v != 0; }
        else if (key == "midi_out_auto") { int v = 1; ls >> v; s.midiOutAutoOpen = v != 0; }
        else if (key == "soft_thru") { int v = 1; ls >> v; s.softThru = v != 0; }
        else if (key == "start_screen") { int v = 1; ls >> v; s.startScreenOnLaunch = v != 0; }
        // 모르는 키는 넘어간다 (앞뒤 버전 호환)
    }
    return true;
}

} // namespace midipro::gui
