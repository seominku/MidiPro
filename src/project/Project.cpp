// =============================================================
// MidiPro - project/Project.cpp
// =============================================================

#include "project/Project.h"

#include "audio/SynthPreset.h"

#include <fstream>
#include <sstream>

namespace midipro::project {

namespace {

// 곡 -> 텍스트 (track/ev 라인). 트랙 이름은 줄 나머지 전체로 둔다.
std::string songToText(const seq::Song& song) {
    std::ostringstream os;
    os << "bpm " << song.bpm << "\n";
    os << "ppqn " << song.ppqn << "\n";
    for (const auto& t : song.tracks) {
        os << "track " << (int)t.channel << " " << (t.muted ? 1 : 0) << " " << t.name << "\n";
        for (const auto& e : t.events)
            os << "ev " << e.tick << " " << (int)e.status << " " << (int)e.data1 << " "
               << (int)e.data2 << "\n";
    }
    return os.str();
}

// 텍스트 -> 곡
void songFromText(const std::string& text, seq::Song& song) {
    song = seq::Song{};
    std::istringstream is(text);
    std::string line;
    seq::Track* cur = nullptr;
    while (std::getline(is, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "bpm") {
            ls >> song.bpm;
        } else if (key == "ppqn") {
            ls >> song.ppqn;
        } else if (key == "track") {
            int ch = 0, muted = 0;
            ls >> ch >> muted;
            seq::Track t;
            t.channel = (uint8_t)ch;
            t.muted = muted != 0;
            std::string name;
            std::getline(ls, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1); // 앞 공백 제거
            t.name = name;
            song.tracks.push_back(std::move(t));
            cur = &song.tracks.back();
        } else if (key == "ev" && cur) {
            int tick = 0, status = 0, d1 = 0, d2 = 0;
            ls >> tick >> status >> d1 >> d2;
            seq::MidiEvent e;
            e.tick = (uint32_t)tick;
            e.status = (uint8_t)status;
            e.data1 = (uint8_t)d1;
            e.data2 = (uint8_t)d2;
            cur->events.push_back(e);
        }
    }
}

// 줄에서 "key value" 분리 (value는 나머지 전체)
bool splitKeyValue(const std::string& line, std::string& key, std::string& value) {
    const auto sp = line.find(' ');
    if (sp == std::string::npos) {
        key = line;
        value.clear();
        return !key.empty();
    }
    key = line.substr(0, sp);
    value = line.substr(sp + 1);
    return true;
}

} // namespace

std::string serialize(const ProjectData& p) {
    std::ostringstream os;
    os << "midipro_project 1\n";
    os << "mpe " << (p.mpe ? 1 : 0) << "\n";
    os << "vstinst " << (p.vstInstrumentPath.empty() ? "-" : p.vstInstrumentPath) << "\n";
    os << "vstinstclass " << p.vstInstrumentClass << "\n";
    os << "vstfx " << (p.vstEffectPath.empty() ? "-" : p.vstEffectPath) << "\n";
    os << "vstfxclass " << p.vstEffectClass << "\n";
    // 섹션: 각 모듈의 기존 직렬화기 결과를 그대로 넣는다
    os << "[song]\n" << songToText(p.song);
    os << "[synth]\n" << audio::serialize(p.synth);
    os << "[midimap]\n" << p.midiMap.serialize();
    os << "[end]\n";
    return os.str();
}

bool deserialize(ProjectData& out, const std::string& text) {
    std::istringstream is(text);
    std::string line;
    if (!std::getline(is, line)) return false;
    {
        std::istringstream hs(line);
        std::string tag;
        hs >> tag;
        if (tag != "midipro_project") return false;
    }

    ProjectData p;
    std::string section; // 현재 섹션 이름 ("" = 헤더)
    std::string sectionBuf;

    auto flushSection = [&]() {
        if (section == "song")
            songFromText(sectionBuf, p.song);
        else if (section == "synth")
            audio::deserialize(p.synth, sectionBuf);
        else if (section == "midimap")
            p.midiMap.deserialize(sectionBuf);
        sectionBuf.clear();
    };

    while (std::getline(is, line)) {
        if (!line.empty() && line[0] == '[') {
            flushSection();
            section = line.substr(1, line.find(']') == std::string::npos ? std::string::npos
                                                                         : line.find(']') - 1);
            if (section == "end") break;
            continue;
        }
        if (section.empty()) {
            // 헤더 키/값
            std::string key, value;
            if (!splitKeyValue(line, key, value)) continue;
            if (key == "mpe")
                p.mpe = (value == "1");
            else if (key == "vstinst")
                p.vstInstrumentPath = (value == "-") ? std::string() : value;
            else if (key == "vstinstclass")
                p.vstInstrumentClass = std::atoi(value.c_str());
            else if (key == "vstfx")
                p.vstEffectPath = (value == "-") ? std::string() : value;
            else if (key == "vstfxclass")
                p.vstEffectClass = std::atoi(value.c_str());
        } else {
            sectionBuf += line;
            sectionBuf += "\n";
        }
    }
    flushSection();

    out = std::move(p);
    return true;
}

bool save(const ProjectData& p, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::string text = serialize(p);
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

bool load(ProjectData& out, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return deserialize(out, ss.str());
}

} // namespace midipro::project
