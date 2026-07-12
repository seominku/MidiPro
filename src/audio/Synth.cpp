// =============================================================
// MidiPro - audio/Synth.cpp
// =============================================================

#include "audio/Synth.h"

namespace midipro::audio {

void Synth::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    for (auto& v : m_voices) {
        v.active = false;
        v.osc.setSampleRate(sampleRate);
        v.env.setSampleRate(sampleRate);
        v.filter.setSampleRate(sampleRate);
        v.filter.reset();
    }
    m_lfo.setSampleRate(sampleRate);
    m_lfo.setWaveform(Waveform::Sine);
    m_lfo.setFrequency(m_params.lfoRateHz);
    m_delayL.prepare(sampleRate, /*maxDelaySec=*/1.0);
    m_delayR.prepare(sampleRate, /*maxDelaySec=*/1.0);
    for (int ch = 0; ch < kChannels; ++ch) {
        m_busDelayL[ch].prepare(sampleRate, /*maxDelaySec=*/1.0);
        m_busDelayR[ch].prepare(sampleRate, /*maxDelaySec=*/1.0);
    }
    m_chGain.fill(1.0f);
    m_chPan.fill(0.0f);
}

void Synth::setParams(const SynthParams& params) {
    m_params = params;
    m_lfo.setFrequency(params.lfoRateHz);
    m_delayL.setParams(params.delayTimeSec, params.delayFeedback, params.delayMix);
    m_delayR.setParams(params.delayTimeSec, params.delayFeedback, params.delayMix);
    for (int ch = 0; ch < kChannels; ++ch) {
        m_busDelayL[ch].setParams(params.delayTimeSec, params.delayFeedback, params.delayMix);
        m_busDelayR[ch].setParams(params.delayTimeSec, params.delayFeedback, params.delayMix);
    }
    for (auto& v : m_voices) {
        if (v.active) updateVoiceTimbre(v);
    }
}

void Synth::setChannelMix(uint8_t channel, float gain, float pan) {
    const int ch = channel & 0x0F;
    m_chGain[ch] = gain < 0.0f ? 0.0f : gain;
    m_chPan[ch] = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
}

void Synth::updateVoiceTimbre(Voice& v) {
    v.osc.setWaveform(m_params.waveform);
    v.env.setParams(m_params.adsr);
}

Synth::Voice* Synth::findFreeVoice() {
    // 1순위: 비활성 보이스
    for (auto& v : m_voices)
        if (!v.active) return &v;

    // 2순위(스틸링): 가장 먼저 시작된 보이스를 뺏는다.
    // 왜: 폴리포니 한계를 넘으면 가장 오래된 음이 가장 덜 중요하다는
    //     관례적 휴리스틱.
    Voice* oldest = &m_voices[0];
    for (auto& v : m_voices)
        if (v.startOrder < oldest->startOrder) oldest = &v;
    return oldest;
}

void Synth::applyVoiceFrequency(Voice& v) {
    if (v.drum) return; // 드럼은 전용 생성기가 주파수를 관리한다
    // 채널 벤드 + 노트별 벤드를 합산해 반음 -> 주파수 배수: 2^(semis/12)
    const double semis = ((double)v.bendNorm + (double)v.perNoteBendNorm) * (double)m_bendRangeSemis;
    const double hz = Oscillator::noteToHz(v.note) * std::pow(2.0, semis / 12.0);
    v.osc.setFrequency(hz);
}

