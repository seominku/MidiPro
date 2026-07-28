#pragma once
// =============================================================
// MidiPro - gui/DrumClassify.h
// 드럼 샘플 파일 경로를 악기(킥/스네어/햇...)로 분류한다.
//
// 왜 헤더로 뺐나 (Rule 1, 6):
//   이름만 보고 악기를 맞히는 순수 문자열 로직이라 GUI 없이 단위 테스트한다.
//   드럼 샘플 브라우저의 "악기별" 탭과 MCP 서버의 자동 배정이 같은 규칙을 쓴다.
//
// 낱말 단위로 보는 이유:
//   단순 부분 문자열이면 "Bottoms Up"이 tom으로, "Bell"이 bd로 잡혀서 목록에
//   엉뚱한 샘플이 섞인다. 사람이 눈으로 거를 때는 성가신 정도지만, 이름만 보고
//   자동 배정할 때는 베이스 샘플이 미드 탐 자리에 앉는다.
//
// 약어를 받는 이유:
//   실제 라이브러리는 "Ac2 Rd Bell"(라이드), "Ac2 Kik Mt"(킥)처럼 줄여 쓴다.
//   ride/kick만 찾으면 이런 샘플이 목록에서 통째로 빠진다.
// =============================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace midipro::gui {

enum : uint16_t {
    kBKick = 1 << 0,
    kBSnare = 1 << 1,
    kBClap = 1 << 2,
    kBHatClosed = 1 << 3,
    kBHatOpen = 1 << 4,
    kBTom = 1 << 5,
    kBCrash = 1 << 6,
    kBRide = 1 << 7,
    kBEtc = 1 << 8,
};

// s에 kw가 "낱말로" 들어 있는가. 앞뒤가 글자면(= 낱말 가운데면) 아니라고 본다.
// 끝의 s 하나는 복수형으로 봐준다 (toms, hats).
// numBoundary=true면 숫자도 경계로 친다 — 약어용("3rd"가 rd로 잡히지 않게).
inline bool drumWordAt(const std::string& s, const char* kw, bool numBoundary) {
    const std::size_t n = std::strlen(kw);
    if (n == 0) return false;
    const auto isBody = [numBoundary](char c) {
        return (c >= 'a' && c <= 'z') || (numBoundary && c >= '0' && c <= '9');
    };
    for (std::size_t p = s.find(kw); p != std::string::npos; p = s.find(kw, p + 1)) {
        std::size_t e = p + n;
        if (e < s.size() && s[e] == 's') ++e; // 복수형
        const bool leftOk = (p == 0) || !isBody(s[p - 1]);
        const bool rightOk = (e >= s.size()) || !isBody(s[e]);
        if (leftOk && rightOk) return true;
    }
    return false;
}

// 경로/파일 이름 -> 악기 비트마스크. 여러 악기에 걸릴 수 있다(모호한 햇 등).
inline uint16_t classifyDrumPath(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    const auto w = [&](const char* kw) { return drumWordAt(s, kw, false); };
    const auto ab = [&](const char* kw) { return drumWordAt(s, kw, true); };
    const auto has = [&](const char* kw) { return s.find(kw) != std::string::npos; };

    uint16_t m = 0;
    if (w("clap")) m |= kBClap;
    if (w("snare") || ab("snr") || ab("sn")) m |= kBSnare;
    if (w("rim")) m |= kBSnare; // 림도 스네어 줄에서 쓴다
    if (w("kick") || ab("kik") || has("bassdrum") || has("bass drum") || has("bass-drum") ||
        ab("bd"))
        m |= kBKick;
    if (w("hat") || has("hihat") || has("hi-hat") || ab("hh") || ab("chh") || ab("ohh")) {
        if (has("open") || ab("ohh")) m |= kBHatOpen;
        else if (has("close") || ab("cls") || ab("chh")) m |= kBHatClosed;
        else m |= (uint16_t)(kBHatClosed | kBHatOpen); // 모호하면 둘 다에 보인다
    }
    if (w("tom")) m |= kBTom;
    if (w("crash") || ab("crsh") || ab("crs")) m |= kBCrash;
    if (w("ride") || ab("rd")) m |= kBRide;
    if (w("cymbal") || ab("cym")) {
        if (!(m & (kBCrash | kBRide))) m |= (uint16_t)(kBCrash | kBRide);
    }
    if (m == 0) m = kBEtc;
    return m;
}

} // namespace midipro::gui
