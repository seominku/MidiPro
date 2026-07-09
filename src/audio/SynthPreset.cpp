// =============================================================
// MidiPro - audio/SynthPreset.cpp
// =============================================================

#include "audio/SynthPreset.h"

#include <fstream>
#include <sstream>

namespace midipro::audio {

const std::vector<NamedPreset>& builtinPresets() {
    static const std::vector<NamedPreset> presets = [] {
        std::vector<NamedPreset> list;

        // 기본값 (톱니 리드성)
        {
            NamedPreset p;
            p.name = "기본 (Saw)";
            list.push_back(p);
        }
        // 부드러운 패드
        {
            NamedPreset p;
            p.name = "부드러운 패드";
            p.params.waveform = Waveform::Triangle;
            p.params.adsr = {0.4f, 0.5f, 0.8f, 0.8f};
            p.params.filterCutoff = 2500.0f;
            p.params.lfoRateHz = 0.6f;
            p.params.lfoDepth = 0.3f;
            p.params.delayMix = 0.35f;
            p.params.masterVolume = 0.28f;
            list.push_back(p);
        }
        // 리드
        {
            NamedPreset p;
            p.name = "리드";
            p.params.waveform = Waveform::Square;
            p.params.adsr = {0.005f, 0.12f, 0.6f, 0.15f};
            p.params.filterCutoff = 6000.0f;
            p.params.filterResonance = 0.35f;
            p.params.delayMix = 0.18f;
            list.push_back(p);
        }
        // 베이스
        {
            NamedPreset p;
            p.name = "베이스";
            p.params.waveform = Waveform::Saw;
            p.params.adsr = {0.005f, 0.15f, 0.4f, 0.1f};
            p.params.filterCutoff = 1200.0f;
            p.params.filterResonance = 0.25f;
            p.params.lfoDepth = 0.0f;
            p.params.delayMix = 0.0f;
            p.params.masterVolume = 0.35f;
            list.push_back(p);
        }
        // 기타풍 플럭
        {
            NamedPreset p;
            p.name = "기타풍 플럭";
            p.params.waveform = Waveform::Triangle;
            p.params.adsr = {0.002f, 0.35f, 0.0f, 0.25f}; // 빠른 감쇠(sustain 0)
            p.params.filterCutoff = 3500.0f;
            p.params.filterResonance = 0.15f;
            p.params.delayMix = 0.12f;
            list.push_back(p);
        }
        return list;
    }();
    return presets;
}

std::string serialize(const SynthParams& p) {
    std::ostringstream os;
    os << "midipro_synth_preset 1\n";
    os << "waveform " << (int)p.waveform << "\n";
    os << "attack " << p.adsr.attackSec << "\n";
    os << "decay " << p.adsr.decaySec << "\n";
    os << "sustain " << p.adsr.sustain << "\n";
    os << "release " << p.adsr.releaseSec << "\n";
    os << "cutoff " << p.filterCutoff << "\n";
    os << "resonance " << p.filterResonance << "\n";
    os << "lfoRate " << p.lfoRateHz << "\n";
    os << "lfoDepth " << p.lfoDepth << "\n";
    os << "delayTime " << p.delayTimeSec << "\n";
    os << "delayFeedback " << p.delayFeedback << "\n";
    os << "delayMix " << p.delayMix << "\n";
    os << "masterVolume " << p.masterVolume << "\n";
    return os.str();
}

bool deserialize(SynthParams& out, const std::string& text) {
    std::istringstream is(text);
    std::string key;
    bool sawHeader = false;
    SynthParams p; // 기본값에서 시작해 나온 키만 덮어쓴다
    while (is >> key) {
        if (key == "midipro_synth_preset") {
            int version = 0;
            is >> version;
            sawHeader = true;
        } else if (key == "waveform") {
            int w = 0;
            is >> w;
            if (w < 0 || w > 3) return false;
            p.waveform = (Waveform)w;
        } else if (key == "attack") {
            is >> p.adsr.attackSec;
        } else if (key == "decay") {
            is >> p.adsr.decaySec;
        } else if (key == "sustain") {
            is >> p.adsr.sustain;
        } else if (key == "release") {
            is >> p.adsr.releaseSec;
        } else if (key == "cutoff") {
            is >> p.filterCutoff;
        } else if (key == "resonance") {
            is >> p.filterResonance;
        } else if (key == "lfoRate") {
            is >> p.lfoRateHz;
        } else if (key == "lfoDepth") {
            is >> p.lfoDepth;
        } else if (key == "delayTime") {
            is >> p.delayTimeSec;
        } else if (key == "delayFeedback") {
            is >> p.delayFeedback;
        } else if (key == "delayMix") {
            is >> p.delayMix;
        } else if (key == "masterVolume") {
            is >> p.masterVolume;
        } else {
            // 알 수 없는 키는 그 값을 건너뛴다 (포맷 확장에 관대하게)
            std::string ignore;
            is >> ignore;
        }
    }
    if (!sawHeader) return false; // 우리 포맷이 아님
    out = p;
    return true;
}

bool savePreset(const SynthParams& params, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::string text = serialize(params);
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

bool loadPreset(SynthParams& out, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return deserialize(out, ss.str());
}

} // namespace midipro::audio
