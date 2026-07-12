// =============================================================
// MidiPro - project/Project.cpp
// =============================================================

#include "project/Project.h"

#include "audio/AudioClip.h"
#include "audio/SynthPreset.h"
#include "audio/WavFile.h"

#include <fstream>
#include <map>
#include <sstream>

namespace midipro::project {

namespace {

// 트랙 N의 j번째 클립 사이드카 파일명 (결정적으로 생성)
std::string audioFileName(int trackIndex, int clipIndex) {
    return "track" + std::to_string(trackIndex) + "_" + std::to_string(clipIndex) + ".wav";
}

// 클립 객체 -> 사이드카 파일명 레지스트리. 라이브 곡의 클립은 기존
// track{i}_{j}.wav 이름을 그대로 쓰고, 버전에만 있는 클립은 vclip{n}.wav를
// 받는다. 여러 버전이 같은 객체를 공유하면 파일도 하나만 쓴다.
// serialize()와 save()가 같은 결과를 얻도록 결정적으로 만든다.
std::map<const audio::AudioClip*, std::string> buildClipNames(const ProjectData& p) {
    std::map<const audio::AudioClip*, std::string> names;
    for (std::size_t i = 0; i < p.song.tracks.size(); ++i) {
        const auto& t = p.song.tracks[i];
        for (std::size_t j = 0; j < t.clips.size(); ++j)
            if (t.clips[j]) names[t.clips[j].get()] = audioFileName((int)i, (int)j);
    }
    int vn = 0;
    for (const auto& v : p.versions)
        for (const auto& t : v.song.tracks)
            for (const auto& c : t.clips)
                if (c && names.find(c.get()) == names.end())
                    names[c.get()] = "vclip" + std::to_string(vn++) + ".wav";
    return names;
}

// 곡 -> 텍스트 (track/ev 라인). 트랙 이름은 줄 나머지 전체로 둔다.
// track 뒤의 tvol/tinch/tplugin/taudio 라인은 선택적이라, 옛 파일도 그대로 읽힌다.
std::string songToText(const seq::Song& song) {
    std::ostringstream os;
    os << "bpm " << song.bpm << "\n";
    os << "ppqn " << song.ppqn << "\n";
    os << "master " << song.masterVolume << " " << song.masterPan << " " << song.masterGain
       << "\n";
    for (const auto& tc : song.tempoChanges) // 곡 중간 템포 변경 지점들
        os << "tempo " << tc.tick << " " << tc.bpm << " " << (tc.ramp ? 1 : 0) << "\n";
    for (const auto& mk : song.markers) // 구간 마커 (이름은 줄 끝까지)
        os << "marker " << mk.tick << " " << mk.name << "\n";
    for (std::size_t i = 0; i < song.tracks.size(); ++i) {
        const auto& t = song.tracks[i];
        os << "track " << (int)t.channel << " " << (t.muted ? 1 : 0) << " " << t.name << "\n";
        os << "tvol " << t.volume << " " << t.pan << " " << t.gain << " " << t.sendLevel
           << "\n"; // 4번째 값(센드)은 옛 파일엔 없다 (읽기 실패 시 0)
        os << "tinch " << t.inputChannelMode << "\n";
        if (t.frozen) os << "tfrz 1\n"; // 트랙 프리즈 상태
        if (t.isGuitar) os << "tgtr 1\n"; // 기타 트랙 표시
        if (!t.importKey.empty()) // 타브 가져오기 그룹 (키는 줄 끝까지)
            os << "timp " << t.importPart << " " << t.importKey << "\n";
        for (const auto& h : t.tabHints) // 타브 운지 힌트 (악보가 지정한 줄)
            os << "thint " << h.tick << " " << (int)h.note << " " << (int)h.strIdx << "\n";
        for (const auto& ap : t.volAuto) os << "tautov " << ap.tick << " " << ap.value << "\n";
        for (const auto& ap : t.panAuto) os << "tautop " << ap.tick << " " << ap.value << "\n";
        for (const auto& mc : t.midiClips) { // MIDI 클립 구간 (이름은 줄 끝까지)
            os << "tmclip " << mc.startTick << " " << mc.endTick << " " << mc.name << "\n";
            for (const auto& m : mc.members) // 소유 노트 키 (직전 tmclip 소속)
                os << "tmmember " << (int)m.first << " " << m.second << "\n";
        }
        // 경로에 공백이 있을 수 있어 path를 줄 끝에 두고, 이름은 그 앞에 탭으로 구분.
        for (const auto& pl : t.plugins)
            os << "tplugin " << (pl.isInstrument ? 1 : 0) << " " << (pl.enabled ? 1 : 0) << " "
               << pl.classIndex << " " << pl.name << "\t" << pl.path << "\n";
        for (std::size_t j = 0; j < t.clips.size(); ++j) {
            if (!t.clips[j]) continue;
            const auto& c = *t.clips[j];
            os << "taudio " << audioFileName((int)i, (int)j) << " " << c.startTick << " "
               << c.speed << " " << c.trimStart << " " << c.trimLen << " " << c.name << "\n";
            // 페이드는 별도 줄 (이름이 줄 끝이라 taudio에 못 붙인다). 직전 taudio에 적용.
            if (c.fadeInSec > 0.0 || c.fadeOutSec > 0.0)
                os << "tfade " << c.fadeInSec << " " << c.fadeOutSec << "\n";
            if (c.gain != 1.0f) os << "tgain " << c.gain << "\n"; // 직전 taudio에 적용
            if (c.freezeBounce) os << "tbounce\n"; // 프리즈로 구운 클립 표시
        }
        for (const auto& e : t.events)
            os << "ev " << e.tick << " " << (int)e.status << " " << (int)e.data1 << " "
               << (int)e.data2 << "\n";
    }
    return os.str();
}

// 텍스트 -> 곡. 오디오는 참조만 모으고 실제 WAV는 load()가 읽는다.
void songFromText(const std::string& text, seq::Song& song, std::vector<AudioRef>& refs) {
    song = seq::Song{};
    refs.clear();
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
        } else if (key == "master") {
            ls >> song.masterVolume >> song.masterPan >> song.masterGain;
        } else if (key == "tempo") {
            seq::TempoChange tc;
            ls >> tc.tick >> tc.bpm;
            int rp = 0;
            if (ls >> rp) tc.ramp = rp != 0; // 옛 파일엔 없다 (기본 false)
            song.tempoChanges.push_back(tc);
        } else if (key == "marker") {
            seq::SectionMarker mk;
            ls >> mk.tick;
            std::getline(ls, mk.name);
            if (!mk.name.empty() && mk.name[0] == ' ') mk.name.erase(0, 1);
            song.markers.push_back(std::move(mk));
        } else if (key == "tvol" && cur) {
            ls >> cur->volume >> cur->pan;
            float g;
            if (ls >> g) cur->gain = g; // 옛 파일엔 없을 수 있음 (기본 1.0)
            float snd;
            if (ls >> snd) cur->sendLevel = snd; // 센드 (옛 파일엔 없음, 기본 0)
        } else if (key == "tinch" && cur) {
            ls >> cur->inputChannelMode;
        } else if (key == "tfrz" && cur) {
            int fz = 0;
            ls >> fz;
            cur->frozen = fz != 0;
        } else if (key == "tgtr" && cur) {
            int g = 0;
            ls >> g;
            cur->isGuitar = g != 0;
        } else if (key == "timp" && cur) {
            ls >> cur->importPart;
            std::getline(ls, cur->importKey);
            while (!cur->importKey.empty() && cur->importKey.front() == ' ')
                cur->importKey.erase(cur->importKey.begin());
        } else if (key == "thint" && cur) {
            uint32_t tick = 0;
            int note = 0, s = 0;
            ls >> tick >> note >> s;
            if (note >= 0 && note <= 127 && s >= 0 && s <= 5)
                cur->tabHints.push_back({tick, (uint8_t)note, (uint8_t)s});
        } else if (key == "tautov" && cur) {
            seq::Track::AutoPoint p;
            ls >> p.tick >> p.value;
            cur->volAuto.push_back(p);
        } else if (key == "tautop" && cur) {
            seq::Track::AutoPoint p;
            ls >> p.tick >> p.value;
            cur->panAuto.push_back(p);
        } else if (key == "tmclip" && cur) {
            seq::MidiClip mc;
            ls >> mc.startTick >> mc.endTick;
            std::string nm;
            std::getline(ls, nm);
            if (!nm.empty() && nm[0] == ' ') nm.erase(0, 1);
            if (!nm.empty()) mc.name = nm;
            if (mc.endTick > mc.startTick) cur->midiClips.push_back(mc);
        } else if (key == "tmmember" && cur && !cur->midiClips.empty()) {
            int mn = -1;
            uint32_t mt = 0;
            ls >> mn >> mt;
            if (mn >= 0 && mn <= 127)
                cur->midiClips.back().members.push_back({(uint8_t)mn, mt});
        } else if (key == "tplugin" && cur) {
            int isInst = 0, enabled = 1, classIndex = 0;
            ls >> isInst >> enabled >> classIndex;
            std::string rest;
            std::getline(ls, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            // "이름\t경로" (경로에 공백 허용). 탭이 없으면 옛 형식이라 전체가 이름.
            std::string name = rest, path;
            const auto tab = rest.find('\t');
            if (tab != std::string::npos) {
                name = rest.substr(0, tab);
                path = rest.substr(tab + 1);
            }
            seq::TrackPlugin pl;
            pl.name = name;
            pl.path = path;
            pl.classIndex = classIndex;
            pl.isInstrument = isInst != 0;
            pl.enabled = enabled != 0;
            cur->plugins.push_back(std::move(pl));
        } else if (key == "taudio" && cur) {
            AudioRef r;
            r.trackIndex = (int)song.tracks.size() - 1;
            ls >> r.file >> r.startTick >> r.speed >> r.trimStart >> r.trimLen;
            std::string name;
            std::getline(ls, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1);
            r.clipName = name;
            refs.push_back(std::move(r));
        } else if (key == "tfade" && !refs.empty()) {
            ls >> refs.back().fadeInSec >> refs.back().fadeOutSec;
        } else if (key == "tgain" && !refs.empty()) {
            ls >> refs.back().gain;
        } else if (key == "tbounce" && !refs.empty()) {
            refs.back().freezeBounce = true;
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


// 버전 노드의 곡 -> 텍스트. songToText와 같지만 클립 "배치"를 places에서,
// 파일명을 레지스트리에서 가져온다. 클립 객체의 배치 필드는 여러 버전이
// 공유하는 라이브 값이라 그대로 쓰면 모든 버전이 같은 배치가 돼 버린다.
std::string versionSongToText(const VersionSnap& v,
                              const std::map<const audio::AudioClip*, std::string>& names) {
    const seq::Song& song = v.song;
    std::ostringstream os;
    os << "bpm " << song.bpm << "\n";
    os << "ppqn " << song.ppqn << "\n";
    os << "master " << song.masterVolume << " " << song.masterPan << " " << song.masterGain
       << "\n";
    for (const auto& tc : song.tempoChanges)
        os << "tempo " << tc.tick << " " << tc.bpm << " " << (tc.ramp ? 1 : 0) << "\n";
    for (const auto& mk : song.markers)
        os << "marker " << mk.tick << " " << mk.name << "\n";
    std::size_t k = 0;
    for (const auto& t : song.tracks) {
        os << "track " << (int)t.channel << " " << (t.muted ? 1 : 0) << " " << t.name << "\n";
        os << "tvol " << t.volume << " " << t.pan << " " << t.gain << " " << t.sendLevel
           << "\n"; // 4번째 값(센드)은 옛 파일엔 없다 (읽기 실패 시 0)
        os << "tinch " << t.inputChannelMode << "\n";
        if (t.frozen) os << "tfrz 1\n";
        if (t.isGuitar) os << "tgtr 1\n";
        if (!t.importKey.empty()) os << "timp " << t.importPart << " " << t.importKey << "\n";
        for (const auto& h : t.tabHints)
            os << "thint " << h.tick << " " << (int)h.note << " " << (int)h.strIdx << "\n";
        for (const auto& ap : t.volAuto) os << "tautov " << ap.tick << " " << ap.value << "\n";
        for (const auto& ap : t.panAuto) os << "tautop " << ap.tick << " " << ap.value << "\n";
        for (const auto& mc : t.midiClips) { // MIDI 클립 구간 (이름은 줄 끝까지)
            os << "tmclip " << mc.startTick << " " << mc.endTick << " " << mc.name << "\n";
            for (const auto& m : mc.members) // 소유 노트 키 (직전 tmclip 소속)
                os << "tmmember " << (int)m.first << " " << m.second << "\n";
        }
        for (const auto& pl : t.plugins)
            os << "tplugin " << (pl.isInstrument ? 1 : 0) << " " << (pl.enabled ? 1 : 0) << " "
               << pl.classIndex << " " << pl.name << "\t" << pl.path << "\n";
        for (const auto& cp : t.clips) {
            if (!cp) continue;
            // 배치: places[k]가 원본. (혹시 부족하면 클립 필드로 대체)
            ClipPlace pl;
            if (k < v.places.size()) pl = v.places[k];
            else pl = {cp->startTick, cp->speed,     cp->trimStart, cp->trimLen,
                       cp->fadeInSec, cp->fadeOutSec, cp->gain};
            ++k;
            const auto it = names.find(cp.get());
            os << "taudio " << (it != names.end() ? it->second : std::string("missing.wav"))
               << " " << pl.startTick << " " << pl.speed << " " << pl.trimStart << " "
               << pl.trimLen << " " << cp->name << "\n";
            os << "tfade " << pl.fadeInSec << " " << pl.fadeOutSec << "\n";
            os << "tgain " << pl.gain << "\n";
            if (cp->freezeBounce) os << "tbounce\n";
        }
        for (const auto& e : t.events)
            os << "ev " << e.tick << " " << (int)e.status << " " << (int)e.data1 << " "
               << (int)e.data2 << "\n";
    }
    return os.str();
}

// [versions] 섹션 텍스트 -> 버전 목록. vbegin~vend 사이의 곡 라인은
// songFromText를 그대로 재사용한다.
void versionsFromText(const std::string& text, ProjectData& p) {
    std::istringstream is(text);
    std::string line, buf;
    VersionSnap cur;
    bool inVer = false;
    auto finish = [&]() {
        if (!inVer) return;
        songFromText(buf, cur.song, cur.refs);
        cur.places.clear();
        for (const auto& r : cur.refs)
            cur.places.push_back({r.startTick, r.speed > 0.0 ? r.speed : 1.0, r.trimStart,
                                  r.trimLen, r.fadeInSec, r.fadeOutSec,
                                  r.gain > 0.0f ? r.gain : 1.0f});
        p.versions.push_back(std::move(cur));
        inVer = false;
    };
    while (std::getline(is, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "vercur") {
            ls >> p.versionCurrent >> p.versionNextId;
        } else if (key == "vnote" && inVer) {
            std::string nm;
            std::getline(ls, nm);
            if (!nm.empty() && nm[0] == ' ') nm.erase(0, 1);
            cur.note = nm;
        } else if (key == "vbegin") {
            finish(); // 안전망: vend가 빠졌어도 직전 버전을 닫는다
            cur = VersionSnap{};
            ls >> cur.id >> cur.parent;
            std::string nm;
            std::getline(ls, nm);
            if (!nm.empty() && nm[0] == ' ') nm.erase(0, 1);
            cur.name = nm;
            buf.clear();
            inVer = true;
        } else if (key == "vend") {
            finish();
        } else if (inVer) {
            buf += line;
            buf += "\n";
        }
    }
    finish();
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

// <프로젝트경로>.audio/ 사이드카 폴더 경로 (오디오 WAV + VST 상태 파일)
std::filesystem::path sidecarDir(const std::filesystem::path& projectPath) {
    std::filesystem::path d = projectPath;
    d.replace_extension();
    d += ".audio";
    return d;
}

std::string serialize(const ProjectData& p) {
    std::ostringstream os;
    os << "midipro_project 1\n";
    os << "mpe " << (p.mpe ? 1 : 0) << "\n";
    os << "vstinst " << (p.vstInstrumentPath.empty() ? "-" : p.vstInstrumentPath) << "\n";
    os << "vstinstclass " << p.vstInstrumentClass << "\n";
    os << "vstinsttrack " << p.vstInstrumentTrack << "\n";
    os << "vstfx " << (p.vstEffectPath.empty() ? "-" : p.vstEffectPath) << "\n";
    os << "vstfxclass " << p.vstEffectClass << "\n";
    os << "metroclick " << p.metroClickNote << "\n";
    os << "countinclick " << p.countInClickNote << "\n";
    os << "metrosample " << (p.metroSamplePath.empty() ? "-" : p.metroSamplePath) << "\n";
    os << "countinsample " << (p.countInSamplePath.empty() ? "-" : p.countInSamplePath) << "\n";
    os << "accentclick " << p.accentClickNote << "\n";
    os << "accentsample " << (p.accentSamplePath.empty() ? "-" : p.accentSamplePath) << "\n";
    os << "ciaccentclick " << p.countInAccentClickNote << "\n";
    os << "ciaccentsample "
       << (p.countInAccentSamplePath.empty() ? "-" : p.countInAccentSamplePath) << "\n";
    os << "metrosig " << p.metroSigIndex << "\n";
    os << "countinbeats " << p.countInBeats << "\n";
    os << "metrovol " << p.metroVolume << "\n";
    os << "countinvol " << p.countInVolume << "\n";
    os << "mlimiter " << (p.masterLimiterOn ? 1 : 0) << " " << p.limiterGainDb << " "
       << p.limiterCeilDb << " " << p.limiterReleaseMs << "\n";
    os << "mreturn " << p.returnLevel << " " << p.returnRoom << " " << p.returnDamp << "\n";
    for (const auto& ds : p.drumSamples) // 경로에 공백 허용 (줄 끝까지)
        os << "drumsample " << ds.first << " " << ds.second << "\n";
    // 섹션: 각 모듈의 기존 직렬화기 결과를 그대로 넣는다
    os << "[song]\n" << songToText(p.song);
    os << "[synth]\n" << audio::serialize(p.synth);
    os << "[midimap]\n" << p.midiMap.serialize();
    // 버전 분기 트리 (없으면 섹션 자체를 안 써서 옛 파일과 동일한 출력)
    if (!p.versions.empty()) {
        const auto names = buildClipNames(p);
        os << "[versions]\n";
        os << "vercur " << p.versionCurrent << " " << p.versionNextId << "\n";
        for (const auto& v : p.versions) {
            os << "vbegin " << v.id << " " << v.parent << " " << v.name << "\n";
            if (!v.note.empty()) {
                // 메모는 한 줄로 저장한다 (줄바꿈이 섞이면 공백으로)
                std::string one = v.note;
                for (auto& ch : one)
                    if (ch == '\n' || ch == '\r') ch = ' ';
                os << "vnote " << one << "\n";
            }
            os << versionSongToText(v, names);
            os << "vend\n";
        }
    }
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
            songFromText(sectionBuf, p.song, p.audioRefs);
        else if (section == "synth")
            audio::deserialize(p.synth, sectionBuf);
        else if (section == "midimap")
            p.midiMap.deserialize(sectionBuf);
        else if (section == "versions")
            versionsFromText(sectionBuf, p);
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
            else if (key == "vstinsttrack")
                p.vstInstrumentTrack = std::atoi(value.c_str());
            else if (key == "vstfx")
                p.vstEffectPath = (value == "-") ? std::string() : value;
            else if (key == "vstfxclass")
                p.vstEffectClass = std::atoi(value.c_str());
            else if (key == "metroclick")
                p.metroClickNote = std::atoi(value.c_str());
            else if (key == "countinclick")
                p.countInClickNote = std::atoi(value.c_str());
            else if (key == "metrosample")
                p.metroSamplePath = (value == "-") ? std::string() : value;
            else if (key == "countinsample")
                p.countInSamplePath = (value == "-") ? std::string() : value;
            else if (key == "accentclick")
                p.accentClickNote = std::atoi(value.c_str());
            else if (key == "accentsample")
                p.accentSamplePath = (value == "-") ? std::string() : value;
            else if (key == "metrosig")
                p.metroSigIndex = std::atoi(value.c_str());
            else if (key == "ciaccentclick")
                p.countInAccentClickNote = std::atoi(value.c_str());
            else if (key == "ciaccentsample")
                p.countInAccentSamplePath = (value == "-") ? std::string() : value;
            else if (key == "countinbeats")
                p.countInBeats = std::atoi(value.c_str());
            else if (key == "metrovol")
                p.metroVolume = (float)std::atof(value.c_str());
            else if (key == "countinvol")
                p.countInVolume = (float)std::atof(value.c_str());
            else if (key == "mlimiter") {
                std::istringstream ls2(value);
                int on = 1;
                ls2 >> on >> p.limiterGainDb >> p.limiterCeilDb >> p.limiterReleaseMs;
                p.masterLimiterOn = on != 0;
            } else if (key == "mreturn") {
                std::istringstream ls2(value);
                ls2 >> p.returnLevel >> p.returnRoom >> p.returnDamp;
            } else if (key == "drumsample") {
                // "노트번호 경로..." (경로에 공백 허용)
                std::istringstream ls2(value);
                int dn = -1;
                ls2 >> dn;
                std::string dp;
                std::getline(ls2, dp);
                if (!dp.empty() && dp[0] == ' ') dp.erase(0, 1);
                if (dn >= 0 && dn <= 127 && !dp.empty()) p.drumSamples.push_back({dn, dp});
            }
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
    if (!out.good()) return false;
    out.close();

    // 오디오 클립(라이브 + 버전 전용)을 사이드카 폴더에 WAV로 저장한다.
    // 같은 클립 객체를 여러 버전이 공유하면 파일도 하나만 쓴다.
    const auto names = buildClipNames(p);
    if (names.empty()) return true; // 클립이 없으면 폴더도 안 만든다

    std::error_code ec;
    const std::filesystem::path dir = sidecarDir(path);
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    for (const auto& entry : names)
        if (!audio::writeWavFile(*entry.first, dir / entry.second)) return false;
    return true;
}

bool load(ProjectData& out, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!deserialize(out, ss.str())) return false;

    // 사이드카 WAV를 읽어 트랙에 붙이고, 배치/트림/속도를 복원한다.
    // 같은 파일은 한 번만 디코드해 공유한다 (버전들이 클립을 공유하는 구조 유지).
    const std::filesystem::path dir = sidecarDir(path);
    std::map<std::string, std::shared_ptr<audio::AudioClip>> cache;
    auto getClip = [&](const AudioRef& r) -> std::shared_ptr<audio::AudioClip> {
        const auto it = cache.find(r.file);
        if (it != cache.end()) return it->second;
        auto c = audio::readWavFile(dir / r.file, r.clipName);
        if (c) cache[r.file] = c;
        return c;
    };

    // 버전 클립부터 붙인다. 못 읽은 클립은 배치(places)도 함께 빼서
    // 곡의 클립 순서와 배치 배열이 어긋나지 않게 한다.
    for (auto& v : out.versions) {
        std::vector<ClipPlace> kept;
        kept.reserve(v.places.size());
        for (std::size_t k = 0; k < v.refs.size(); ++k) {
            const auto& r = v.refs[k];
            if (r.trackIndex < 0 || r.trackIndex >= (int)v.song.tracks.size()) continue;
            const bool fresh = cache.find(r.file) == cache.end();
            auto clip = getClip(r);
            if (!clip) continue;
            if (fresh) {
                // 버전에만 있는 클립: 객체 배치 필드를 일단 이 버전 값으로.
                // (라이브 곡에도 있으면 아래 라이브 루프가 덮어쓴다)
                clip->freezeBounce = r.freezeBounce;
                clip->startTick = r.startTick;
                clip->speed = r.speed > 0.0 ? r.speed : 1.0;
                clip->trimStart = r.trimStart;
                clip->trimLen = r.trimLen > 0 ? r.trimLen : (int64_t)clip->frames();
                clip->fadeInSec = r.fadeInSec;
                clip->fadeOutSec = r.fadeOutSec;
                clip->gain = r.gain > 0.0f ? r.gain : 1.0f;
            }
            v.song.tracks[(std::size_t)r.trackIndex].clips.push_back(clip);
            if (k < v.places.size()) kept.push_back(v.places[k]);
        }
        v.places = std::move(kept);
    }

    // 라이브 곡을 마지막에 붙여, 공유 클립 객체의 배치 필드가 라이브 값으로 남게 한다.
    for (const auto& r : out.audioRefs) {
        if (r.trackIndex < 0 || r.trackIndex >= (int)out.song.tracks.size()) continue;
        auto clip = getClip(r);
        if (!clip) continue; // 오디오가 없어도 곡은 열리게 한다
        clip->startTick = r.startTick;
        clip->speed = r.speed > 0.0 ? r.speed : 1.0;
        clip->trimStart = r.trimStart;
        clip->trimLen = r.trimLen > 0 ? r.trimLen : (int64_t)clip->frames();
        clip->fadeInSec = r.fadeInSec;
        clip->fadeOutSec = r.fadeOutSec;
        clip->gain = r.gain > 0.0f ? r.gain : 1.0f;
        clip->freezeBounce = r.freezeBounce;
        // taudio 라인 순서대로 추가된다 (트랙당 여러 클립)
        out.song.tracks[(std::size_t)r.trackIndex].clips.push_back(clip);
    }
    return true;
}

} // namespace midipro::project
