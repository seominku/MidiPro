#pragma once
// =============================================================
// MidiPro - sequencer/TimeBase.h
// 타이밍 엔진의 기본 단위 변환 (BPM, 틱, 초).
//
// 왜 틱(tick) 기반인가:
//   이벤트 시각을 초로 저장하면 BPM을 바꿀 때마다 전부 다시
//   계산해야 한다. 틱(PPQN 기준 음악적 시간)으로 저장하면
//   BPM은 재생 시점에만 곱해지는 계수가 된다. SMF 포맷도
//   틱 기반이라 파일 입출력과도 맞아떨어진다.
//
// 순수 함수만 있으므로 유닛 테스트 대상 (Rule 6).
// =============================================================

#include <cstdint>

namespace midipro::seq {

// PPQN(Pulses Per Quarter Note): 4분음표 하나를 쪼개는 틱 수.
// 왜 480인가: 3/5/7연음까지 정수로 떨어지는 관례적 해상도.
inline constexpr int kDefaultPpqn = 480;

// Phase 2에서는 4/4 박자로 고정한다. (박자표 변경은 확장 지점)
inline constexpr int kBeatsPerBar = 4;

// 초당 틱 수 = (bpm / 60) * ppqn
double ticksPerSecond(double bpm, int ppqn);

double ticksToSeconds(double ticks, double bpm, int ppqn);
double secondsToTicks(double seconds, double bpm, int ppqn);

// 마디:박:틱 표시용 (1부터 시작, 4/4 가정)
struct BarBeatTick {
    int bar = 1;
    int beat = 1;
    int tick = 0;
};

BarBeatTick toBarBeatTick(uint32_t tick, int ppqn);

} // namespace midipro::seq
