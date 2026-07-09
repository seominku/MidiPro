// =============================================================
// MidiPro - sequencer/TimeBase.cpp
// =============================================================

#include "sequencer/TimeBase.h"

namespace midipro::seq {

double ticksPerSecond(double bpm, int ppqn) {
    return (bpm / 60.0) * (double)ppqn;
}

double ticksToSeconds(double ticks, double bpm, int ppqn) {
    return ticks / ticksPerSecond(bpm, ppqn);
}

double secondsToTicks(double seconds, double bpm, int ppqn) {
    return seconds * ticksPerSecond(bpm, ppqn);
}

BarBeatTick toBarBeatTick(uint32_t tick, int ppqn) {
    BarBeatTick result;
    const uint32_t ticksPerBeat = (uint32_t)ppqn;
    const uint32_t ticksPerBar = ticksPerBeat * kBeatsPerBar;
    result.bar = (int)(tick / ticksPerBar) + 1;
    result.beat = (int)((tick % ticksPerBar) / ticksPerBeat) + 1;
    result.tick = (int)(tick % ticksPerBeat);
    return result;
}

} // namespace midipro::seq
