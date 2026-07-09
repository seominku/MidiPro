// =============================================================
// MidiPro - mapping/MidiMap.cpp
// =============================================================

#include "mapping/MidiMap.h"

#include <fstream>
#include <sstream>

namespace midipro::mapping {

namespace {
// ParamTarget 순서와 반드시 일치해야 한다 (인덱스로 조회).
const ParamInfo kParamInfos[(int)ParamTarget::Count] = {
    {"cutoff", "필터 Cutoff", 50.0f, 12000.0f},
    {"resonance", "필터 Resonance", 0.0f, 1.0f},
    {"attack", "Attack", 0.001f, 2.0f},
    {"decay", "Decay", 0.001f, 2.0f},
    {"sustain", "Sustain", 0.0f, 1.0f},
    {"release", "Release", 0.001f, 3.0f},
    {"lfoRate", "LFO Rate", 0.1f, 20.0f},
    {"lfoDepth", "LFO Depth", 0.0f, 1.0f},
    {"delayMix", "딜레이 Mix", 0.0f, 1.0f},
    {"masterVolume", "마스터 볼륨", 0.0f, 1.0f},
};

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
} // namespace

const ParamInfo& paramInfo(ParamTarget target) {
    return kParamInfos[(int)target];
}

void applyNormalized(audio::SynthParams& p, ParamTarget target, float norm01) {
    const ParamInfo& info = paramInfo(target);
    const float v = info.min + (info.max - info.min) * clamp01(norm01);
    switch (target) {
    case ParamTarget::FilterCutoff:    p.filterCutoff = v; break;
    case ParamTarget::FilterResonance: p.filterResonance = v; break;
    case ParamTarget::Attack:          p.adsr.attackSec = v; break;
    case ParamTarget::Decay:           p.adsr.decaySec = v; break;
    case ParamTarget::Sustain:         p.adsr.sustain = v; break;
    case ParamTarget::Release:         p.adsr.releaseSec = v; break;
    case ParamTarget::LfoRate:         p.lfoRateHz = v; break;
    case ParamTarget::LfoDepth:        p.lfoDepth = v; break;
    case ParamTarget::DelayMix:        p.delayMix = v; break;
    case ParamTarget::MasterVolume:    p.masterVolume = v; break;
    default: break;
    }
}

float readNormalized(const audio::SynthParams& p, ParamTarget target) {
    const ParamInfo& info = paramInfo(target);
    float v = 0.0f;
    switch (target) {
    case ParamTarget::FilterCutoff:    v = p.filterCutoff; break;
    case ParamTarget::FilterResonance: v = p.filterResonance; break;
    case ParamTarget::Attack:          v = p.adsr.attackSec; break;
    case ParamTarget::Decay:           v = p.adsr.decaySec; break;
    case ParamTarget::Sustain:         v = p.adsr.sustain; break;
    case ParamTarget::Release:         v = p.adsr.releaseSec; break;
    case ParamTarget::LfoRate:         v = p.lfoRateHz; break;
    case ParamTarget::LfoDepth:        v = p.lfoDepth; break;
    case ParamTarget::DelayMix:        v = p.delayMix; break;
    case ParamTarget::MasterVolume:    v = p.masterVolume; break;
    default: break;
    }
    const float range = info.max - info.min;
    return range > 0.0f ? clamp01((v - info.min) / range) : 0.0f;
}

void MidiMap::bind(uint8_t cc, int channel, ParamTarget target) {
    // 같은 cc 또는 같은 target을 쓰던 기존 매핑 제거 (1:1 유지)
    for (auto it = m_mappings.begin(); it != m_mappings.end();) {
        if (it->cc == cc || it->target == target)
            it = m_mappings.erase(it);
        else
            ++it;
    }
    m_mappings.push_back({cc, channel, target});
}

void MidiMap::clearTarget(ParamTarget target) {
    for (auto it = m_mappings.begin(); it != m_mappings.end();) {
        if (it->target == target)
            it = m_mappings.erase(it);
        else
            ++it;
    }
}

bool MidiMap::findTarget(uint8_t cc, ParamTarget& out) const {
    for (const auto& m : m_mappings) {
        if (m.cc == cc) {
            out = m.target;
            return true;
        }
    }
    return false;
}

int MidiMap::ccForTarget(ParamTarget target) const {
    for (const auto& m : m_mappings)
        if (m.target == target) return m.cc;
    return -1;
}

int MidiMap::channelForTarget(ParamTarget target) const {
    for (const auto& m : m_mappings)
        if (m.target == target) return m.channel;
    return -1;
}

std::string MidiMap::serialize() const {
    std::ostringstream os;
    os << "midipro_midimap 1\n";
    for (const auto& m : m_mappings)
        os << "map " << (int)m.cc << " " << m.channel << " " << paramInfo(m.target).id << "\n";
    return os.str();
}

bool MidiMap::deserialize(const std::string& text) {
    std::istringstream is(text);
    std::string token;
    if (!(is >> token) || token != "midipro_midimap") return false;
    int version = 0;
    is >> version;

    std::vector<CcMapping> parsed;
    while (is >> token) {
        if (token != "map") continue;
        int cc = 0, channel = 0;
        std::string id;
        if (!(is >> cc >> channel >> id)) return false;
        // id -> ParamTarget
        bool matched = false;
        for (int t = 0; t < (int)ParamTarget::Count; ++t) {
            if (id == kParamInfos[t].id) {
                parsed.push_back({(uint8_t)cc, channel, (ParamTarget)t});
                matched = true;
                break;
            }
        }
        if (!matched) continue; // 모르는 파라미터는 건너뜀 (포맷 확장 관용)
    }
    m_mappings = std::move(parsed);
    return true;
}

bool MidiMap::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::string text = serialize();
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

bool MidiMap::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return deserialize(ss.str());
}

} // namespace midipro::mapping
