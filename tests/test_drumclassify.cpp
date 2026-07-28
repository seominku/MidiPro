// =============================================================
// MidiPro - tests/test_drumclassify.cpp
// 드럼 샘플 이름 -> 악기 분류 (gui/DrumClassify.h)
//
// 실제 라이브러리(src/Drum, 드럼머신 아카이브)의 이름 짓기를 표본으로 삼는다.
// =============================================================

#include "gui/DrumClassify.h"

#include <cstdio>
#include <string>

using namespace midipro::gui;

static int g_fail = 0;

static void expectHas(const char* path, uint16_t bucket, const char* what) {
    const uint16_t m = classifyDrumPath(path);
    if (!(m & bucket)) {
        std::printf("[FAIL] \"%s\" 가 %s 로 안 잡힘 (mask=0x%X)\n", path, what, m);
        ++g_fail;
    }
}

static void expectNot(const char* path, uint16_t bucket, const char* what) {
    const uint16_t m = classifyDrumPath(path);
    if (m & bucket) {
        std::printf("[FAIL] \"%s\" 가 %s 로 잘못 잡힘 (mask=0x%X)\n", path, what, m);
        ++g_fail;
    }
}

int main() {
    // ---- 기본 낱말 ----
    expectHas("Acoustic-Kick-Ac2 Kick.wav", kBKick, "킥");
    expectHas("TR909/Snares/Snare-01.wav", kBSnare, "스네어");
    expectHas("Perc/Clap-02.wav", kBClap, "클랩");
    expectHas("Toms/Tom-03.wav", kBTom, "탐");
    expectHas("Cymbals/Crash-01.wav", kBCrash, "크래시");
    expectHas("Cymbals/Ride-01.wav", kBRide, "라이드");
    expectHas("Hats/ClosedHat-01.wav", kBHatClosed, "클로즈드 햇");
    expectHas("Hats/OpenHat-01.wav", kBHatOpen, "오픈 햇");
    expectHas("Bassdrum-01.wav", kBKick, "킥");

    // ---- 낱말 단위: 낱말 가운데에 들어간 것은 아니어야 한다 ----
    // 실제 라이브러리에 있는 이름. 예전 규칙(부분 문자열)에서는 탐으로 잡혔다.
    expectNot("HipHop-Bass-MSXII Bottoms Up 01.wav", kBTom, "탐");
    expectNot("Perc/Bottom Snap.wav", kBTom, "탐");
    expectNot("Bells/Bell Tree.wav", kBKick, "킥");        // "bell" 안의 bd 아님
    expectNot("Vocal/Abduction Hit.wav", kBKick, "킥");     // "abd"uction
    expectNot("Perc/3rd Take Shaker.wav", kBRide, "라이드"); // "3rd" — 숫자 경계
    expectNot("Loops/Snap Roll.wav", kBSnare, "스네어");     // "sn"ap 아님
    expectNot("Perc/Kicker Sound.wav", kBKick, "킥");        // kick"er" — 낱말 아님

    // ---- 복수형은 받아준다 ----
    expectHas("Toms/Low.wav", kBTom, "탐");
    expectHas("Hats/Foo.wav", (uint16_t)(kBHatClosed | kBHatOpen), "햇");
    expectHas("Kicks/Deep.wav", kBKick, "킥");

    // ---- 약어: 실제 라이브러리가 줄여 쓰는 이름 ----
    // 예전 규칙에서는 이 라이드 샘플들이 목록에 아예 안 떴다.
    expectHas("Acoustic-Cymbal-Ac2 Rd Bell.wav", kBRide, "라이드");
    expectHas("Acoustic-Cymbal-Ac2 Rd Bow.wav", kBRide, "라이드");
    expectHas("Acoustic-Kick-Ac2 Kik Mt.wav", kBKick, "킥");
    expectHas("Acoustic-Snare-Ac2 Sn Cnt.wav", kBSnare, "스네어");
    expectHas("Acoustic-Cymbal-Ac2 Crsh 1.wav", kBCrash, "크래시");
    expectHas("Kit/BD 01.wav", kBKick, "킥");
    expectHas("Kit/HH Closed.wav", kBHatClosed, "클로즈드 햇");
    expectHas("Kit/OHH 02.wav", kBHatOpen, "오픈 햇");

    // ---- 햇 열림/닫힘 구분 ----
    expectNot("Hats/Closed Hat 01.wav", kBHatOpen, "오픈 햇");
    expectNot("Hats/Open Hat 01.wav", kBHatClosed, "클로즈드 햇");
    expectHas("Hats/Hat 01.wav", (uint16_t)(kBHatClosed | kBHatOpen), "양쪽 햇"); // 모호하면 둘 다

    // ---- 심벌: 이름이 정확한 쪽이 먼저, 막연하면 둘 다 ----
    expectHas("Cymbals/Cymbal 01.wav", (uint16_t)(kBCrash | kBRide), "크래시+라이드");
    expectNot("Cymbals/Ride Cymbal.wav", kBCrash, "크래시"); // ride가 이미 잡혔으면 둘 다 안 넣는다

    // ---- 림은 스네어 줄에서 쓴다 ----
    expectHas("Perc/Rim-01.wav", kBSnare, "스네어");

    // ---- 아무 데도 안 걸리면 기타 ----
    expectHas("Misc/Zap 01.wav", kBEtc, "기타/퍼커션");
    expectNot("Misc/Zap 01.wav", kBKick, "킥");

    if (g_fail) {
        std::printf("[FAIL] drum classify tests failed (%d)\n", g_fail);
        return 1;
    }
    std::printf("[OK] drum classify tests passed\n");
    return 0;
}
