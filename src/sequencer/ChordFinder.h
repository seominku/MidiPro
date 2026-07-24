#pragma once
// =============================================================
// MidiPro - sequencer/ChordFinder.h
// 멜로디 MIDI 노트 → ① 조성(스케일) 판별 → ② 다이어토닉 코드 → ③ 마디별
// 코드 추천. 순수 로직(헤더온리)이라 GUI 없이 테스트할 수 있다 (Rule 6).
//
// 판별은 옥타브를 무시하고 12개 피치 클래스로만 한다. 등장 "횟수"가 아니라
// "길이 합"으로 가중치를 준다(길게 울린 음이 곡에서 더 중요하므로).
// =============================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace midipro::seq {

// 입력 멜로디 노트 (옥타브 포함 MIDI 번호 + 길이).
struct MelNote {
    uint8_t note = 0;      // MIDI 노트 번호 (0~127)
    uint32_t startTick = 0;
    uint32_t durTicks = 0; // 울린 길이 (틱)
};

struct MusicKey {
    int root = 0;       // 으뜸음 피치 클래스 (0=C)
    bool minor = false; // true=단조, false=장조
};

// 코드: 근음 피치 클래스 + 성격 + 구성음 3개(피치 클래스).
struct Chord {
    int root = 0;
    int quality = 0; // 0=메이저, 1=마이너, 2=디미니시
    std::array<int, 3> pcs = {0, 4, 7};
};

struct BarChord {
    int bar = 0;      // 0부터
    Chord chord;
    double score = 0; // 그 마디에서 이 코드가 받은 점수 (진단용)
};

// ── 스케일 패턴 ──
inline const std::array<int, 7>& majorScalePattern() {
    static const std::array<int, 7> p = {0, 2, 4, 5, 7, 9, 11};
    return p;
}
inline const std::array<int, 7>& minorScalePattern() {
    static const std::array<int, 7> p = {0, 2, 3, 5, 7, 8, 10}; // 자연단음계
    return p;
}

