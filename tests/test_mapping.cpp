// =============================================================
// MidiPro - tests/test_mapping.cpp
// MIDI Learn 매핑 순수 로직 테스트 (Rule 6):
//   값 스케일링, 1:1 바인딩 규칙, 직렬화 왕복.
// =============================================================

#include "mapping/MidiMap.h"

#include <cmath>
#include <iostream>

using namespace midipro;
using mapping::ParamTarget;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

void testNormalizedScaling() {
    audio::SynthParams p;
    // Cutoff 범위 50~12000. norm 0 -> 50, 1 -> 12000, 0.5 -> 6025
    mapping::applyNormalized(p, ParamTarget::FilterCutoff, 0.0f);
    CHECK(std::fabs(p.filterCutoff - 50.0f) < 0.1f);
    mapping::applyNormalized(p, ParamTarget::FilterCutoff, 1.0f);
    CHECK(std::fabs(p.filterCutoff - 12000.0f) < 0.1f);
    mapping::applyNormalized(p, ParamTarget::FilterCutoff, 0.5f);
    CHECK(std::fabs(p.filterCutoff - 6025.0f) < 1.0f);

    // 역산 왕복
    mapping::applyNormalized(p, ParamTarget::MasterVolume, 0.3f);
    CHECK(std::fabs(mapping::readNormalized(p, ParamTarget::MasterVolume) - 0.3f) < 1e-4);

    // 범위 밖 입력은 클램프
    mapping::applyNormalized(p, ParamTarget::Sustain, 2.0f);
    CHECK(std::fabs(p.adsr.sustain - 1.0f) < 1e-4);
}

void testBindingIsOneToOne() {
    mapping::MidiMap map;
    map.bind(74, 0, ParamTarget::FilterCutoff);
    map.bind(71, 0, ParamTarget::FilterResonance);
    CHECK(map.list().size() == 2);

    ParamTarget t;
    CHECK(map.findTarget(74, t) && t == ParamTarget::FilterCutoff);
    CHECK(map.ccForTarget(ParamTarget::FilterResonance) == 71);

    // 같은 CC를 다른 파라미터에 재학습하면 기존 CC 매핑을 교체
    map.bind(74, 0, ParamTarget::Attack);
    CHECK(map.findTarget(74, t) && t == ParamTarget::Attack);
    CHECK(map.ccForTarget(ParamTarget::FilterCutoff) == -1); // 예전 대상은 풀림
    CHECK(map.list().size() == 2);

    // 같은 파라미터를 다른 CC로 재학습하면 그 파라미터의 기존 CC를 교체
    map.bind(20, 0, ParamTarget::Attack);
    CHECK(map.ccForTarget(ParamTarget::Attack) == 20);
    CHECK(!map.findTarget(74, t)); // CC74는 이제 아무 데도 안 붙음

    map.clearTarget(ParamTarget::Attack);
    CHECK(map.ccForTarget(ParamTarget::Attack) == -1);
}

void testSerializeRoundTrip() {
    mapping::MidiMap map;
    map.bind(74, 0, ParamTarget::FilterCutoff);
    map.bind(71, 2, ParamTarget::LfoDepth);
    map.bind(7, 0, ParamTarget::MasterVolume);

    const std::string text = map.serialize();
    mapping::MidiMap loaded;
    CHECK(loaded.deserialize(text));
    CHECK(loaded.list().size() == 3);
    CHECK(loaded.ccForTarget(ParamTarget::FilterCutoff) == 74);
    CHECK(loaded.ccForTarget(ParamTarget::LfoDepth) == 71);
    CHECK(loaded.channelForTarget(ParamTarget::LfoDepth) == 2);

    // 우리 포맷이 아니면 거부
    mapping::MidiMap bad;
    CHECK(!bad.deserialize("not our format"));
}

} // namespace

int main() {
    testNormalizedScaling();
    testBindingIsOneToOne();
    testSerializeRoundTrip();

    if (g_failures == 0) {
        std::cout << "[OK] mapping tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
