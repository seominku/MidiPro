// =============================================================
// MidiPro - guitar/Fretboard.cpp
// =============================================================

#include "guitar/Fretboard.h"

namespace midipro::guitar {

uint8_t noteAt(int stringIndex, int fret, const uint8_t (&tuning)[kStringCount]) {
    const int note = tuning[stringIndex] + fret;
    return (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
}

std::vector<FretPosition> positionsForNote(uint8_t note, int maxFret,
                                           const uint8_t (&tuning)[kStringCount]) {
    std::vector<FretPosition> positions;
    for (int s = 0; s < kStringCount; ++s) {
        const int fret = (int)note - (int)tuning[s];
        if (fret >= 0 && fret <= maxFret) {
            positions.push_back({s, fret});
        }
    }
    return positions;
}

const std::vector<ChordShape>& commonChords() {
    // 정적 초기화로 한 번만 만든다. 반음 간격은 루트 기준.
    static const std::vector<ChordShape> chords = {
        {"Major", {0, 4, 7}},        {"Minor", {0, 3, 7}},
        {"Dom7", {0, 4, 7, 10}},     {"Maj7", {0, 4, 7, 11}},
        {"Min7", {0, 3, 7, 10}},     {"Sus4", {0, 5, 7}},
        {"Power5", {0, 7}},          {"Dim", {0, 3, 6}},
    };
    return chords;
}

const char* pitchClassName(int pitchClass) {
    static const char* names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};
    const int index = ((pitchClass % 12) + 12) % 12;
    return names[index];
}

} // namespace midipro::guitar
