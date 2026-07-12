// =============================================================
// MidiPro - tests/test_synth.cpp
// 신스 DSP 순수 로직 유닛 테스트 (Rule 6):
//   RtAudio(하드웨어) 없이 Synth를 직접 렌더해 검증한다.
// =============================================================

#include "audio/BuiltinFx.h"
#include "audio/PitchDetect.h"
#include "audio/Synth.h"
#include "audio/SynthPreset.h"
#include "audio/dsp/Oscillator.h"

#include <algorithm>
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

// 채널(트랙) 게인: 그 채널 보이스의 음량만 줄어야 한다.
void testChannelGain() {
    Synth a, b;
    a.prepare(44100.0);
    b.prepare(44100.0);
    std::vector<float> full(2048, 0.0f), half(2048, 0.0f);

    a.noteOn(3, 69, 100);
    renderInto(a, full);

    b.setChannelMix(3, 0.5f, 0.0f); // 채널 3만 절반 볼륨
    b.noteOn(3, 69, 100);
    renderInto(b, half);

    CHECK(rms(full) > 0.001);
    // 게인 0.5 -> RMS도 대략 절반 (딜레이/클립 영향으로 여유를 둔다)
    CHECK(rms(half) < rms(full) * 0.75);
    CHECK(rms(half) > rms(full) * 0.25);

    // 다른 채널은 영향 없음
    Synth c;
    c.prepare(44100.0);
    c.setChannelMix(3, 0.0f, 0.0f); // 채널 3 뮤트
    c.noteOn(5, 69, 100);           // 소리는 채널 5
    std::vector<float> other(2048, 0.0f);
    renderInto(c, other);
    CHECK(rms(other) > 0.001);

    // 게인 0 = 무음
    Synth d;
    d.prepare(44100.0);
    d.setChannelMix(3, 0.0f, 0.0f);
    d.noteOn(3, 69, 100);
    std::vector<float> muted(2048, 0.0f);
    renderInto(d, muted);
    CHECK(rms(muted) < 1e-6);
}

