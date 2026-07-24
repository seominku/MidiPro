#pragma once
// =============================================================
// MidiPro - audio/PitchDetect.h
// 단선율 피치 검출 (연습 모드용). 헤더 온리 — GUI와 테스트가 공유한다.
//
// 방식: 정규화 자기상관(NSDF, McLeod Pitch Method 계열) + 포물선 보간.
//  - 기타 음역(70~1000Hz) 전용으로 랙 범위를 제한해 비용을 줄인다
//  - 명료도(피크 값) 0.75 미만이면 0을 반환 (잡음/심한 코드 = 판정 보류)
// =============================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace midipro::audio {

// 반환: Hz. 0 = 무음이거나 음정이 불명확.
inline double detectPitchHz(const float* x, int n, double sr) {
    if (!x || n < 1024 || sr <= 0.0) return 0.0;
    static thread_local std::vector<float> b;
    b.resize((std::size_t)n);
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += x[i];
    mean /= n;
    double energy = 0.0;
    for (int i = 0; i < n; ++i) {
        b[(std::size_t)i] = x[i] - (float)mean;
        energy += (double)b[(std::size_t)i] * b[(std::size_t)i];
    }
    if (energy < 1e-4) return 0.0; // 무음

    const int tauMin = std::max(2, (int)(sr / 1000.0));
    const int tauMax = std::min(n - 2, (int)(sr / 70.0));
    if (tauMax <= tauMin + 2) return 0.0;
    static thread_local std::vector<double> nsdf;
    nsdf.assign((std::size_t)tauMax + 2, 0.0);
    for (int tau = tauMin; tau <= tauMax; ++tau) {
        double acf = 0.0, norm = 0.0;
        const int lim = n - tau;
        for (int i = 0; i < lim; ++i) {
            const double a = b[(std::size_t)i];
            const double c = b[(std::size_t)(i + tau)];
            acf += a * c;
            norm += a * a + c * c;
        }
        nsdf[(std::size_t)tau] = norm > 0.0 ? 2.0 * acf / norm : 0.0;
    }
    // 국소 최대(피크)들 중 최고값의 88% 이상인 "가장 이른" 피크를 고른다.
    // (최고 피크만 고르면 한 옥타브 아래(2배 랙)로 미끄러지기 쉽다)
    double maxv = 0.0;
    for (int t = tauMin + 1; t < tauMax; ++t)
        if (nsdf[(std::size_t)t] > maxv) maxv = nsdf[(std::size_t)t];
    if (maxv < 0.75) return 0.0; // 명료도 부족
    const double thresh = maxv * 0.88;
    for (int t = tauMin + 1; t < tauMax; ++t) {
        const double v = nsdf[(std::size_t)t];
        if (v < thresh) continue;
        if (v < nsdf[(std::size_t)(t - 1)] || v < nsdf[(std::size_t)(t + 1)]) continue;
        const double a = nsdf[(std::size_t)(t - 1)], c = nsdf[(std::size_t)(t + 1)];
        const double den = 2.0 * (2.0 * v - a - c);
        const double d = den != 0.0 ? (c - a) / den : 0.0;
        return sr / ((double)t + std::clamp(d, -0.5, 0.5));
    }
    return 0.0;
}

inline int hzToMidi(double hz) {
    if (hz <= 0.0) return 0;
    const double m = 69.0 + 12.0 * std::log2(hz / 440.0);
    const long r = std::lround(m);
    return (int)(r < 0 ? 0 : (r > 127 ? 127 : r));
}