// ---- 내장 드럼 (채널 10, GM 드럼 맵 일부) ----
// 톤(사인 + 피치 드롭)과 노이즈(하이패스)를 섞어 합성하는 원샷 퍼커션.
// 표에 없는 노트(메트로놈 클릭 77/84/88/91 등)는 기존 신스 음으로 소리낸다.
namespace {
struct DrumSpec {
    uint8_t note;
    float toneStart, toneEnd; // Hz (0 = 톤 없음)
    float pitchDecaySec;      // 피치 드롭 속도
    float ampDecaySec;        // 소리 길이
    float noiseMix;           // 0=톤만, 1=노이즈만
    float noiseHp;            // 노이즈 하이패스 (1=밝은 심벌)
    float gain;
};
constexpr DrumSpec kDrumSpecs[] = {
    {36, 160.0f, 45.0f, 0.045f, 0.22f, 0.06f, 0.0f, 1.25f},  // 킥: 깊은 펀치
    {38, 195.0f, 165.0f, 0.03f, 0.16f, 0.68f, 0.35f, 0.95f}, // 스네어: 톤+노이즈
    {39, 0.0f, 0.0f, 0.05f, 0.14f, 1.0f, 0.45f, 0.85f},      // 클랩: 노이즈 버스트
    {42, 0.0f, 0.0f, 0.05f, 0.045f, 1.0f, 0.92f, 0.65f},     // 클로즈드 햇: 짧고 밝게
    {46, 0.0f, 0.0f, 0.05f, 0.32f, 1.0f, 0.88f, 0.6f},       // 오픈 햇
    {45, 150.0f, 85.0f, 0.08f, 0.3f, 0.08f, 0.0f, 1.0f},     // 로우 탐
    {47, 190.0f, 110.0f, 0.075f, 0.27f, 0.08f, 0.0f, 1.0f},  // 미드 탐
    {50, 235.0f, 140.0f, 0.07f, 0.24f, 0.08f, 0.0f, 1.0f},   // 하이 탐
    {49, 0.0f, 0.0f, 0.05f, 1.1f, 1.0f, 0.7f, 0.7f},         // 크래시: 길게 밝게
    {51, 0.0f, 0.0f, 0.05f, 0.7f, 1.0f, 0.8f, 0.45f},        // 라이드: 은은하게
};
const DrumSpec* drumSpecFor(uint8_t note) {
    for (const auto& d : kDrumSpecs)
        if (d.note == note) return &d;
    return nullptr;
}
} // namespace

void Synth::noteOnFloat(uint8_t channel, uint8_t note, float velocity01) {
    Voice* v = findFreeVoice();
    v->active = true;
    v->channel = channel;
    v->note = note;
    v->velocityGain = velocity01 < 0.0f ? 0.0f : (velocity01 > 1.0f ? 1.0f : velocity01);
    v->bendNorm = 0.0f;
    v->perNoteBendNorm = 0.0f;
    v->pressure = 0.0f;
    v->timbre = 0.5f;
    v->startOrder = ++m_orderCounter;

    // 채널 10 + 드럼 맵 노트 = 전용 퍼커션 보이스
    const DrumSpec* spec = (channel & 0x0F) == 9 ? drumSpecFor(note) : nullptr;
    if (spec) {
        v->drum = true;
        // 드럼은 벨로시티 제곱 커브: 강세(127)와 일반(100)의 차이가 또렷해진다
        v->velocityGain *= v->velocityGain;
        v->dPhase = 0.0f;
        v->dFreq = spec->toneStart;
        v->dFreqEnd = spec->toneEnd;
        v->dPitchCoef = (float)std::exp(-1.0 / (spec->pitchDecaySec * m_sampleRate));
        v->dAmp = 1.0f;
        v->dAmpCoef = (float)std::exp(-1.0 / (spec->ampDecaySec * m_sampleRate));
        v->dNoiseMix = spec->noiseMix;
        v->dHpAmt = spec->noiseHp;
        v->dLp = 0.0f;
        v->dGain = spec->gain;
        v->dRng = (uint32_t)(m_orderCounter * 2654435761u + 12345u);
        return;
    }
    v->drum = false;
    v->osc.setWaveform(m_params.waveform);
    applyVoiceFrequency(*v);
    v->osc.reset();
    v->env.setParams(m_params.adsr);
    v->env.noteOn();
    v->filter.reset();
}

