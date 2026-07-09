#pragma once
// =============================================================
// MidiPro - audio/dsp/Adsr.h
// ADSR 엔벨로프 (순수 DSP, 헤더 전용).
//
// 왜 필요한가: 오실레이터를 그냥 켜고 끄면 클릭 노이즈가 난다.
//   Attack/Decay/Sustain/Release로 진폭을 부드럽게 만든다.
//
// 상태 머신: Idle -> Attack -> Decay -> Sustain -> Release -> Idle.
//   각 구간은 목표값을 향해 선형 증분한다. 오디오 콜백에서
//   샘플마다 next()를 부르며, 할당/분기 최소화 (Rule 3).
// =============================================================

namespace midipro::audio {

struct AdsrParams {
    float attackSec = 0.005f;
    float decaySec = 0.08f;
    float sustain = 0.7f; // 0~1
    float releaseSec = 0.15f;
};

class Adsr {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setSampleRate(double sampleRate) { m_sampleRate = sampleRate; }
    void setParams(const AdsrParams& p) { m_params = p; }

    void noteOn() {
        m_stage = Stage::Attack;
        // 남은 레벨에서 자연스럽게 이어지도록 level은 리셋하지 않는다
    }
    void noteOff() {
        if (m_stage != Stage::Idle) m_stage = Stage::Release;
    }

    bool isActive() const { return m_stage != Stage::Idle; }
    float level() const { return m_level; }

    float next() {
        switch (m_stage) {
        case Stage::Idle:
            m_level = 0.0f;
            break;
        case Stage::Attack:
            m_level += rate(m_params.attackSec);
            if (m_level >= 1.0f) {
                m_level = 1.0f;
                m_stage = Stage::Decay;
            }
            break;
        case Stage::Decay:
            m_level -= rate(m_params.decaySec) * (1.0f - m_params.sustain);
            if (m_level <= m_params.sustain) {
                m_level = m_params.sustain;
                m_stage = Stage::Sustain;
            }
            break;
        case Stage::Sustain:
            m_level = m_params.sustain;
            break;
        case Stage::Release:
            m_level -= rate(m_params.releaseSec);
            if (m_level <= 0.0f) {
                m_level = 0.0f;
                m_stage = Stage::Idle;
            }
            break;
        }
        return m_level;
    }

private:
    // 구간 초 동안 0->1(또는 1->0) 이동에 필요한 샘플당 증분.
    // 0으로 나눔을 막기 위해 최소 시간을 둔다.
    float rate(float seconds) const {
        const double s = seconds < 0.0001 ? 0.0001 : seconds;
        return (float)(1.0 / (s * m_sampleRate));
    }

    double m_sampleRate = 44100.0;
    AdsrParams m_params;
    Stage m_stage = Stage::Idle;
    float m_level = 0.0f;
};

} // namespace midipro::audio
