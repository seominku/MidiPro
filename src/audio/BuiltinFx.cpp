// =============================================================
// MidiPro - audio/BuiltinFx.cpp
// 내장 이펙트 DSP 구현. 설계 메모는 BuiltinFx.h 참고.
// =============================================================

#include "audio/BuiltinFx.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace midipro::audio {

namespace {
constexpr double kMaxSampleRate = 192000.0; // 선할당 기준
constexpr double kPi = 3.14159265358979323846;

// EQ 고정 코너 주파수 (미드 중심만 파라미터)
constexpr double kEqLowHz = 120.0;
constexpr double kEqHighHz = 6000.0;

// 리버브 콤/올패스 길이 (44.1kHz 기준 샘플 수, Freeverb 튜닝에서 발췌)
constexpr int kCombTuning[4] = {1116, 1188, 1277, 1356};
constexpr int kApTuning[2] = {556, 441};
constexpr int kStereoSpread = 23; // 오른쪽 채널을 살짝 늦춰 폭을 만든다

const BuiltinFx::ParamDesc kEqParams[BuiltinFx::kNumParams] = {
    {"저음 (120Hz)", -12.0f, 12.0f, 0.0f, false, "%+.1f dB"},
    {"중음", -12.0f, 12.0f, 0.0f, false, "%+.1f dB"},
    {"고음 (6kHz)", -12.0f, 12.0f, 0.0f, false, "%+.1f dB"},
    {"중음 주파수", 100.0f, 8000.0f, 1000.0f, true, "%.0f Hz"},
    {nullptr, 0, 0, 0, false, nullptr},
};
const BuiltinFx::ParamDesc kDelayParams[BuiltinFx::kNumParams] = {
    {"시간", 20.0f, 1000.0f, 300.0f, false, "%.0f ms"},
    {"피드백", 0.0f, 0.9f, 0.35f, false, "%.2f"},
    {"믹스", 0.0f, 1.0f, 0.3f, false, "%.2f"},
    {nullptr, 0, 0, 0, false, nullptr},
    {nullptr, 0, 0, 0, false, nullptr},
};
const BuiltinFx::ParamDesc kReverbParams[BuiltinFx::kNumParams] = {
    {"공간 크기", 0.0f, 1.0f, 0.5f, false, "%.2f"},
    {"댐핑", 0.0f, 1.0f, 0.4f, false, "%.2f"},
    {"믹스", 0.0f, 1.0f, 0.3f, false, "%.2f"},
    {nullptr, 0, 0, 0, false, nullptr},
    {nullptr, 0, 0, 0, false, nullptr},
};
const BuiltinFx::ParamDesc kLimiterParams[BuiltinFx::kNumParams] = {
    {"게인", -12.0f, 12.0f, 0.0f, false, "%+.1f dB"},
    {"실링", -6.0f, 0.0f, -0.3f, false, "%.1f dB"},
    {"릴리스", 20.0f, 500.0f, 120.0f, false, "%.0f ms"},
    {nullptr, 0, 0, 0, false, nullptr},
    {nullptr, 0, 0, 0, false, nullptr},
};
const BuiltinFx::ParamDesc kCompressorParams[BuiltinFx::kNumParams] = {
    {"스레숄드", -60.0f, 0.0f, -18.0f, false, "%.0f dB"},
    {"비율", 1.0f, 20.0f, 4.0f, false, "%.1f:1"},
    {"어택", 0.1f, 100.0f, 10.0f, true, "%.1f ms"},
    {"릴리스", 20.0f, 500.0f, 120.0f, false, "%.0f ms"},
    {"메이크업", 0.0f, 24.0f, 0.0f, false, "%+.1f dB"},
};

// 아주 작은 값은 0으로 (denormal이 CPU를 태우는 것 방지)
inline float flush(float v) { return (v > -1e-18f && v < 1e-18f) ? 0.0f : v; }
} // namespace

const char* BuiltinFx::typeName(int type) {
    switch (type) {
    case kEq: return "EQ";
    case kDelay: return "딜레이";
    case kReverb: return "리버브";
    case kLimiter: return "리미터";
    case kCompressor: return "컴프레서";
    default: return "?";
    }
}

const char* BuiltinFx::typeToken(int type) {
    switch (type) {
    case kEq: return "eq";
    case kDelay: return "delay";
    case kReverb: return "reverb";
    case kLimiter: return "limiter";
    case kCompressor: return "comp";
    default: return "";
    }
}

