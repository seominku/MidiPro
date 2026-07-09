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
    m_delay.prepare(sampleRate, /*maxDelaySec=*/1.0);
}

void Synth::setParams(const SynthParams& params) {
    m_params = params;
    m_lfo.setFrequency(params.lfoRateHz);
    m_delay.setParams(params.delayTimeSec, params.delayFeedback, params.delayMix);
    for (auto& v : m_voices) {
        if (v.active) updateVoiceTimbre(v);
    }
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
    // 채널 벤드 + 노트별 벤드를 합산해 반음 -> 주파수 배수: 2^(semis/12)
    const double semis = ((double)v.bendNorm + (double)v.perNoteBendNorm) * (double)m_bendRangeSemis;
    const double hz = Oscillator::noteToHz(v.note) * std::pow(2.0, semis / 12.0);
    v.osc.setFrequency(hz);
}

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
    v->osc.setWaveform(m_params.waveform);
    applyVoiceFrequency(*v);
    v->osc.reset();
    v->env.setParams(m_params.adsr);
    v->env.noteOn();
    v->filter.reset();
}

void Synth::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    noteOnFloat(channel, note, (float)velocity / 127.0f);
}

void Synth::noteOff(uint8_t channel, uint8_t note) {
    // 같은 채널·음높이의 활성 보이스를 릴리스로 보낸다
    for (auto& v : m_voices)
        if (v.active && v.channel == channel && v.note == note) v.env.noteOff();
}

void Synth::allNotesOff() {
    for (auto& v : m_voices)
        if (v.active) v.env.noteOff();
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
        if (v.active && v.channel == channel && v.note == note) {
            const double semis =
                ((double)v.bendNorm + (double)v.perNoteBendNorm) * (double)m_bendRangeSemis;
            return (float)(Oscillator::noteToHz(v.note) * std::pow(2.0, semis / 12.0));
        }
    return -1.0f;
}

void Synth::render(float* out, int frames) {
    const double cutoff = m_params.filterCutoff;
    const double res = m_params.filterResonance;
    const float lfoDepth = m_params.lfoDepth;
    const float master = m_params.masterVolume;

    for (int i = 0; i < frames; ++i) {
        // LFO는 프레임당 한 번 계산해 모든 보이스 필터에 공유한다.
        const float lfo = m_lfo.next(); // -1~1
        // 컷오프를 배수(0.5x~1.5x 정도)로 변조
        const double modCutoff = cutoff * (1.0 + lfoDepth * 0.5 * lfo);

        float mix = 0.0f;
        for (auto& v : m_voices) {
            if (!v.active) continue;
            const float envLevel = v.env.next();
            if (!v.env.isActive()) {
                // 릴리스가 끝나 소리가 0이 된 보이스는 회수
                v.active = false;
                continue;
            }
            // 표현(MPE): 음색(timbre)은 컷오프를, 압력(pressure)은 밝기+음량을
            // 노트별로 조절한다. timbre 0.5=중립(×1.0).
            const double voiceCutoff = modCutoff * (0.5 + (double)v.timbre + 0.6 * v.pressure);
            v.filter.setParams(voiceCutoff, res);
            float sample = v.osc.next();
            sample = v.filter.processLowpass(sample);
            const float gain = v.velocityGain * (1.0f + 0.6f * v.pressure);
            mix += sample * envLevel * gain;
        }

        // 딜레이(간이 리버브) 후 마스터 볼륨, 소프트 클립
        float wet = m_delay.process(mix);
        float outSample = wet * master;
        if (outSample > 1.0f) outSample = 1.0f;
        if (outSample < -1.0f) outSample = -1.0f;
        out[i] = outSample;
    }
}

int Synth::activeVoiceCount() const {
    int count = 0;
    for (const auto& v : m_voices)
        if (v.active) ++count;
    return count;
}

} // namespace midipro::audio
