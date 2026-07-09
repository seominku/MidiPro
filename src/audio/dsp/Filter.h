#pragma once
// =============================================================
// MidiPro - audio/dsp/Filter.h
// 저역통과 필터 (state-variable filter, 순수 DSP, 헤더 전용).
//
// 왜 SVF인가: 계수 계산이 간단하고 실시간으로 컷오프를 흔들어도
//   (LFO 변조) 안정적이라 오디오 콜백에서 매 샘플 갱신하기 좋다.
//   Andrew Simper의 SVF(TPT) 구조를 저역 출력만 사용한다.
// =============================================================

#include <cmath>

namespace midipro::audio {

class Filter {
public:
    void setSampleRate(double sampleRate) { m_sampleRate = sampleRate; }

    // cutoff(Hz), resonance(0~1). 매 샘플 호출해도 될 만큼 가볍다.
    void setParams(double cutoffHz, double resonance) {
        // 나이퀴스트 근처 발산 방지로 컷오프를 제한
        const double maxHz = m_sampleRate * 0.45;
        if (cutoffHz > maxHz) cutoffHz = maxHz;
        if (cutoffHz < 20.0) cutoffHz = 20.0;
        m_g = std::tan(3.14159265358979323846 * cutoffHz / m_sampleRate);
        // resonance 0~1 -> Q; 1에 가까울수록 감쇠(k) 작아짐
        const double k = 2.0 - 1.98 * (resonance < 0.0 ? 0.0 : (resonance > 1.0 ? 1.0 : resonance));
        m_k = k;
        m_a1 = 1.0 / (1.0 + m_g * (m_g + k));
        m_a2 = m_g * m_a1;
    }

    void reset() { m_ic1 = m_ic2 = 0.0; }

    float processLowpass(float in) {
        const double v3 = in - m_ic2;
        const double v1 = m_a1 * m_ic1 + m_a2 * v3;
        const double v2 = m_ic2 + m_g * v1;
        m_ic1 = 2.0 * v1 - m_ic1;
        m_ic2 = 2.0 * v2 - m_ic2;
        return (float)v2; // 저역 출력
    }

private:
    double m_sampleRate = 44100.0;
    double m_g = 0.0, m_k = 1.0, m_a1 = 0.0, m_a2 = 0.0;
    double m_ic1 = 0.0, m_ic2 = 0.0; // 적분기 상태
};

} // namespace midipro::audio
