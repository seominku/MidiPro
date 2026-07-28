// =============================================================
// MidiPro - vst/VstPreset.cpp
// =============================================================

#include "vst/VstPreset.h"

#include <cstring>

namespace midipro::vst {
namespace {

uint32_t readU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t readU64(const uint8_t* p) {
    return (uint64_t)readU32(p) | ((uint64_t)readU32(p + 4) << 32);
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

} // namespace

std::vector<uint8_t> packState(const uint8_t* comp, std::size_t compSize, const uint8_t* ctrl,
                               std::size_t ctrlSize) {
    std::vector<uint8_t> out;
    out.reserve(12 + compSize + ctrlSize);
    out.push_back('M');
    out.push_back('P');
    out.push_back('S');
    out.push_back('T');
    putU32(out, (uint32_t)compSize);
    if (comp && compSize) out.insert(out.end(), comp, comp + compSize);
    putU32(out, (uint32_t)ctrlSize);
    if (ctrl && ctrlSize) out.insert(out.end(), ctrl, ctrl + ctrlSize);
    return out;
}

bool isAppState(const uint8_t* data, std::size_t size) {
    return data && size >= 12 && std::memcmp(data, "MPST", 4) == 0;
}

bool isVstPreset(const uint8_t* data, std::size_t size) {
    return data && size >= 48 && std::memcmp(data, "VST3", 4) == 0;
}

bool vstPresetToState(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out,
                      std::string* classIdHex) {
    out.clear();
    if (!isVstPreset(data, size)) return false;

    if (classIdHex) classIdHex->assign((const char*)data + 8, 32);

    // 범위 검사는 전부 "빼기"로 한다. a + b > size 로 쓰면 파일에 적힌 큰 수가
    // 넘쳐서 작은 값으로 되돌아가 검사를 통과해 버린다 (손상된 파일 = 잘못된 포인터).
    const uint64_t sz64 = (uint64_t)size;
    const auto fits = [sz64](uint64_t off, uint64_t len) { return off <= sz64 && len <= sz64 - off; };

    const uint64_t listOffset = readU64(data + 40);
    if (!fits(listOffset, 8)) return false; // 목록은 최소 'List'(4) + count(4)

    const uint8_t* list = data + listOffset;
    if (std::memcmp(list, "List", 4) != 0) return false;
    const uint32_t count = readU32(list + 4);
    if (count == 0 || count > 64) return false; // 상식 범위를 벗어나면 손상된 파일
    // 각 항목은 20바이트 (id 4 + offset 8 + size 8)
    if (!fits(listOffset + 8, (uint64_t)count * 20)) return false;

    const uint8_t* comp = nullptr;
    const uint8_t* ctrl = nullptr;
    std::size_t compSize = 0, ctrlSize = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = list + 8 + (std::size_t)i * 20;
        const uint64_t off = readU64(e + 4);
        const uint64_t sz = readU64(e + 12);
        if (!fits(off, sz)) return false; // 파일 밖을 가리키면 손상
        if (std::memcmp(e, "Comp", 4) == 0) {
            comp = data + off;
            compSize = (std::size_t)sz;
        } else if (std::memcmp(e, "Cont", 4) == 0) {
            ctrl = data + off;
            ctrlSize = (std::size_t)sz;
        }
        // "Info"(설명 xml) 등 나머지는 앱이 쓰지 않는다
    }
    if (!comp || compSize == 0) return false; // 컴포넌트 상태가 없으면 음색이 아니다

    out = packState(comp, compSize, ctrl, ctrlSize);
    return true;
}

bool anyPresetToState(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out,
                      std::string* classIdHex) {
    if (isAppState(data, size)) {
        out.assign(data, data + size);
        if (classIdHex) classIdHex->clear();
        return true;
    }
    return vstPresetToState(data, size, out, classIdHex);
}

} // namespace midipro::vst
