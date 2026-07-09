// =============================================================
// MidiPro - tests/test_synth.cpp
// 신스 DSP 순수 로직 유닛 테스트 (Rule 6):
//   RtAudio(하드웨어) 없이 Synth를 직접 렌더해 검증한다.
// =============================================================

#include "audio/Synth.h"
#include "audio/SynthPreset.h"
#include "audio/dsp/Oscillator.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace midipro::audio;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

// 버퍼의 RMS(음량 대용)
double rms(const std::vector<float>& buf) {
    double sum = 0.0;
    for (float s : buf) sum += (double)s * s;
    return std::sqrt(sum / (double)buf.size());
}

void renderInto(Synth& synth, std::vector<float>& buf) {
    synth.render(buf.data(), (int)buf.size());
}

void testNoteToHz() {
    CHECK(std::fabs(Oscillator::noteToHz(69) - 440.0) < 0.001);  // A4
    CHECK(std::fabs(Oscillator::noteToHz(60) - 261.6256) < 0.01); // C4
    CHECK(std::fabs(Oscillator::noteToHz(81) - 880.0) < 0.001);   // A5 = 한 옥타브 위
}

void testSoundOnNoteOn() {
    Synth synth;
    synth.prepare(44100.0);

    std::vector<float> buf(2048, 0.0f);

    // 아무 노트도 없으면 무음
    renderInto(synth, buf);
    CHECK(rms(buf) < 1e-6);

    // 노트를 켜면 소리가 난다
    synth.noteOn(0, 69, 100);
    renderInto(synth, buf);
    CHECK(rms(buf) > 0.001);
    CHECK(synth.activeVoiceCount() == 1);
}

void testNoteOffDecays() {
    Synth synth;
    SynthParams p;
    p.adsr.releaseSec = 0.02f; // 빨리 사라지게
    synth.prepare(44100.0);
    synth.setParams(p);

    synth.noteOn(0, 60, 120);
    std::vector<float> buf(1024, 0.0f);
    renderInto(synth, buf);
    const double loud = rms(buf);
    CHECK(loud > 0.001);

    synth.noteOff(0, 60);
    // 릴리스가 끝날 만큼 충분히 렌더 (0.02s @44100 ~ 900샘플, 넉넉히)
    std::vector<float> tail(8192, 0.0f);
    renderInto(synth, tail);
    // 릴리스 종료 후 보이스가 회수돼 무음이어야 한다
    CHECK(synth.activeVoiceCount() == 0);
}

void testPolyphony() {
    Synth synth;
    synth.prepare(44100.0);

    synth.noteOn(0, 60, 100);
    synth.noteOn(0, 64, 100);
    synth.noteOn(0, 67, 100);
    std::vector<float> buf(512, 0.0f);
    renderInto(synth, buf);
    CHECK(synth.activeVoiceCount() == 3); // 세 음 동시

    synth.allNotesOff();
    // allNotesOff는 릴리스로 보낼 뿐이니 즉시 0은 아님. 길게 렌더하면 회수.
    std::vector<float> tail(1 << 16, 0.0f);
    renderInto(synth, tail);
    CHECK(synth.activeVoiceCount() == 0);
}

void testVoiceStealingDoesNotOverflow() {
    Synth synth;
    synth.prepare(44100.0);
    // 최대 보이스보다 많이 눌러도 활성 수는 한도를 넘지 않는다
    for (int n = 40; n < 40 + Synth::kMaxVoices + 8; ++n)
        synth.noteOn(0, (uint8_t)n, 100);
    std::vector<float> buf(256, 0.0f);
    renderInto(synth, buf);
    CHECK(synth.activeVoiceCount() <= Synth::kMaxVoices);
}

