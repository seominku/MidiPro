#pragma once
// =============================================================
// MidiPro - sequencer/Song.h
// 곡: 트랙 목록 + 전역 타이밍 정보.
// =============================================================

#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"

#include <cstdint>
#include <vector>

namespace midipro::seq {

struct Song {
    int ppqn = kDefaultPpqn;
    double bpm = 120.0;
    std::vector<Track> tracks;

    uint32_t lengthTicks() const;
};

} // namespace midipro::seq