// 드럼 보이스 1샘플. 진폭이 바닥에 닿으면 보이스를 회수한다.
float Synth::drumNext(Voice& v) {
    v.dAmp *= v.dAmpCoef;
    if (v.dAmp < 0.0005f) {
        v.active = false;
        return 0.0f;
    }
    float out = 0.0f;
    if (v.dNoiseMix < 1.0f && v.dFreq > 0.0f) { // 톤 성분 (피치 드롭 사인)
        v.dFreq = v.dFreqEnd + (v.dFreq - v.dFreqEnd) * v.dPitchCoef;
        v.dPhase += (float)(v.dFreq / m_sampleRate);
        if (v.dPhase >= 1.0f) v.dPhase -= 1.0f;
        out += std::sin(v.dPhase * 6.28318530718f) * (1.0f - v.dNoiseMix);
    }
    if (v.dNoiseMix > 0.0f) { // 노이즈 성분 (하이패스 양으로 밝기 결정)
        v.dRng = v.dRng * 1664525u + 1013904223u;
        const float n = ((float)(v.dRng >> 8) / 8388608.0f) - 1.0f; // -1~1
        v.dLp += 0.18f * (n - v.dLp);
        out += (n - v.dLp * v.dHpAmt) * v.dNoiseMix * 0.9f;
    }
    return out * v.dAmp * v.dGain;
}

void Synth::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    noteOnFloat(channel, note, (float)velocity / 127.0f);
}

void Synth::noteOff(uint8_t channel, uint8_t note) {
    // 같은 채널·음높이의 활성 보이스를 릴리스로 보낸다 (드럼은 원샷이라 무시)
    for (auto& v : m_voices)
        if (v.active && !v.drum && v.channel == channel && v.note == note) v.env.noteOff();
}

void Synth::allNotesOff() {
    for (auto& v : m_voices)
        if (v.active) v.env.noteOff();
}

void Synth::allSoundOff() {
    // 즉시 무음: 모든 보이스를 바로 비활성화한다 (릴리스 잔음 없음).
    for (auto& v : m_voices) {
        v.active = false;
        v.filter.reset();
    }
}

void Synth::setPitchBend(uint8_t channel, float bendNorm) {
    for (auto& v : m_voices)
        if (v.active && v.channel == channel) {
            v.bendNorm = bendNorm;
            applyVoiceFrequency(v);
        }
}

void Synth::setPerNotePitchBend(uint8_t channel, uint8_t note, float bendNorm) {
    for (auto& v : m_voices)
        if (v.active && v.channel == channel && v.note == note) {
            v.perNoteBendNorm = bendNorm;
            applyVoiceFrequency(v);
        }
}

void Synth::setPressure(uint8_t channel, float value01) {
    for (auto& v : m_voices)
        if (v.active && v.channel == channel) v.pressure = value01;
}

void Synth::setTimbre(uint8_t channel, float value01) {
    for (auto& v : m_voices)
        if (v.active && v.channel == channel) v.timbre = value01;
}

float Synth::debugVoiceFrequency(uint8_t channel, uint8_t note) const {
    for (const auto& v : m_voices)
        if (v.active && !v.drum && v.channel == channel && v.note == note) {
            const double semis =
                ((double)v.bendNorm + (double)v.perNoteBendNorm) * (double)m_bendRangeSemis;
            return (float)(Oscillator::noteToHz(v.note) * std::pow(2.0, semis / 12.0));
        }
    return -1.0f;
}

void Synth::render(float* out, int frames) { renderInto(out, nullptr, frames); }

void Synth::renderStereo(float* outL, float* outR, int frames) {
    renderInto(outL, outR, frames);
}

