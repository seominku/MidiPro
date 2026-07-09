#pragma once
// =============================================================
// MidiPro - audio/dsp/Delay.h
// 피드백 딜레이 기반 간이 리버브/에코 (순수 DSP, 헤더 전용).
//
// 왜 이 방식인가 (Rule 3):
//   본격 리버브 대신, 미리 할당한 원형 버퍼 하나로 피드백 딜레이를
//   구현해 "공간감"을 준다. 오디오 콜백에서는 버퍼 재사용만 하고
//   할당하지 않는다. 버퍼는 prepare()에서 한 번만 잡는다.
// =============================================================

#include <vector>

namespace midipro::audio {

class Delay {
public:
    // maxDelaySec만큼의 버퍼를 미리 확보한다 (오디오 시작 전 호출).
    void prepare(double sampleRate, double maxDelaySec) {
        m_sampleRate = sampleRate;
        m_buffer.assign((std::size_t)(sampleRate * maxDelaySec) + 1, 0.0f);
        m_writePos = 0;
        setParams(0.25, 0.35f, 0.25f);
    }

    void setParams(double delaySec, float feedback, float mix) {
        std::size_t d = (std::size_t)(delaySec * m_sampleRate);
        if (m_buffer.empty()) return;
        if (d >= m_buffer.size()) d = m_buffer.size() - 1;
        if (d < 1) d = 1;
        m_delaySamples = d;
        m_feedback = feedback < 0.0f ? 0.0f : (feedback > 0.95f ? 0.95f : feedback);
        m_mix = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    }

    float process(float in) {
        if (m_buffer.empty()) return in;
        const std::size_t readPos =
            (m_writePos + m_buffer.size() - m_delaySamples) % m_buffer.size();
        const float delayed = m_buffer[readPos];
        // 입력 + 피드백을 버퍼에 기록
        m_buffer[m_writePos] = in + delayed * m_feedback;
        m_writePos = (m_writePos + 1) % m_buffer.size();
        // dry/wet 믹스
        return in * (1.0f - m_mix) + delayed * m_mix;
    }

    void reset() {
        for (auto& s : m_buffer) s = 0.0f;
        m_writePos = 0;
    }

private:
    double m_sampleRate = 44100.0;
    std::vector<float> m_buffer;
    std::size_t m_writePos = 0;
    std::size_t m_delaySamples = 1;
    float m_feedback = 0.35f;
    float m_mix = 0.25f;
};

} // namespace midipro::audio