// ── 1단계: 조성 판별 (Krumhansl-Schmuckler 프로파일 상관계수) ──
// 피치 클래스별 길이 합 히스토그램과, 각 조로 회전한 프로파일의 피어슨
// 상관계수를 24개 조에 대해 구해 최댓값을 고른다. 단순 "포함 가점"보다
// 장조/단조·근접 조 구분이 정확하다.
inline MusicKey detectKey(const std::vector<MelNote>& notes) {
    // K-S 프로파일 (장조·단조 각 12개 가중치)
    static const double kMaj[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                    2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double kMin[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                    2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    double h[12] = {};
    for (const auto& n : notes)
        h[n.note % 12] += (double)n.durTicks;

    double sumH = 0.0;
    for (double v : h) sumH += v;
    if (sumH <= 0.0) return {0, false}; // 음이 없으면 C장조

    // 피어슨 상관계수: corr(h, profile를 root만큼 회전)
    auto corr = [&](const double* prof, int root) {
        double x[12];
        for (int i = 0; i < 12; ++i) x[i] = prof[(i - root + 12) % 12];
        double mx = 0.0, my = 0.0;
        for (int i = 0; i < 12; ++i) { mx += x[i]; my += h[i]; }
        mx /= 12.0;
        my /= 12.0;
        double num = 0.0, dx = 0.0, dy = 0.0;
        for (int i = 0; i < 12; ++i) {
            const double a = x[i] - mx, b = h[i] - my;
            num += a * b;
            dx += a * a;
            dy += b * b;
        }
        const double den = std::sqrt(dx * dy);
        return den > 1e-12 ? num / den : -1.0;
    };

    MusicKey best{0, false};
    double bestScore = -2.0;
    for (int root = 0; root < 12; ++root) {
        const double sMaj = corr(kMaj, root);
        if (sMaj > bestScore) { bestScore = sMaj; best = {root, false}; }
        const double sMin = corr(kMin, root);
        if (sMin > bestScore) { bestScore = sMin; best = {root, true}; }
    }
    return best;
}

// ── 2단계: 다이어토닉 코드 7개 ──
// 스케일의 각 음을 근음으로 삼아 한 음 건너 3개(1·3·5번째 스케일 음)를 쌓는다.
// 성격(메이저/마이너/디미니시)은 구성음 간격에서 자동으로 나오므로 장·단조 공용.
inline std::array<Chord, 7> diatonicChords(MusicKey key) {
    const auto& pat = key.minor ? minorScalePattern() : majorScalePattern();
    int scale[7];
    for (int i = 0; i < 7; ++i) scale[i] = (pat[(std::size_t)i] + key.root) % 12;

    std::array<Chord, 7> out;
    for (int i = 0; i < 7; ++i) {
        const int r = scale[i];
        const int third = scale[(i + 2) % 7];
        const int fifth = scale[(i + 4) % 7];
        const int t = (third - r + 12) % 12; // 3=단3도, 4=장3도
        const int f = (fifth - r + 12) % 12; // 6=감5도, 7=완전5도
        int q = 0;
        if (t == 4 && f == 7) q = 0;      // 메이저
        else if (t == 3 && f == 7) q = 1; // 마이너
        else q = 2;                       // 디미니시 (t==3, f==6)
        out[(std::size_t)i] = Chord{r, q, {r, third, fifth}};
    }
    return out;
}

struct ChordRecoOptions {
    uint32_t ticksPerBar = 1920; // 한 마디 틱 (박자표 기준, 호출자가 계산)
    uint32_t minNoteTicks = 0;   // 이 길이 미만 음은 경과음으로 보고 제외 (0=끄기)
    bool smooth = true;          // 이웃 마디와 점수가 비슷하면 유지 (진행 안정화)
};

// ── 3단계: 마디별 코드 추천 ──
// 각 마디에서 멜로디 음이 코드 구성음에 포함되면 그 음의 길이만큼 가점,
// 최고점 코드를 그 마디 코드로 확정. 동점이면 주요 3화음(I·IV·V) 우선.
inline std::vector<BarChord> recommendChords(const std::vector<MelNote>& notes,
                                             MusicKey key,
                                             const ChordRecoOptions& opt) {
    const auto chords = diatonicChords(key);
    const uint32_t tpb = opt.ticksPerBar > 0 ? opt.ticksPerBar : 1920;

    // 마디 수 = 마지막 노트 끝이 든 마디까지
    uint32_t endTick = 0;
    for (const auto& n : notes)
        endTick = std::max(endTick, n.startTick + n.durTicks);
    const int nBars = endTick > 0 ? (int)((endTick + tpb - 1) / tpb) : 0;

    // 마디별 피치 클래스 길이 합 (음이 마디 경계를 넘으면 겹치는 만큼 각 마디에 배분)
    std::vector<std::array<double, 12>> hist((std::size_t)nBars);
    for (auto& a : hist) a.fill(0.0);
    for (const auto& n : notes) {
        if (opt.minNoteTicks > 0 && n.durTicks < opt.minNoteTicks) continue;
        const int pc = n.note % 12;
        const uint32_t s = n.startTick, e = n.startTick + n.durTicks;
        for (int b = (int)(s / tpb); b <= (int)((e - 1) / tpb) && b < nBars; ++b) {
            const uint32_t bs = (uint32_t)b * tpb, be = bs + tpb;
            const uint32_t ov = (std::min(e, be) > std::max(s, bs))
                                    ? std::min(e, be) - std::max(s, bs)
                                    : 0;
            hist[(std::size_t)b][(std::size_t)pc] += (double)ov;
        }
    }

    std::vector<BarChord> out;
    int prevIdx = -1; // 직전 마디에서 고른 코드 자리(0~6)
    for (int b = 0; b < nBars; ++b) {
        // 마디에 아무 음도 없으면 직전 코드를 유지(없으면 I)
        double total = 0.0;
        for (double v : hist[(std::size_t)b]) total += v;
        if (total <= 0.0) {
            const int idx = prevIdx >= 0 ? prevIdx : 0;
            out.push_back({b, chords[(std::size_t)idx], 0.0});
            continue;
        }
        // 코드별 점수 = 구성음에 든 피치 클래스의 길이 합
        double score[7];
        for (int i = 0; i < 7; ++i) {
            double s = 0.0;
            for (int pc : chords[(std::size_t)i].pcs) s += hist[(std::size_t)b][(std::size_t)pc];
            score[i] = s;
        }
        // 최고점 (동점: I·IV·V 우선 → 직전 코드 유지 → 낮은 자리)
        auto isPrimary = [](int i) { return i == 0 || i == 3 || i == 4; };
        int bestIdx = 0;
        for (int i = 1; i < 7; ++i) {
            const double d = score[i] - score[bestIdx];
            if (d > 1e-9) { bestIdx = i; continue; }
            if (d < -1e-9) continue;
            // 동점 처리
            if (isPrimary(i) && !isPrimary(bestIdx)) bestIdx = i;
            else if (isPrimary(i) == isPrimary(bestIdx) && i == prevIdx) bestIdx = i;
        }
        // 스무딩: 직전 코드가 최고점의 92% 이상이면 유지(코드가 너무 자주 안 바뀌게)
        if (opt.smooth && prevIdx >= 0 && prevIdx != bestIdx &&
            score[prevIdx] >= score[bestIdx] * 0.92)
            bestIdx = prevIdx;

        out.push_back({b, chords[(std::size_t)bestIdx], score[bestIdx]});
        prevIdx = bestIdx;
    }
    return out;
}

// ── 표기 헬퍼 ──
inline const char* pitchClassName(int pc) {
    static const char* n[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                "F#", "G",  "G#", "A",  "A#", "B"};
    return n[((pc % 12) + 12) % 12];
}
inline std::string chordName(const Chord& c) {
    std::string s = pitchClassName(c.root);
    if (c.quality == 1) s += "m";
    else if (c.quality == 2) s += "dim";
    return s;
}
inline std::string keyName(MusicKey k) {
    std::string s = pitchClassName(k.root);
    s += k.minor ? "m" : ""; // Am / C
    return s;
}

} // namespace midipro::seq
