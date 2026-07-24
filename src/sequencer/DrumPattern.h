#pragma once
// =============================================================
// MidiPro - sequencer/DrumPattern.h
// 박자표(4/4·3/4·6/8) + 스타일을 고르면 드럼 패턴을 자동 생성한다.
// 순수 로직(헤더온리)이라 GUI 없이 테스트할 수 있다 (Rule 6).
//
// GM 드럼 맵: 36=킥, 38=스네어, 42=클로즈드 햇, 46=오픈 햇, 49=크래시, 51=라이드
// =============================================================

#include <cstdint>
#include <vector>

namespace midipro::seq {

struct DrumHit {
    uint32_t tick = 0;
    uint8_t note = 0;
    uint8_t velocity = 100;
};

// GM 드럼 노트 번호
enum : uint8_t {
    kDrumKick = 36,
    kDrumSnare = 38,
    kDrumHatClosed = 42,
    kDrumHatOpen = 46,
    kDrumCrash = 49,
    kDrumRide = 51,
};

// 박자표: metroSigIndex와 같은 값 (0=4/4, 1=3/4, 2=6/8)
enum DrumSig { kSig44 = 0, kSig34 = 1, kSig68 = 2 };

// 스타일
enum DrumStyle {
    kStyleBasic = 0,   // 기본(락): 정박 킥/스네어 + 8분 햇
    kStyleEighth = 1,  // 8비트(팝): 엇박 킥이 섞인 그루브
    kStyleBallad = 2,  // 발라드(하프타임): 성글고 여린 패턴
};

inline int drumStyleCount() { return 3; }
inline const char* drumStyleName(int s) {
    switch (s) {
        case kStyleEighth: return "8비트(팝)";
        case kStyleBallad: return "발라드";
        default: return "기본(락)";
    }
}

// 박자표별 한 마디 8분음표 개수 (4/4=8, 3/4=6, 6/8=6).
inline int eighthsPerBar(int sig) {
    if (sig == kSig34) return 6;
    if (sig == kSig68) return 6;
    return 8;
}

// 마디 하나(0~bars-1)만큼 드럼 패턴을 만든다. startTick부터 채운다.
// 8분음표 격자(pos = 0..eighthsPerBar-1)로 배치를 정의하고 틱으로 환산한다.
inline std::vector<DrumHit> generateDrumPattern(int sig, int style, uint32_t ppqn,
                                                int bars, uint32_t startTick = 0) {
    std::vector<DrumHit> out;
    if (ppqn == 0 || bars <= 0) return out;
    const uint32_t e8 = ppqn / 2;          // 8분음표 틱
    const int nE = eighthsPerBar(sig);     // 마디당 8분음표 수
    const uint32_t barTicks = (uint32_t)nE * e8;

    // 한 마디의 배치를 (8분 위치, 노트, 세기)로 모은다.
    struct Cell { int pos; uint8_t note; uint8_t vel; };
    std::vector<Cell> bar;

    auto addHat = [&](int accentEvery) {
        for (int p = 0; p < nE; ++p) {
            const bool accent = accentEvery > 0 && (p % accentEvery == 0);
            bar.push_back({p, kDrumHatClosed, (uint8_t)(accent ? 92 : 72)});
        }
    };

    if (sig == kSig44) {
        if (style == kStyleBallad) {
            // 하프타임: 킥 1박, 스네어 3박, 햇은 8분(여리게)
            bar.push_back({0, kDrumKick, 104});
            bar.push_back({4, kDrumSnare, 96});
            for (int p = 0; p < 8; ++p) bar.push_back({p, kDrumHatClosed, 64});
        } else if (style == kStyleEighth) {
            // 8비트 팝: 킥 1·&2·3, 스네어 2·4, 햇 8분
            bar.push_back({0, kDrumKick, 108});
            bar.push_back({3, kDrumKick, 92});  // "and of 2"
            bar.push_back({4, kDrumKick, 100});
            bar.push_back({2, kDrumSnare, 104});
            bar.push_back({6, kDrumSnare, 104});
            addHat(2);
        } else { // 기본 락: 킥 1·3, 스네어 2·4, 햇 8분
            bar.push_back({0, kDrumKick, 108});
            bar.push_back({4, kDrumKick, 104});
            bar.push_back({2, kDrumSnare, 106});
            bar.push_back({6, kDrumSnare, 106});
            addHat(2);
        }
    } else if (sig == kSig34) {
        // 왈츠 계열: 킥 1박, 스네어 2·3박, 햇은 각 박(4분) 또는 8분
        bar.push_back({0, kDrumKick, 108});
        bar.push_back({2, kDrumSnare, 100});
        bar.push_back({4, kDrumSnare, 100});
        if (style == kStyleBallad) {
            for (int p = 0; p < 6; p += 2) bar.push_back({p, kDrumHatClosed, 66}); // 4분 햇
        } else {
            addHat(2); // 8분 햇 (각 박 강세)
        }
    } else { // 6/8: 둘로 나눠 느끼는 겹박자. 킥 1·4번째 8분, 스네어 4번째, 햇 8분
        bar.push_back({0, kDrumKick, 108});
        bar.push_back({3, kDrumSnare, 102});
        if (style != kStyleBallad) bar.push_back({3, kDrumKick, 84});
        for (int p = 0; p < 6; ++p) {
            const bool accent = (p == 0 || p == 3); // 두 겹박의 첫 8분
            bar.push_back({p, kDrumHatClosed, (uint8_t)(accent ? 90 : 68)});
        }
    }

    for (int b = 0; b < bars; ++b) {
        const uint32_t base = startTick + (uint32_t)b * barTicks;
        // 마디 첫 박에 크래시 한 방 (기본/8비트만, 첫 마디 시작 강조)
        if (b == 0 && style != kStyleBallad)
            out.push_back({base, kDrumCrash, 100});
        for (const auto& c : bar)
            out.push_back({base + (uint32_t)c.pos * e8, c.note, c.vel});
    }
    return out;
}

} // namespace midipro::seq