// Goertzel: 특정 주파수 f의 에너지. FFT 전체가 필요 없을 때(몇 개 음만 검사) 싸다.
inline double goertzelPower(const float* x, int n, double sr, double f) {
    if (!x || n < 16 || sr <= 0.0 || f <= 0.0 || f >= sr * 0.5) return 0.0;
    const double w = 2.0 * 3.14159265358979323846 * f / sr;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = (double)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// "기대-음 검증": midi 음이 신호에 실제로 울리고 있는가.
// 코드(다성)·디스토션에서도 동작한다 — 악보가 기대하는 음의 배음 에너지를
// 반음 이웃(±1)과 비교해, 그 음이 두드러지면 '있다'고 본다.
//  - 창 분해능(sr/n)보다 반음 간격이 좁은 배음은 판별에 못 쓰므로 건너뛴다
//    (저음 기본음이 여기 해당 — 넣으면 이웃과 똑같이 새어 판별을 희석시킨다)
//  - 기타 튜닝 오차(±30센트)를 흡수하도록 목표 주파수를 세 지점에서 탐색한다
inline bool notePresent(const float* x, int n, double sr, int midi) {
    const double f0 = 440.0 * std::pow(2.0, ((double)midi - 69.0) / 12.0);
    const double up = std::pow(2.0, 1.0 / 12.0), dn = 1.0 / up;
    const double res = sr / (double)n;      // 주파수 분해능
    const double kSemi = 0.059463;          // 반음의 상대 간격 (2^(1/12) - 1)
    const double cUp = std::pow(2.0, 30.0 / 1200.0), cDn = 1.0 / cUp; // ±30센트
    double target = 0.0, neigh = 0.0;
    int used = 0;
    for (int h = 1; h <= 5 && used < 3; ++h) {
        const double fh = f0 * h;
        if (fh >= sr * 0.45) break;
        if (fh * kSemi < res * 1.3) continue; // 이 배음으론 반음 구분이 안 된다
        const double t = std::max({goertzelPower(x, n, sr, fh),
                                   goertzelPower(x, n, sr, fh * cUp),
                                   goertzelPower(x, n, sr, fh * cDn)});
        target += t;
        neigh += 0.5 * (goertzelPower(x, n, sr, fh * up) + goertzelPower(x, n, sr, fh * dn));
        ++used;
    }
    if (used == 0) return false;
    return target > 1e-6 && target > neigh * 1.35;
}

// ── 악보 주도 청취 (score-driven listening) ──
// 온셋 감지 없이, "기대하는 음이 그 시각 주변에서 새로 울리기 시작했는가"를
// 기준선(연주 직전) 대비 배음 에너지 상승으로 직접 확인한다. 어떤 주파수를
// 찾을지 미리 아니까, 폴리포닉 온셋 감지의 난제(잔향·맥놀이·스트로크 중복)를
// 원리적으로 피한다.

// midi 음의 배음 에너지(target)와 반음 이웃 에너지(neigh)를 잰다.
// notePresent와 같은 배음·디튠(±30센트) 규칙. neigh<0 = 이웃 비교 불가
// (창이 짧아 어떤 배음도 반음 분해능이 안 나올 때).
// exHz: 이웃 비교에서 제외할 주파수들(±60센트) — 같은 코드의 다른 구성음이
// 배음을 이웃 대역에 정당하게 얹는 경우(예: G코드에서 B2의 3배음 F#4가
// G4 바로 아래) 그 대역을 이웃으로 세면 정상 음이 기각된다.
inline void noteBandPower(const float* x, int n, double sr, int midi,
                          double* tgtOut, double* nghOut,
                          int* strongOut = nullptr, const double* exHz = nullptr,
                          int nEx = 0) {
    // Hann 창: 사각창 Goertzel은 1~2반음 옆의 큰 소리가 1/Δ²로 누설돼
    // (예: A3가 G3 대역에 3~4%) 헛 배음을 만든다. Hann이면 수백 배 준다.
    static thread_local std::vector<float> hann;
    static thread_local std::vector<float> wbuf;
    if ((int)hann.size() != n) {
        hann.resize((std::size_t)n);
        for (int i = 0; i < n; ++i)
            hann[(std::size_t)i] =
                0.5f - 0.5f * (float)std::cos(2.0 * 3.14159265358979 * i / (n - 1));
    }
    wbuf.resize((std::size_t)n);
    for (int i = 0; i < n; ++i) wbuf[(std::size_t)i] = x[i] * hann[(std::size_t)i];
    x = wbuf.data();
    const double f0 = 440.0 * std::pow(2.0, ((double)midi - 69.0) / 12.0);
    const double up = std::pow(2.0, 1.0 / 12.0), dn = 1.0 / up;
    const double res = sr / (double)n;
    const double kSemi = 0.059463; // 2^(1/12) - 1
    const double cUp = std::pow(2.0, 30.0 / 1200.0), cDn = 1.0 / cUp;
    double tgt = 0.0, ngh = 0.0, ph[3] = {};
    int usedN = 0, usedT = 0;
    for (int h = 1; h <= 5 && usedT < 3; ++h) {
        const double fh = f0 * h;
        if (fh >= sr * 0.45) break;
        ph[usedT] = std::max({goertzelPower(x, n, sr, fh),
                              goertzelPower(x, n, sr, fh * cUp),
                              goertzelPower(x, n, sr, fh * cDn)});
        tgt += ph[usedT];
        ++usedT;
        if (fh * kSemi >= res * 1.3) {
            auto excluded = [&](double f) {
                for (int e = 0; e < nEx; ++e) {
                    const double r = f / exHz[e];
                    if (r > 0.966 && r < 1.035) return true; // ±60센트
                }
                return false;
            };
            const double fu = fh * up, fd = fh * dn;
            int cnt = 0;
            double s = 0.0;
            if (!excluded(fu)) {
                s += goertzelPower(x, n, sr, fu);
                ++cnt;
            }
            if (!excluded(fd)) {
                s += goertzelPower(x, n, sr, fd);
                ++cnt;
            }
            if (cnt > 0) {
                ngh += s * (cnt == 2 ? 0.5 : 1.0);
                ++usedN;
            }
        }
    }
    *tgtOut = tgt;
    *nghOut = usedN > 0 ? ngh : -1.0;
    if (strongOut) {
        // "서 있는" 배음 수 — 다른 음의 "고차" 배음 하나가 우연히 겹친 헛
        // 크레딧은 높은 배음 1개에 몰려 있고, 진짜 그 음이 울리면 여러 배음이
        // 함께 서거나 최소한 기본음 대역이 선다. 저음 코드에서는 베이스의
        // 배음이 기본음 대역을 지배해 에너지가 한 대역에 몰릴 수 있으므로,
        // 기본음이 서 있으면 배음 1개여도 인정한다 (옥타브 관용과 같은 급).
        // 사용할 수 있는 배음이 애초에 1개뿐이면(초고음) 판별 불가로 -1.
        if (usedT < 2) {
            *strongOut = -1;
        } else {
            int st = 0;
            for (int i = 0; i < usedT; ++i)
                if (ph[i] >= tgt * 0.08) ++st;
            if (st == 1 && ph[0] >= tgt * 0.08) st = 2;
            *strongOut = st;
        }
    }
}

// x[0..n): 연속 입력 조각. [scan0, ...)이 탐색 구간 (탐색 프레임의 확인
// 프레임까지 n 안에 들어와야 하므로 실제 탐색은 n - 85ms - 프레임까지).
// 판별: 프레임의 배음 에너지가 (1) 85ms 전보다 rise배 이상 뛰고 — 감쇠 중인
// 잔향·맥놀이(85ms에 ±2배 미만)는 못 넘고, 진짜 새 음은 5배 이상 뛴다 —
// (2) 85ms 뒤에도 유지되고(스쳐가는 픽 잡음/광대역 어택 배제), (3) 반음
// 이웃보다 뚜렷하면 그 프레임 중심을 반환. 없으면 -1.
// (프레임 경계 때문에 실제 어택보다 일정하게 이르거나 늦을 수 있다 — 계통
// 편차는 자동 지연 보정이 흡수하므로 상대 타이밍만 정확하면 된다.)
inline int noteRiseAt(const float* x, int n, int scan0, double sr, int midi,
                      double rise, const double* exHz = nullptr, int nEx = 0) {
    constexpr int kFrame = 4096, kHopR = 1024, kGap = 4; // 4 hop = 85ms
    const int pre = kGap * kHopR;
    if (!x || sr <= 0.0 || scan0 < pre || scan0 + pre + kFrame > n) return -1;
    if (scan0 % kHopR != 0) return -1; // 격자 정렬 (호출부 계약)
    // 위치별 파워는 한 번만 계산 — 같은 위치가 이웃 프레임의 기준선(-85ms)과
    // 확인(+85ms)으로 재사용되므로 3배쯤 아낀다. 게이트를 통과 못 하면 이후
    // 계산을 건너뛰도록 지연 계산한다.
    const int nPos = (n - kFrame) / kHopR + 1;
    static thread_local std::vector<double> tgtV, nghV, eV;
    static thread_local std::vector<int> strV;
    tgtV.assign((std::size_t)nPos, -1.0);
    nghV.assign((std::size_t)nPos, 0.0);
    eV.assign((std::size_t)nPos, 0.0);
    strV.assign((std::size_t)nPos, 0);
    auto calc = [&](int i) {
        if (tgtV[(std::size_t)i] >= 0.0) return;
        double t = 0.0, g = 0.0;
        int s = 0;
        noteBandPower(x + i * kHopR, kFrame, sr, midi, &t, &g, &s, exHz, nEx);
        double e = 0.0;
        const float* q = x + i * kHopR;
        for (int k = 0; k < kFrame; ++k) e += (double)q[k] * q[k];
        tgtV[(std::size_t)i] = t;
        nghV[(std::size_t)i] = g;
        strV[(std::size_t)i] = s;
        eV[(std::size_t)i] = e;
    };
    for (int p = scan0; p + pre + kFrame <= n; p += kHopR) {
        const int i = p / kHopR;
        calc(i);
        if (tgtV[(std::size_t)i] < 1e-6) continue; // 절대 바닥
        if (nghV[(std::size_t)i] >= 0.0 &&
            tgtV[(std::size_t)i] < nghV[(std::size_t)i] * 1.2)
            continue; // 이웃 반음보다 뚜렷해야
        if (strV[(std::size_t)i] == 0 || strV[(std::size_t)i] == 1)
            continue; // 배음 1개 = 남의 배음 겹침
        // 프레임 총 에너지 대비 최소 지분 — 무음/사이드로브 누설/광대역
        // 트랜지언트가 "0에서의 상승"으로 통과하는 걸 막는다.
        // (Hann 창 순음 진폭 A: goertzel ≈ (A·n/4)², Σx²(원신호) = A²n/2 → n/8배)
        if (tgtV[(std::size_t)i] < eV[(std::size_t)i] * (kFrame / 8) * 0.01)
            continue;
        calc(i - kGap);
        const double tPrev = tgtV[(std::size_t)(i - kGap)];
        if (tgtV[(std::size_t)i] < tPrev * rise + 1e-9) continue; // 상승
        // 확인 프레임(+85ms)도 "그 음이 울리는 모습"이어야 한다 — 스쳐가는
        // 광대역 트랜지언트(어택 잡음)나 남의 배음 하나가 겹친 헛 크레딧은
        // 여기서 배음 수/에너지 지분을 다시 검사하면 걸러진다.
        calc(i + kGap);
        const double tNext = tgtV[(std::size_t)(i + kGap)];
        if (tNext < tPrev * std::max(1.6, rise * 0.7) + 1e-9) continue; // 유지
        if (tNext < tgtV[(std::size_t)i] * 0.25) continue; // 사라지면 트랜지언트
        if (strV[(std::size_t)(i + kGap)] == 0 || strV[(std::size_t)(i + kGap)] == 1)
            continue;
        if (tNext < eV[(std::size_t)(i + kGap)] * (kFrame / 8) * 0.01) continue;
        return p + kFrame / 2;
    }
    return -1;
}

// ── 온셋(어택) 감지: 스펙트럴 플럭스 + 적응 임계값 ──
// RMS 상승비 방식은 스트로크(여러 줄이 수 ms에 걸쳐 울림)를 잘 놓친다.
// 스펙트럼의 "새로 늘어난 에너지"(플럭스)는 광대역 트랜지언트에 민감하고
// 임계값을 최근 플럭스 통계로 잡아 입력 게인과 무관하게 동작한다.

// 소형 radix-2 FFT (in-place). n = 2의 거듭제곱.
inline void fftRadix2(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) { // 비트 반전 재배열
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = (float)(re[a] - tr);
                im[b] = (float)(im[a] - ti);
                re[a] = (float)(re[a] + tr);
                im[a] = (float)(im[a] + ti);
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

struct OnsetDetector {
    static constexpr int kFft = 512;
    static constexpr int kHop = 256;

    float prevMag[kFft / 2] = {};
    float hist[64] = {}; // 최근 플럭스 이력 (적응 임계값)
    int histN = 0, histPos = 0;
    double fluxPrev = 0.0, fluxPrev2 = 0.0;
    double eHist[16] = {}; // 최근 hop 에너지 (어택 = 에너지 상승, 지속음 = 감쇠/유지)
    int ePos = 0;
    int sinceOnset = 1000;
    float sensitivity = 1.0f; // 크면 둔감, 작으면 민감

    void reset() {
        std::fill(prevMag, prevMag + kFft / 2, 0.0f);
        histN = histPos = 0;
        fluxPrev = fluxPrev2 = 0.0;
        std::fill(eHist, eHist + 16, 0.0);
        ePos = 0;
        sinceOnset = 1000;
    }

    // 최근 512샘플 창(hop 256 전진)을 넣는다.
    // 반환: "직전 hop"이 온셋이었는가 (국소 최대 확인 때문에 한 hop 늦게 확정).
    bool feed(const float* w512) {
        float re[kFft], im[kFft] = {};
        for (int i = 0; i < kFft; ++i) { // Hann 창
            const float h =
                0.5f - 0.5f * (float)std::cos(2.0 * 3.14159265358979 * i / (kFft - 1));
            re[i] = w512[i] * h;
            im[i] = 0.0f;
        }
        fftRadix2(re, im, kFft);
        // 로그-플럭스: 새 배음의 "출현"(무→유)에는 크게 반응하고, 이미 울리는
        // 배음의 음량 출렁임(코드 잔향의 맥놀이)에는 거의 반응하지 않는다.
        // 바닥(eps)은 (1) 프레임 최대 크기의 2%와 (2) 중앙값 빈 크기의 8배 중
        // 큰 쪽 — 중앙값은 배음 몇 개에 안 끌려가는 잡음 바닥 추정치라,
        // 마이크 잡음의 로그 요동이 플럭스를 오염시키지 않게 한다.
        float mag[kFft / 2];
        float mx = 0.0f;
        for (int k = 1; k < kFft / 2; ++k) {
            mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
            mx = std::max(mx, mag[k]);
        }
        float tmp[kFft / 2 - 1];
        for (int k = 1; k < kFft / 2; ++k) tmp[k - 1] = mag[k];
        std::nth_element(tmp, tmp + (kFft / 4), tmp + (kFft / 2 - 1));
        const float med = tmp[kFft / 4];
        const float eps = std::max(std::max(0.05f, mx * 0.02f), med * 8.0f);
        double flux = 0.0;
        for (int k = 1; k < kFft / 2; ++k) {
            const float d = std::log((mag[k] + eps) / (prevMag[k] + eps));
            if (d > 0.0f) flux += (double)d;
            prevMag[k] = mag[k];
        }
        // 적응 임계값: 최근 플럭스 평균의 배수 + 절대 바닥
        double mean = 0.0;
        for (int i = 0; i < histN; ++i) mean += hist[i];
        mean = histN > 0 ? mean / histN : 0.0;
        const double thr = mean * 1.9 * (double)sensitivity + 6.0;
        hist[histPos] = (float)flux;
        histPos = (histPos + 1) & 63;
        if (histN < 64) ++histN;

        // 새 hop의 에너지 (창 뒤쪽 kHop 구간)
        double e = 0.0;
        for (int i = kFft - kHop; i < kFft; ++i) e += (double)w512[i] * w512[i];
        eHist[ePos] = e;
        ePos = (ePos + 1) & 15;
        // 맥놀이 때문에 hop 에너지가 크게 요동하므로 4-hop 이동평균으로 비교한다:
        // 어택 직후 평균 >> 어택 직전 평균, 지속음은 두 평균이 비슷하다.
        auto eAgo = [&](int k) { return eHist[(ePos + 15 - k) & 15]; }; // k hop 전
        double smNew = 0.0, smOld = 0.0;
        for (int k = 0; k < 4; ++k) smNew += eAgo(k);
        for (int k = 6; k < 10; ++k) smOld += eAgo(k);

        ++sinceOnset;
        bool onset = false;
        // 직전 hop이 (1) 플럭스 임계값을 넘는 국소 최대이고 (2) 에너지가 붕괴
        // 중만 아니면 온셋. 맥놀이 거부는 로그-플럭스가 맡는다 — 재타격은 직전
        // 링잉이 한창 감쇠 중일 때 나와서 에너지 비가 0.8 근처까지 내려가므로,
        // 이 조건은 뮤트/릴리스 클릭(에너지 급락 + 플럭스 스파이크)만 거른다.
        const bool rising = smNew > smOld * 0.70 + 1e-7;
        if (fluxPrev > thr && fluxPrev >= flux && fluxPrev >= fluxPrev2 && rising &&
            sinceOnset >= 12) {
            onset = true;
            sinceOnset = 0;
        }
        fluxPrev2 = fluxPrev;
        fluxPrev = flux;
        return onset;
    }
};

} // namespace midipro::audio
