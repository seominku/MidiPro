// =============================================================
// MidiPro - sequencer/Song.cpp
// =============================================================

#include "sequencer/Song.h"

namespace midipro::seq {

uint32_t Song::lengthTicks() const {
    uint32_t longest = 0;
    for (const auto& t : tracks) {
        const uint32_t len = t.lengthTicks();
        if (len > longest) longest = len;
    }
    return longest;
}

} // namespace midipro::seq