void testMpePerNoteBend() {
    Synth synth;
    synth.prepare(44100.0);
    synth.setPitchBendRange(48.0f); // MPE 멤버 채널 기본

    // MPE: 노트마다 다른 채널로 온다
    synth.noteOn(2, 69, 100); // A4, ch2
    synth.noteOn(3, 69, 100); // A4, ch3
    CHECK(std::fabs(synth.debugVoiceFrequency(2, 69) - 440.0f) < 0.5f);
    CHECK(std::fabs(synth.debugVoiceFrequency(3, 69) - 440.0f) < 0.5f);

    // ch2만 +2반음 벤드 (48범위에서 norm 2/48)
    synth.setPitchBend(2, 2.0f / 48.0f);
    const float f2 = synth.debugVoiceFrequency(2, 69);
    const float f3 = synth.debugVoiceFrequency(3, 69);
    CHECK(std::fabs(f2 - 493.88f) < 1.0f); // A4 +2반음 = B4
    CHECK(std::fabs(f3 - 440.0f) < 0.5f);  // ch3는 영향 없음 (노트별 독립)

    // 채널을 안 쓰는(모두 ch0) 일반 벤드는 함께 걸린다
    Synth s2;
    s2.prepare(44100.0);
    s2.setPitchBendRange(2.0f);
    s2.noteOn(0, 60, 100);
    s2.noteOn(0, 64, 100);
    s2.setPitchBend(0, 1.0f); // 최대 +2반음
    const float a = s2.debugVoiceFrequency(0, 60);
    const float b = s2.debugVoiceFrequency(0, 64);
    CHECK(a > Oscillator::noteToHz(60) * 1.05f); // 둘 다 올라감
    CHECK(b > Oscillator::noteToHz(64) * 1.05f);
}

void testPerNotePitchBend() {
    // MIDI 2.0: 같은 채널의 두 노트를 노트별로 독립 벤딩
    Synth synth;
    synth.prepare(44100.0);
    synth.setPitchBendRange(2.0f);
    synth.noteOn(0, 60, 100); // C4
    synth.noteOn(0, 64, 100); // E4

    synth.setPerNotePitchBend(0, 60, 1.0f); // C4만 최대 +2반음
    const float f60 = synth.debugVoiceFrequency(0, 60);
    const float f64 = synth.debugVoiceFrequency(0, 64);
    CHECK(f60 > Oscillator::noteToHz(60) * 1.10f); // C4는 올라감
    CHECK(std::fabs(f64 - Oscillator::noteToHz(64)) < 0.5f); // E4는 그대로 (노트별 독립)

    // 고해상도(float) 벨로시티도 발음
    Synth s2;
    s2.prepare(44100.0);
    s2.noteOnFloat(0, 72, 0.8f);
    std::vector<float> buf(1024, 0.0f);
    s2.render(buf.data(), (int)buf.size());
    CHECK(rms(buf) > 0.001);
}

void testPresetRoundTrip() {
    SynthParams p;
    p.waveform = Waveform::Square;
    p.adsr = {0.02f, 0.3f, 0.55f, 0.4f};
    p.filterCutoff = 3210.0f;
    p.filterResonance = 0.42f;
    p.lfoRateHz = 2.5f;
    p.lfoDepth = 0.6f;
    p.delayTimeSec = 0.33f;
    p.delayFeedback = 0.5f;
    p.delayMix = 0.28f;
    p.masterVolume = 0.22f;

    const std::string text = serialize(p);
    SynthParams q;
    CHECK(deserialize(q, text));
    CHECK(q.waveform == Waveform::Square);
    CHECK(std::fabs(q.adsr.attackSec - 0.02f) < 1e-4);
    CHECK(std::fabs(q.adsr.sustain - 0.55f) < 1e-4);
    CHECK(std::fabs(q.filterCutoff - 3210.0f) < 0.1);
    CHECK(std::fabs(q.lfoDepth - 0.6f) < 1e-4);
    CHECK(std::fabs(q.delayMix - 0.28f) < 1e-4);
    CHECK(std::fabs(q.masterVolume - 0.22f) < 1e-4);

    // 우리 포맷이 아닌 텍스트는 거부한다
    SynthParams r;
    CHECK(!deserialize(r, "hello world 123"));

    // 내장 프리셋 목록이 비어있지 않다
    CHECK(!builtinPresets().empty());
}

} // namespace

int main() {
    testNoteToHz();
    testSoundOnNoteOn();
    testNoteOffDecays();
    testPolyphony();
    testVoiceStealingDoesNotOverflow();
    testMpePerNoteBend();
    testPerNotePitchBend();
    testPresetRoundTrip();

    if (g_failures == 0) {
        std::cout << "[OK] synth tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