int BuiltinFx::typeFromToken(const char* token) {
    for (int t = 0; t < kTypes; ++t)
        if (std::strcmp(token, typeToken(t)) == 0) return t;
    return -1;
}

const BuiltinFx::ParamDesc* BuiltinFx::paramDescs(int type) {
    switch (type) {
    case kDelay: return kDelayParams;
    case kReverb: return kReverbParams;
    case kLimiter: return kLimiterParams;
    case kCompressor: return kCompressorParams;
    default: return kEqParams;
    }
}

BuiltinFx::BuiltinFx(int type) : m_type(std::clamp(type, 0, kTypes - 1)) {
    const ParamDesc* pd = paramDescs(m_type);
    for (int i = 0; i < kNumParams; ++i)
        m_params[i].store(pd[i].label ? pd[i].def : 0.0f, std::memory_order_relaxed);

    if (m_type == kDelay) {
        const std::size_t cap = (std::size_t)(kMaxSampleRate * 2.0) + 8; // 최대 2초
        m_dbuf[0].assign(cap, 0.0f);
        m_dbuf[1].assign(cap, 0.0f);
    } else if (m_type == kReverb) {
        for (int c = 0; c < kCombs; ++c)
            for (int ch = 0; ch < 2; ++ch) {
                const int maxLen = (int)((kCombTuning[c] + kStereoSpread) * kMaxSampleRate /
                                         44100.0) + 8;
                m_comb[c][ch].assign((std::size_t)maxLen, 0.0f);
            }
        for (int a = 0; a < kAllpasses; ++a)
            for (int ch = 0; ch < 2; ++ch) {
                const int maxLen = (int)((kApTuning[a] + kStereoSpread) * kMaxSampleRate /
                                         44100.0) + 8;
                m_ap[a][ch].assign((std::size_t)maxLen, 0.0f);
            }
    }
}

float BuiltinFx::param(int i) const {
    if (i < 0 || i >= kNumParams) return 0.0f;
    return m_params[i].load(std::memory_order_relaxed);
}

void BuiltinFx::setParam(int i, float v) {
    if (i < 0 || i >= kNumParams) return;
    const ParamDesc& d = paramDescs(m_type)[i];
    if (d.label) v = std::clamp(v, d.min, d.max);
    m_params[i].store(v, std::memory_order_relaxed);
    m_dirty.store(true, std::memory_order_release);
}

// RBJ cookbook 계수. kind: 0=로우셸프, 1=피크, 2=하이셸프.
static void makeBiquad(BuiltinFx::Biquad& q, int kind, double sr, double f0, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * std::clamp(f0, 10.0, sr * 0.45) / sr;
    const double cw = std::cos(w0), sw = std::sin(w0);
    double b0, b1, b2, a0, a1, a2;
    if (kind == 1) { // 피킹 EQ (Q 고정 0.9)
        const double alpha = sw / (2.0 * 0.9);
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else { // 셸프 (S=0.9)
        const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 0.9 - 1.0) + 2.0);
        const double sq = 2.0 * std::sqrt(A) * alpha;
        if (kind == 0) { // 로우 셸프
            b0 = A * ((A + 1) - (A - 1) * cw + sq);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sq);
            a0 = (A + 1) + (A - 1) * cw + sq;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sq;
        } else { // 하이 셸프
            b0 = A * ((A + 1) + (A - 1) * cw + sq);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sq);
            a0 = (A + 1) - (A - 1) * cw + sq;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sq;
        }
    }
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0;
    q.a1 = a1 / a0; q.a2 = a2 / a0;
}