void Synth::renderBuses(float* const* busL, float* const* busR, int frames) {
    const double cutoff = m_params.filterCutoff;
    const double res = m_params.filterResonance;
    const float lfoDepth = m_params.lfoDepth;
    const float master = m_params.masterVolume;

    for (int i = 0; i < frames; ++i) {
        const float lfo = m_lfo.next();
        const double modCutoff = cutoff * (1.0 + lfoDepth * 0.5 * lfo);

        // 프레임당 채널별 드라이 합 (스택, 할당 없음)
        float dryL[kChannels] = {0.0f};
        float dryR[kChannels] = {0.0f};

        for (auto& v : m_voices) {
            if (!v.active) continue;
            float sample, gain;
            if (v.drum) { // 드럼: 전용 생성기 (엔벨로프/필터 안 거침)
                sample = drumNext(v);
                if (!v.active) continue;
                gain = v.velocityGain;
            } else {
                const float envLevel = v.env.next();
                if (!v.env.isActive()) {
                    v.active = false;
                    continue;
                }
                const double voiceCutoff =
                    modCutoff * (0.5 + (double)v.timbre + 0.6 * v.pressure);
                v.filter.setParams(voiceCutoff, res);
                sample = v.filter.processLowpass(v.osc.next()) * envLevel;
                gain = v.velocityGain * (1.0f + 0.6f * v.pressure);
            }
            const int ch = v.channel & 0x0F;
            const float s = sample * gain * m_chGain[ch];
            const float p = m_chPan[ch];
            dryL[ch] += s * (p <= 0.0f ? 1.0f : 1.0f - p);
            dryR[ch] += s * (p >= 0.0f ? 1.0f : 1.0f + p);
        }

        // 버스마다 딜레이 + 마스터볼륨을 적용해 누적한다.
        for (int ch = 0; ch < kChannels; ++ch) {
            float l = m_busDelayL[ch].process(dryL[ch]) * master;
            float r = m_busDelayR[ch].process(dryR[ch]) * master;
            if (l > 1.0f) l = 1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f;
            if (r < -1.0f) r = -1.0f;
            busL[ch][i] += l;
            busR[ch][i] += r;
        }
    }
}

void Synth::renderInto(float* outL, float* outR, int frames) {
    const double cutoff = m_params.filterCutoff;
    const double res = m_params.filterResonance;
    const float lfoDepth = m_params.lfoDepth;
    const float master = m_params.masterVolume;
    const bool stereo = (outR != nullptr);

    for (int i = 0; i < frames; ++i) {
        // LFO는 프레임당 한 번 계산해 모든 보이스 필터에 공유한다.
        const float lfo = m_lfo.next(); // -1~1
        // 컷오프를 배수(0.5x~1.5x 정도)로 변조
        const double modCutoff = cutoff * (1.0 + lfoDepth * 0.5 * lfo);

        float mixL = 0.0f, mixR = 0.0f;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            float sample, gain;
            if (v.drum) { // 드럼: 전용 생성기 (엔벨로프/필터 안 거침)
                sample = drumNext(v);
                if (!v.active) continue;
                gain = v.velocityGain;
            } else {
                const float envLevel = v.env.next();
                if (!v.env.isActive()) {
                    // 릴리스가 끝나 소리가 0이 된 보이스는 회수
                    v.active = false;
                    continue;
                }
                // 표현(MPE): 음색(timbre)은 컷오프를, 압력(pressure)은 밝기+음량을
                // 노트별로 조절한다. timbre 0.5=중립(×1.0).
                const double voiceCutoff =
                    modCutoff * (0.5 + (double)v.timbre + 0.6 * v.pressure);
                v.filter.setParams(voiceCutoff, res);
                sample = v.filter.processLowpass(v.osc.next()) * envLevel;
                gain = v.velocityGain * (1.0f + 0.6f * v.pressure);
            }
            // 채널(트랙) 볼륨을 보이스에 곱한다 -> 믹서의 트랙 볼륨이 MIDI에도 적용
            const int ch = v.channel & 0x0F;
            const float s = sample * gain * m_chGain[ch];
            if (stereo) {
                const float p = m_chPan[ch];
                mixL += s * (p <= 0.0f ? 1.0f : 1.0f - p);
                mixR += s * (p >= 0.0f ? 1.0f : 1.0f + p);
            } else {
                mixL += s; // 모노: 팬 무시
            }
        }

        // 딜레이(간이 리버브) 후 마스터 볼륨, 소프트 클립
        float l = m_delayL.process(mixL) * master;
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        outL[i] = l;
        if (stereo) {
            float r = m_delayR.process(mixR) * master;
            if (r > 1.0f) r = 1.0f;
            if (r < -1.0f) r = -1.0f;
            outR[i] = r;
        }
    }
}

int Synth::activeVoiceCount() const {
    int count = 0;
    for (const auto& v : m_voices)
        if (v.active) ++count;
    return count;
}

} // namespace midipro::audio