// 채널 팬: 완전 좌측이면 오른쪽 채널은 무음이어야 한다.
void testChannelPan() {
    Synth synth;
    synth.prepare(44100.0);
    std::vector<float> l(2048, 0.0f), r(2048, 0.0f);

    synth.setChannelMix(0, 1.0f, -1.0f); // 완전 좌측
    synth.noteOn(0, 69, 100);
    synth.renderStereo(l.data(), r.data(), (int)l.size());
    CHECK(rms(l) > 0.001);
    CHECK(rms(r) < 1e-6);

    // 중앙이면 좌우가 같다
    Synth center;
    center.prepare(44100.0);
    std::vector<float> cl(2048, 0.0f), cr(2048, 0.0f);
    center.noteOn(0, 69, 100);
    center.renderStereo(cl.data(), cr.data(), (int)cl.size());
    CHECK(std::fabs(rms(cl) - rms(cr)) < 1e-6);
    CHECK(rms(cl) > 0.001);
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

// ---- 내장 이펙트 (BuiltinFx) ----

// 파라미터 기본값(EQ 0dB, 딜레이/리버브 믹스만)에서 신호가 파괴되지 않는지,
// 각 이펙트가 실제로 신호를 바꾸는지 확인한다.
void testBuiltinFxBasics() {
    constexpr int kN = 512;
    constexpr double kSr = 44100.0;
    std::vector<float> l(kN), r(kN), l0(kN), r0(kN);
    auto fill = [&] {
        for (int i = 0; i < kN; ++i) {
            l[i] = l0[i] = std::sin(2.0 * 3.14159265 * 440.0 * i / kSr) * 0.5f;
            r[i] = r0[i] = l[i];
        }
    };
    float* ch2[2] = {l.data(), r.data()};

    // EQ 0dB = 거의 패스스루
    {
        BuiltinFx eq(BuiltinFx::kEq);
        fill();
        eq.process(ch2, kN, kSr);
        double diff = 0;
        for (int i = 0; i < kN; ++i) diff += std::fabs((double)l[i] - l0[i]);
        CHECK(diff / kN < 1e-3);
    }
    // EQ 저음 +12dB = 신호가 바뀐다 (100Hz 사인으로)
    {
        BuiltinFx eq(BuiltinFx::kEq);
        for (int i = 0; i < kN; ++i)
            l[i] = l0[i] = r[i] = std::sin(2.0 * 3.14159265 * 100.0 * i / kSr) * 0.25f;
        eq.setParam(0, 12.0f);
        eq.process(ch2, kN, kSr);
        double a = 0, b = 0;
        for (int i = kN / 2; i < kN; ++i) { // 필터가 자리잡은 후반부만
            a += std::fabs((double)l[i]);
            b += std::fabs((double)l0[i]);
        }
        CHECK(a > b * 1.5); // 저음이 눈에 띄게 커졌다
    }
    // 딜레이 믹스 0 = 드라이 그대로
    {
        BuiltinFx dl(BuiltinFx::kDelay);
        dl.setParam(2, 0.0f);
        fill();
        dl.process(ch2, kN, kSr);
        double diff = 0;
        for (int i = 0; i < kN; ++i) diff += std::fabs((double)l[i] - l0[i]);
        CHECK(diff < 1e-6);
    }
    // 딜레이: 임펄스가 지정 시간 뒤에 메아리로 나온다
    {
        BuiltinFx dl(BuiltinFx::kDelay);
        dl.setParam(0, 5.0f); // 5ms = 220샘플 @44.1k (최소 20ms로 클램프 -> 882)
        dl.setParam(2, 1.0f); // 웻 100%
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        l[0] = r[0] = 1.0f;
        std::vector<float> outL;
        for (int blk = 0; blk < 4; ++blk) { // 2048샘플 진행
            dl.process(ch2, kN, kSr);
            outL.insert(outL.end(), l.begin(), l.end());
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
        }
        const int expect = (int)(0.020 * kSr); // 클램프된 20ms
        bool found = false;
        for (int i = expect - 2; i <= expect + 2 && i < (int)outL.size(); ++i)
            if (std::fabs(outL[(std::size_t)i]) > 0.5f) found = true;
        CHECK(found);
    }
    // 리미터: 실링 아래 신호는 그대로, 큰 신호는 실링 밑으로 눌린다
    {
        BuiltinFx lm(BuiltinFx::kLimiter);
        fill(); // 피크 0.5 — 실링(-0.3dB=0.966) 아래라 거의 그대로
        lm.process(ch2, kN, kSr);
        double diff = 0;
        for (int i = 0; i < kN; ++i) diff += std::fabs((double)l[i] - l0[i]);
        CHECK(diff / kN < 1e-3);
        CHECK(lm.gainReductionDb() < 0.1f);
    }
    {
        BuiltinFx lm(BuiltinFx::kLimiter);
        for (int i = 0; i < kN; ++i) // 피크 2.0 — 그대로면 심한 클리핑
            l[i] = r[i] = std::sin(2.0 * 3.14159265 * 440.0 * i / kSr) * 2.0f;
        lm.process(ch2, kN, kSr);
        float mx = 0.0f;
        for (int i = 0; i < kN; ++i) mx = std::max(mx, std::fabs(l[i]));
        CHECK(mx <= 0.967f);              // 실링(-0.3dB) 이하로 제한됐다
        CHECK(lm.gainReductionDb() > 5.0f); // 6dB 이상 눌렀음을 보고한다
    }

    // 컴프레서: 스레숄드 위 신호는 눌리고, 아래 신호는 그대로
    {
        BuiltinFx cmp(BuiltinFx::kCompressor);
        cmp.setParam(0, -20.0f); // 스레숄드 -20dB
        cmp.setParam(1, 10.0f);  // 10:1
        cmp.setParam(2, 0.1f);   // 어택 최솟값 (빠르게 자리잡게)
        fill();                  // 피크 0.5 = -6dB (스레숄드 위)
        for (int blk = 0; blk < 4; ++blk) cmp.process(ch2, kN, kSr); // 엔벨로프 정착
        CHECK(cmp.gainReductionDb() > 6.0f); // (-6 - -20) * 0.9 ≈ 12.6dB 감소
        float mx = 0.0f;
        for (int i = 0; i < kN; ++i) mx = std::max(mx, std::fabs(l[i]));
        CHECK(mx < 0.25f); // 0.5가 크게 눌렸다
    }
    {
        BuiltinFx cmp(BuiltinFx::kCompressor);
        cmp.setParam(0, 0.0f); // 스레숄드 0dB = 아무것도 안 누른다
        fill();
        cmp.process(ch2, kN, kSr);
        double diff = 0;
        for (int i = 0; i < kN; ++i) diff += std::fabs((double)l[i] - l0[i]);
        CHECK(diff / kN < 1e-4);
        CHECK(cmp.gainReductionDb() < 0.1f);
    }

    // 사이드체인: 자기 입력이 조용해도 키 신호가 크면 눌린다 (덕킹)
    {
        BuiltinFx cmp(BuiltinFx::kCompressor);
        cmp.setParam(0, -20.0f); // 스레숄드
        cmp.setParam(1, 10.0f);  // 10:1
        cmp.setParam(2, 0.1f);   // 빠른 어택
        std::vector<float> key(kN);
        for (int i = 0; i < kN; ++i)
            key[(std::size_t)i] = std::sin(2.0 * 3.14159265 * 60.0 * i / kSr) * 0.9f; // 킥처럼 큼
        for (int blk = 0; blk < 4; ++blk) {
            for (int i = 0; i < kN; ++i) // 자기 입력은 -26dB쯤으로 조용하게
                l[i] = r[i] = std::sin(2.0 * 3.14159265 * 220.0 * i / kSr) * 0.05f;
            cmp.processSidechain(ch2, key.data(), key.data(), kN, kSr);
        }
        CHECK(cmp.gainReductionDb() > 6.0f); // 키 덕분에 눌렸다
        float mx = 0.0f;
        for (int i = 0; i < kN; ++i) mx = std::max(mx, std::fabs(l[i]));
        CHECK(mx < 0.02f); // 조용한 신호가 더 눌렸다 (덕킹)
    }

    // 리버브 믹스 0 = 드라이 그대로, 믹스 0.5 = 꼬리가 생긴다
    {
        BuiltinFx rv(BuiltinFx::kReverb);
        rv.setParam(2, 0.0f);
        fill();
        rv.process(ch2, kN, kSr);
        double diff = 0;
        for (int i = 0; i < kN; ++i) diff += std::fabs((double)l[i] - l0[i]);
        CHECK(diff < 1e-6);
    }
    {
        BuiltinFx rv(BuiltinFx::kReverb);
        rv.setParam(2, 0.5f);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        l[0] = r[0] = 1.0f;
        std::vector<float> tail;
        for (int blk = 0; blk < 8; ++blk) {
            rv.process(ch2, kN, kSr);
            tail.insert(tail.end(), l.begin(), l.end());
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
        }
        double energy = 0;
        for (std::size_t i = 2000; i < tail.size(); ++i) energy += std::fabs((double)tail[i]);
        CHECK(energy > 0.01); // 임펄스 후에도 잔향이 이어진다
    }
}

// ---- 내장 드럼 (채널 10) ----
void testDrumVoices() {
    // 킥: 소리가 나고, 노트오프 없이도 스스로 잦아든다 (원샷)
    {
        Synth s;
        s.prepare(44100.0);
        std::vector<float> buf(4410); // 0.1s
        s.noteOn(9, 36, 127);
        renderInto(s, buf);
        const double a = rms(buf);
        CHECK(a > 0.01);
        for (int k = 0; k < 6; ++k) renderInto(s, buf); // ~0.6s 경과
        CHECK(rms(buf) < a * 0.2); // 충분히 감쇠
    }
    // 킥(저역)과 클로즈드 햇(고역 노이즈)은 소리가 다르다 — 영교차 수로 비교
    {
        const auto zc = [](const std::vector<float>& b) {
            int c = 0;
            for (std::size_t i = 1; i < b.size(); ++i)
                if ((b[i - 1] < 0.0f) != (b[i] < 0.0f)) ++c;
            return c;
        };
        Synth sk;
        sk.prepare(44100.0);
        std::vector<float> kick(2205);
        sk.noteOn(9, 36, 127);
        renderInto(sk, kick);
        Synth sh;
        sh.prepare(44100.0);
        std::vector<float> hat(2205);
        sh.noteOn(9, 42, 127);
        renderInto(sh, hat);
        CHECK(zc(hat) > zc(kick) * 3); // 햇이 훨씬 밝다(고주파)
    }
    // 드럼 맵 밖 노트(메트로놈 클릭 84 등)는 기존 신스 음 그대로
    {
        Synth s;
        s.prepare(44100.0);
        s.noteOn(9, 84, 100);
        CHECK(s.debugVoiceFrequency(9, 84) > 0.0f); // 일반 보이스로 잡혔다
    }
}

// 연습 모드 피치 검출: 기타 음역의 합성 파형으로 정확도를 검증한다
void testPitchDetect() {
    constexpr int kN = 2048;
    constexpr double kSr = 48000.0;
    std::vector<float> buf((std::size_t)kN);

    // 기타 소리 흉내: 기본음 + 배음(1/k 감쇠) — E2, A2, G3, E4
    const double freqs[] = {82.41, 110.0, 196.0, 329.63};
    for (double f : freqs) {
        for (int i = 0; i < kN; ++i) {
            double s = 0.0;
            for (int h = 1; h <= 5; ++h)
                s += std::sin(2.0 * 3.14159265358979 * f * h * i / kSr) / (double)h;
            buf[(std::size_t)i] = (float)(0.3 * s);
        }
        const double got = detectPitchHz(buf.data(), kN, kSr);
        CHECK(got > 0.0);
        CHECK(std::fabs(got - f) / f < 0.012); // 20센트(약 1.2%) 이내
        CHECK(hzToMidi(got) == (int)std::lround(69.0 + 12.0 * std::log2(f / 440.0)));
    }

    // 무음과 백색잡음은 0 (판정 보류 — 엉뚱한 BAD를 내지 않게)
    std::fill(buf.begin(), buf.end(), 0.0f);
    CHECK(detectPitchHz(buf.data(), kN, kSr) == 0.0);
    unsigned seed = 12345;
    for (int i = 0; i < kN; ++i) {
        seed = seed * 1664525u + 1013904223u;
        buf[(std::size_t)i] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
    }
    CHECK(detectPitchHz(buf.data(), kN, kSr) == 0.0);
}

// 코드 검증(notePresent): 디스토션 걸린 파워코드에서 구성음을 알아보는가
void testChordVerify() {
    constexpr int kN = 8192;
    constexpr double kSr = 48000.0;
    std::vector<float> buf((std::size_t)kN);

    // E5 파워코드 (E2=40, B2=47, E3=52) + 배음, tanh 소프트클립 = 디스토션 모사.
    // 현실처럼 줄마다 튜닝이 살짝 나가 있다 (±12센트).
    const int chord[] = {40, 47, 52};
    const double detune[] = {12.0, -10.0, 8.0}; // 센트
    for (int i = 0; i < kN; ++i) {
        double s = 0.0;
        for (int c = 0; c < 3; ++c) {
            const double f =
                440.0 * std::pow(2.0, (chord[c] - 69) / 12.0 + detune[c] / 1200.0);
            for (int h = 1; h <= 4; ++h)
                s += std::sin(2.0 * 3.14159265358979 * f * h * i / kSr) / (double)h;
        }
        buf[(std::size_t)i] = (float)std::tanh(1.5 * s * 0.25);
    }
    for (int m : chord) CHECK(notePresent(buf.data(), kN, kSr, m));
    CHECK(!notePresent(buf.data(), kN, kSr, 41)); // F2 — 코드에 없는 반음 이웃
    CHECK(!notePresent(buf.data(), kN, kSr, 49)); // C#3 — 코드에 없는 음

    // 무음이면 아무 음도 '있다'고 하면 안 된다
    std::fill(buf.begin(), buf.end(), 0.0f);
    CHECK(!notePresent(buf.data(), kN, kSr, 40));
}

// 온셋 감지: 잡음 바닥 위에서 스트로크(코드 어택) 두 번을 정확히 잡는가
void testOnsetDetect() {
    constexpr double kSr = 48000.0;
    constexpr int kLen = (int)(1.4 * kSr);
    std::vector<float> sig((std::size_t)kLen, 0.0f);

    // 잡음 바닥 (-46dB쯤)
    unsigned seed = 777;
    for (int i = 0; i < kLen; ++i) {
        seed = seed * 1664525u + 1013904223u;
        sig[(std::size_t)i] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 0.005f;
    }
    // 스트로크 흉내: 코드 3음이 4ms 간격으로 순차 어택, 지수 감쇠.
    // 픽 트랜지언트(브로드밴드 어택 잡음 ~6ms)도 섞는다 — 실제 픽킹에 반드시
    // 있고, 감지기(로그-플럭스)가 "같은 코드 재타격"을 이것으로 구분한다.
    auto strum = [&](double at) {
        const int chord[] = {40, 47, 52};
        unsigned prng = (unsigned)(at * 1000.0) | 1;
        for (int c = 0; c < 3; ++c) {
            const double f = 440.0 * std::pow(2.0, (chord[c] - 69) / 12.0);
            const int s0 = (int)(at * kSr) + c * 192;
            for (int i = 0; i < (int)(0.5 * kSr) && s0 + i < kLen; ++i) {
                const double env = std::exp(-3.0 * i / kSr);
                double v = 0.0;
                for (int h = 1; h <= 4; ++h)
                    v += std::sin(2.0 * 3.14159265358979 * f * h * i / kSr) / (double)h;
                double s = 0.25 * env * v;
                if (i < (int)(0.006 * kSr)) {
                    prng = prng * 1664525u + 1013904223u;
                    const double nz = (double)(prng >> 8) / 8388608.0 - 1.0;
                    s += 0.25 * 0.35 * nz * (1.0 - i / (0.006 * kSr));
                }
                sig[(std::size_t)(s0 + i)] += (float)s;
            }
        }
    };
    // 연타 포함: 0.4/0.5/0.6초(100ms 간격, 앞 음이 울리는 중 재타격) + 0.9초
    strum(0.4);
    strum(0.5);
    strum(0.6);
    strum(0.9);

    OnsetDetector det;
    std::vector<int> onsets; // hop 위치 (샘플)
    for (int pos = 0; pos + OnsetDetector::kFft <= kLen; pos += OnsetDetector::kHop)
        if (det.feed(sig.data() + pos)) onsets.push_back(pos);
    CHECK(onsets.size() == 4);
    if (onsets.size() == 4) {
        const double expect[] = {0.4, 0.5, 0.6, 0.9};
        for (int i = 0; i < 4; ++i) // 각 온셋이 실제 어택의 30ms 이내
            CHECK(std::abs(onsets[(std::size_t)i] - (int)(expect[i] * kSr)) <
                  (int)(0.03 * kSr));
    }
}

} // namespace

int main() {
    testDrumVoices();
    testPitchDetect();
    testChordVerify();
    testOnsetDetect();
    testBuiltinFxBasics();
    testNoteToHz();
    testSoundOnNoteOn();
    testNoteOffDecays();
    testPolyphony();
    testVoiceStealingDoesNotOverflow();
    testMpePerNoteBend();
    testPerNotePitchBend();
    testChannelGain();
    testChannelPan();
    testPresetRoundTrip();

    if (g_failures == 0) {
        std::cout << "[OK] synth tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