void BuiltinFx::refresh(double sr) {
    m_sr = sr;
    if (m_type == kEq) {
        for (int ch = 0; ch < 2; ++ch) {
            makeBiquad(m_eq[0][ch], 0, sr, kEqLowHz, (double)param(0));
            makeBiquad(m_eq[1][ch], 1, sr, (double)param(3), (double)param(1));
            makeBiquad(m_eq[2][ch], 2, sr, kEqHighHz, (double)param(2));
        }
    } else if (m_type == kDelay) {
        const double ms = std::clamp((double)param(0), 1.0, 2000.0);
        m_dlen = std::max(1, (int)(ms * 0.001 * sr));
        if ((std::size_t)m_dlen >= m_dbuf[0].size()) m_dlen = (int)m_dbuf[0].size() - 1;
    } else if (m_type == kLimiter) {
        m_limGain = std::pow(10.0f, param(0) / 20.0f);
        m_limCeil = std::pow(10.0f, param(1) / 20.0f);
        const double relSec = std::max(0.005, (double)param(2) * 0.001);
        m_limRelCoef = (float)(1.0 - std::exp(-1.0 / (relSec * sr)));
    } else if (m_type == kCompressor) {
        m_cmpThrDb = param(0);
        const float ratio = param(1) < 1.0f ? 1.0f : param(1);
        m_cmpSlope = 1.0f - 1.0f / ratio;
        const double attSec = std::max(0.0001, (double)param(2) * 0.001);
        const double relSec = std::max(0.005, (double)param(3) * 0.001);
        m_cmpAttCoef = (float)(1.0 - std::exp(-1.0 / (attSec * sr)));
        m_cmpRelCoef = (float)(1.0 - std::exp(-1.0 / (relSec * sr)));
        m_cmpMakeup = std::pow(10.0f, param(4) / 20.0f);
    } else { // 리버브
        const float room = param(0);
        m_combFeedback = 0.7f + room * 0.28f;
        m_damp = std::clamp(param(1), 0.0f, 1.0f) * 0.8f;
        const double scale = sr / 44100.0;
        for (int c = 0; c < kCombs; ++c)
            for (int ch = 0; ch < 2; ++ch) {
                int len = (int)((kCombTuning[c] + (ch == 1 ? kStereoSpread : 0)) * scale);
                len = std::clamp(len, 8, (int)m_comb[c][ch].size() - 1);
                if (len != m_combLen[c][ch]) {
                    m_combLen[c][ch] = len;
                    if (m_combPos[c][ch] >= len) m_combPos[c][ch] = 0;
                }
            }
        for (int a = 0; a < kAllpasses; ++a)
            for (int ch = 0; ch < 2; ++ch) {
                int len = (int)((kApTuning[a] + (ch == 1 ? kStereoSpread : 0)) * scale);
                len = std::clamp(len, 8, (int)m_ap[a][ch].size() - 1);
                if (len != m_apLen[a][ch]) {
                    m_apLen[a][ch] = len;
                    if (m_apPos[a][ch] >= len) m_apPos[a][ch] = 0;
                }
            }
    }
}

