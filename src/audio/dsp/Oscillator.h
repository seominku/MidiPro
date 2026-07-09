#pragma once
// =============================================================
// MidiPro - audio/dsp/Oscillator.h
// 파형 오실레이터 (순수 DSP, 헤더 전용).
//
// 왜 헤더 전용인가: 오디오 콜백에서 샘플마다 호출되는 초경량
//   상태 머신이라 인라인이 유리하고, 동적 할당이 전혀 없다 (Rule 3).
//
// 위상(phase)은 0~1로 정규화해 들고 다니며, 샘플레이트에 맞춰
// 매 샘플 위상 증분을 더한다.
// =============================================================

#include <cmath>
#include <cstdint>

namespace midipro::audio {

enum class Waveform { Sine, Saw, Square, Triangle };

class Oscillator {
public:
    void setSampleRate(double sampleRate) { m_sampleRate = sampleRate; }
    void setFrequency(double hz) { m_phaseInc = hz / m_sampleRate; }
    void setWaveform(Waveform wf) { m_waveform = wf; }
    void reset() { m_phase = 0.0; }

    // 다음 샘플 [-1, 1]. 위상을 전진시키며 파형 값을 낸다.
    float next() {
        float value = 0.0f;
        switch (m_waveform) {
        case Waveform::Sine:
            value = (float)std::sin(m_phase * 2.0 * 3.14159265358979323846);
            break;
        case Waveform::Saw:
            // 위상 0~1 -> -1~1 톱니
            value = (float)(2.0 * m_phase - 1.0);
            break;
        case Waveform::Square:
            value = (m_phase < 0.5) ? 1.0f : -1.0f;
            break;
        case Waveform::Triangle:
            value = (float)(4.0 * std::fabs(m_phase - 0.5) - 1.0);
            break;
        }
        m_phase += m_phaseInc;
        if (m_phase >= 1.0) m_phase -= 1.0;
        return value;
    }

    // MIDI 노트 -> 주파수 (A4=69=440Hz 기준 평균율)
    static double noteToHz(uint8_t note) {
        return 440.0 * std::pow(2.0, ((double)note - 69.0) / 12.0);
    }

private:
    double m_sampleRate = 44100.0;
    double m_phase = 0.0;
    double m_phaseInc = 0.0;
    Waveform m_waveform = Waveform::Saw;
};

} // namespace midipro::audio
