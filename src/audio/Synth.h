#pragma once
// =============================================================
// MidiPro - audio/Synth.h
// 폴리포닉 신디사이저 DSP (순수 로직, 오디오 스레드 전용).
//
// 스레딩 (Rule 3):
//   이 클래스는 오디오 스레드에서만 호출된다고 가정한다. 락도
//   할당도 하지 않는다. GUI 스레드와의 안전한 통신(파라미터/노트
//   이벤트 전달)은 RtAudioEngine이 아토믹/락프리 큐로 처리한다.
//
//   보이스 풀은 생성 시 한 번 할당되고, 재생 중에는 재사용만 한다.
//
// 테스트 가능 (Rule 6): RtAudio 없이 noteOn -> render로 파형이
//   생성되는지, noteOff 후 소리가 잦아드는지 단독 검증한다.
// =============================================================

#include "audio/dsp/Adsr.h"
#include "audio/dsp/Delay.h"
#include "audio/dsp/Filter.h"
#include "audio/dsp/Oscillator.h"

#include <array>
#include <cstdint>

namespace midipro::audio {

// GUI가 조절하는 음색 파라미터 (엔진이 아토믹으로 스냅샷해 전달)
struct SynthParams {
    Waveform waveform = Waveform::Saw;
    AdsrParams adsr;
    float filterCutoff = 4000.0f; // Hz
    float filterResonance = 0.2f; // 0~1
    float lfoRateHz = 4.0f;       // 필터 컷오프 변조 LFO
    float lfoDepth = 0.0f;        // 0~1 (0이면 LFO 끔)
    float masterVolume = 0.3f;    // 0~1 (클리핑 여유)
    // 딜레이(간이 리버브)
    float delayTimeSec = 0.25f;
    float delayFeedback = 0.3f;
    float delayMix = 0.2f;
};

class Synth {
public:
    static constexpr int kMaxVoices = 32;

    void prepare(double sampleRate);
    void setParams(const SynthParams& params); // 오디오 스레드에서만

    // 노트/표현은 MIDI 채널을 함께 받는다. MPE에서는 노트마다 다른
    // 채널로 오므로, 채널로 표현(피치벤드/압력/음색)이 개별 보이스에
    // 라우팅되어 노트별로 독립 적용된다. 일반 모드(모두 ch0)에서는
    // 같은 채널의 모든 보이스에 함께 걸린다.
    void noteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOnFloat(uint8_t channel, uint8_t note, float velocity01); // MIDI 2.0 고해상도
    void noteOff(uint8_t channel, uint8_t note);
    void allNotesOff();

    // 표현 (해당 채널의 활성 보이스에 적용)
    void setPitchBend(uint8_t channel, float bendNorm); // -1~1 (채널 전체)
    void setPressure(uint8_t channel, float value01);   // 채널 애프터터치
    void setTimbre(uint8_t channel, float value01);     // CC74 (밝기)

    // MIDI 2.0 노트별 피치벤드: 같은 채널에서도 특정 (채널,노트) 보이스만
    // 벤딩한다. 채널 전체 벤드와 합산된다.
    void setPerNotePitchBend(uint8_t channel, uint8_t note, float bendNorm);

    // 피치벤드 범위(반음). 일반=2, MPE 멤버 채널 기본=48.
    void setPitchBendRange(float semitones) { m_bendRangeSemis = semitones; }

    // mono 버퍼에 frames개 샘플을 채운다 (기존 내용 덮어씀).
    void render(float* out, int frames);

    int activeVoiceCount() const;

    // 테스트용: 해당 채널/노트의 활성 보이스가 내는 주파수(Hz). 없으면 -1.
    float debugVoiceFrequency(uint8_t channel, uint8_t note) const;

private:
    struct Voice {
        bool active = false;
        uint8_t channel = 0;
        uint8_t note = 0;
        float velocityGain = 0.0f;
        float bendNorm = 0.0f;        // -1~1 (채널 전체)
        float perNoteBendNorm = 0.0f; // -1~1 (MIDI 2.0 노트별)
        float pressure = 0.0f;  // 0~1 (애프터터치)
        float timbre = 0.5f;    // 0~1 (CC74, 0.5=중립)
        uint64_t startOrder = 0; // 보이스 스틸링용 (오래된 것 우선)
        Oscillator osc;
        Adsr env;
        Filter filter;
    };

    Voice* findFreeVoice();     // 비었거나 가장 오래된 보이스
    void updateVoiceTimbre(Voice& v);
    void applyVoiceFrequency(Voice& v); // 노트+벤드로 osc 주파수 갱신

    double m_sampleRate = 44100.0;
    float m_bendRangeSemis = 2.0f;
    SynthParams m_params;
    std::array<Voice, kMaxVoices> m_voices;
    Oscillator m_lfo;
    Delay m_delay;
    uint64_t m_orderCounter = 0;
};

} // namespace midipro::audio