void BuiltinFx::process(float* const* ch2, int frames, double sampleRate) {
    if (frames <= 0 || sampleRate <= 0.0) return;
    if (m_dirty.exchange(false, std::memory_order_acquire) || sampleRate != m_sr)
        refresh(sampleRate);

    if (m_type == kEq) {
        for (int ch = 0; ch < 2; ++ch) {
            float* p = ch2[ch];
            for (int i = 0; i < frames; ++i) {
                float v = p[i];
                v = m_eq[0][ch].run(v);
                v = m_eq[1][ch].run(v);
                v = m_eq[2][ch].run(v);
                p[i] = flush(v);
            }
        }
        return;
    }

    if (m_type == kLimiter) {
        // 피크 리미터: 어택 즉시(엔벨로프가 피크로 바로 점프), 릴리스는 지수 감쇠.
        // 실링을 넘는 만큼 게인을 줄이고, 남는 오버슛은 하드 클립으로 막는다.
        float minG = 1.0f;
        for (int i = 0; i < frames; ++i) {
            float l = ch2[0][i] * m_limGain;
            float r = ch2[1][i] * m_limGain;
            const float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            const float peak = al > ar ? al : ar;
            if (peak > m_limEnv) m_limEnv = peak; // 즉시 어택
            else m_limEnv += (peak - m_limEnv) * m_limRelCoef;
            const float g = m_limEnv > m_limCeil ? m_limCeil / m_limEnv : 1.0f;
            if (g < minG) minG = g;
            l *= g;
            r *= g;
            l = std::clamp(l, -m_limCeil, m_limCeil);
            r = std::clamp(r, -m_limCeil, m_limCeil);
            ch2[0][i] = l;
            ch2[1][i] = r;
        }
        m_limEnv = flush(m_limEnv);
        m_grDb.store(minG >= 1.0f ? 0.0f : -20.0f * std::log10(minG),
                     std::memory_order_relaxed);
        return;
    }

    if (m_type == kCompressor) {
        processSidechain(ch2, nullptr, nullptr, frames, sampleRate); // key 없음 = 자기 입력
        return;
    }

    if (m_type == kDelay) {
        const float fb = param(1);
        const float mix = param(2);
        const std::size_t cap = m_dbuf[0].size();
        std::size_t pos = m_dpos;
        for (int i = 0; i < frames; ++i) {
            const std::size_t rd = (pos + cap - (std::size_t)m_dlen) % cap;
            for (int ch = 0; ch < 2; ++ch) {
                float* p = ch2[ch];
                const float dry = p[i];
                const float wet = m_dbuf[ch][rd];
                m_dbuf[ch][pos] = flush(dry + wet * fb);
                p[i] = dry * (1.0f - mix) + wet * mix;
            }
            pos = (pos + 1) % cap;
        }
        m_dpos = pos;
        return;
    }

    // 리버브 (Freeverb 축약형)
    const float mix = param(2);
    const float wetGain = mix * 0.6f; // 콤 4개 합이 커서 살짝 줄인다
    for (int i = 0; i < frames; ++i) {
        const float inMono = (ch2[0][i] + ch2[1][i]) * 0.5f;
        for (int ch = 0; ch < 2; ++ch) {
            float acc = 0.0f;
            for (int c = 0; c < kCombs; ++c) {
                float* buf = m_comb[c][ch].data();
                int& pos = m_combPos[c][ch];
                const float out = buf[pos];
                // 댐핑: 피드백 루프 안 1폴 로우패스 (고음이 먼저 사그라든다)
                m_combLp[c][ch] = flush(out * (1.0f - m_damp) + m_combLp[c][ch] * m_damp);
                buf[pos] = flush(inMono + m_combLp[c][ch] * m_combFeedback);
                if (++pos >= m_combLen[c][ch]) pos = 0;
                acc += out;
            }
            acc *= 0.25f;
            for (int a = 0; a < kAllpasses; ++a) { // 직렬 올패스로 밀도 확산
                float* buf = m_ap[a][ch].data();
                int& pos = m_apPos[a][ch];
                const float bufOut = buf[pos];
                buf[pos] = flush(acc + bufOut * 0.5f);
                acc = bufOut - acc * 0.5f;
                if (++pos >= m_apLen[a][ch]) pos = 0;
            }
            ch2[ch][i] = ch2[ch][i] * (1.0f - mix) + acc * wetGain;
        }
    }
}

void BuiltinFx::processSidechain(float* const* ch2, const float* keyL, const float* keyR,
                                 int frames, double sampleRate) {
    if (frames <= 0 || sampleRate <= 0.0) return;
    if (m_type != kCompressor) { // 컴프레서 외에는 일반 처리로
        process(ch2, frames, sampleRate);
        return;
    }
    if (m_dirty.exchange(false, std::memory_order_acquire) || sampleRate != m_sr)
        refresh(sampleRate);
    // 피크 검출(어택/릴리스 평활) -> 스레숄드 초과분을 비율로 눌러준다.
    // key가 있으면 그 신호로 감지한다 (사이드체인 덕킹), 없으면 자기 입력.
    float minG = 1.0f;
    for (int i = 0; i < frames; ++i) {
        const float sl = keyL ? keyL[i] : ch2[0][i];
        const float sr2 = keyR ? keyR[i] : ch2[1][i];
        const float al = sl < 0 ? -sl : sl;
        const float ar = sr2 < 0 ? -sr2 : sr2;
        const float peak = al > ar ? al : ar;
        m_cmpEnv += (peak - m_cmpEnv) * (peak > m_cmpEnv ? m_cmpAttCoef : m_cmpRelCoef);
        float g = 1.0f;
        if (m_cmpEnv > 1e-6f) {
            const float envDb = 20.0f * std::log10(m_cmpEnv);
            const float over = envDb - m_cmpThrDb;
            if (over > 0.0f) g = std::pow(10.0f, -over * m_cmpSlope / 20.0f);
        }
        if (g < minG) minG = g;
        const float total = g * m_cmpMakeup;
        ch2[0][i] *= total;
        ch2[1][i] *= total;
    }
    m_cmpEnv = flush(m_cmpEnv);
    m_grDb.store(minG >= 1.0f ? 0.0f : -20.0f * std::log10(minG), std::memory_order_relaxed);
}

} // namespace midipro::audio
