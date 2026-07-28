// =============================================================
// MidiPro - tests/test_vstpreset.cpp
// 프리셋 파일 <-> 앱 상태 블롭 변환 (vst/VstPreset.h)
//
// 플러그인 없이 바이트만 다루므로 여기서 전부 검사할 수 있다.
// =============================================================

#include "vst/VstPreset.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace midipro::vst;

static int g_fail = 0;

static void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("[FAIL] %s\n", what);
        ++g_fail;
    }
}

static void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}
static void putU64(std::vector<uint8_t>& v, uint64_t x) {
    putU32(v, (uint32_t)(x & 0xFFFFFFFFu));
    putU32(v, (uint32_t)(x >> 32));
}

// 진짜 .vstpreset 처럼 생긴 바이트를 만든다.
static std::vector<uint8_t> makePreset(const std::vector<uint8_t>& comp,
                                       const std::vector<uint8_t>& ctrl, bool withInfo = true) {
    std::vector<uint8_t> f;
    f.insert(f.end(), {'V', 'S', 'T', '3'});
    putU32(f, 1);
    const char* cid = "56535458747362737572676578740000"; // 32자 16진 ASCII
    f.insert(f.end(), cid, cid + 32);
    const std::size_t listOffsetPos = f.size();
    putU64(f, 0); // 나중에 채운다

    const uint64_t compOff = f.size();
    f.insert(f.end(), comp.begin(), comp.end());
    const uint64_t ctrlOff = f.size();
    f.insert(f.end(), ctrl.begin(), ctrl.end());
    const uint64_t infoOff = f.size();
    const std::string info = "<xml/>";
    if (withInfo) f.insert(f.end(), info.begin(), info.end());

    const uint64_t listOff = f.size();
    f.insert(f.end(), {'L', 'i', 's', 't'});
    putU32(f, withInfo ? 3u : 2u);
    f.insert(f.end(), {'C', 'o', 'm', 'p'});
    putU64(f, compOff);
    putU64(f, comp.size());
    f.insert(f.end(), {'C', 'o', 'n', 't'});
    putU64(f, ctrlOff);
    putU64(f, ctrl.size());
    if (withInfo) {
        f.insert(f.end(), {'I', 'n', 'f', 'o'});
        putU64(f, infoOff);
        putU64(f, info.size());
    }
    // listOffset 채우기
    for (int i = 0; i < 8; ++i) f[listOffsetPos + (std::size_t)i] = (uint8_t)((listOff >> (i * 8)) & 0xFF);
    return f;
}

int main() {
    const std::vector<uint8_t> comp = {1, 2, 3, 4, 5, 6, 7};
    const std::vector<uint8_t> ctrl = {9, 8, 7};

    // ---- packState: 앱 포맷 ----
    {
        const auto b = packState(comp.data(), comp.size(), ctrl.data(), ctrl.size());
        expect(b.size() == 12 + comp.size() + ctrl.size(), "packState 크기");
        expect(std::memcmp(b.data(), "MPST", 4) == 0, "packState 머리글");
        expect(b[4] == 7 && b[5] == 0, "compSize 리틀엔디언");
        expect(std::memcmp(b.data() + 8, comp.data(), comp.size()) == 0, "comp 바이트 보존");
        expect(std::memcmp(b.data() + 8 + comp.size() + 4, ctrl.data(), ctrl.size()) == 0,
               "ctrl 바이트 보존");
        expect(isAppState(b.data(), b.size()), "isAppState");
        expect(!isVstPreset(b.data(), b.size()), "MPST는 vstpreset이 아니다");
    }
    // 컨트롤러 상태가 없어도 된다
    {
        const auto b = packState(comp.data(), comp.size(), nullptr, 0);
        expect(b.size() == 12 + comp.size(), "ctrl 없는 packState 크기");
        expect(b[8 + comp.size()] == 0, "ctrlSize 0");
    }

    // ---- .vstpreset 읽기 ----
    {
        const auto f = makePreset(comp, ctrl);
        expect(isVstPreset(f.data(), f.size()), "isVstPreset");
        std::vector<uint8_t> out;
        std::string cid;
        expect(vstPresetToState(f.data(), f.size(), out, &cid), "vstpreset 변환 성공");
        expect(cid == "56535458747362737572676578740000", "classId 읽기");
        const auto want = packState(comp.data(), comp.size(), ctrl.data(), ctrl.size());
        expect(out == want, "Comp/Cont가 MPST로 정확히 옮겨진다");
    }
    // Info 덩어리가 없어도 된다
    {
        const auto f = makePreset(comp, ctrl, /*withInfo=*/false);
        std::vector<uint8_t> out;
        expect(vstPresetToState(f.data(), f.size(), out, nullptr), "Info 없는 프리셋도 읽는다");
    }
    // Cont 없이 Comp만 있어도 된다
    {
        const auto f = makePreset(comp, {}, false);
        std::vector<uint8_t> out;
        expect(vstPresetToState(f.data(), f.size(), out, nullptr), "Cont 없는 프리셋도 읽는다");
        expect(out.size() == 12 + comp.size(), "ctrl 0으로 채워진다");
    }

    // ---- 손상/이상한 입력은 거부 ----
    {
        std::vector<uint8_t> out;
        expect(!vstPresetToState(nullptr, 0, out), "널 입력 거부");
        const std::vector<uint8_t> junk = {'X', 'X', 'X', 'X', 1, 2, 3};
        expect(!vstPresetToState(junk.data(), junk.size(), out), "머리글 다르면 거부");
        expect(!isVstPreset(junk.data(), junk.size()), "isVstPreset 거짓");

        auto f = makePreset(comp, ctrl);
        // 목록 위치를 파일 밖으로
        for (int i = 0; i < 8; ++i) f[40 + (std::size_t)i] = 0xFF;
        expect(!vstPresetToState(f.data(), f.size(), out), "목록 위치가 파일 밖이면 거부");

        auto f2 = makePreset(comp, ctrl);
        f2.resize(f2.size() - 10); // 뒤를 잘라 목록을 깬다
        expect(!vstPresetToState(f2.data(), f2.size(), out), "잘린 파일 거부");

        // Comp 덩어리가 파일 밖을 가리키게
        auto f3 = makePreset(comp, ctrl);
        const std::size_t listOff = f3.size() - (4 + 4 + 20 * 3);
        for (int i = 0; i < 8; ++i) f3[listOff + 8 + 4 + (std::size_t)i] = 0xFF; // Comp offset
        expect(!vstPresetToState(f3.data(), f3.size(), out), "덩어리가 파일 밖이면 거부");
    }

    // ---- anyPresetToState: 두 형식 모두 ----
    {
        std::vector<uint8_t> out;
        const auto app = packState(comp.data(), comp.size(), ctrl.data(), ctrl.size());
        expect(anyPresetToState(app.data(), app.size(), out), "MPST 통과");
        expect(out == app, "MPST는 그대로 복사");
        const auto f = makePreset(comp, ctrl);
        expect(anyPresetToState(f.data(), f.size(), out), "vstpreset 통과");
        expect(isAppState(out.data(), out.size()), "결과는 MPST");
        const std::vector<uint8_t> junk = {'n', 'o', 'p', 'e'};
        expect(!anyPresetToState(junk.data(), junk.size(), out), "모르는 형식 거부");
    }

    if (g_fail) {
        std::printf("[FAIL] vst preset tests failed (%d)\n", g_fail);
        return 1;
    }
    std::printf("[OK] vst preset tests passed\n");
    return 0;
}
