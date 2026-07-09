#pragma once
// =============================================================
// MidiPro - sequencer/Track.h
// 트랙: 이벤트 목록 + 표시용 속성.
//
// 책임 분리 (Rule 1의 SRP):
//   Track은 이벤트 "저장"만 담당한다. 파일 저장은 SmfFile,
//   재생은 Player, 화면 표시는 gui가 각각 맡는다.
// =============================================================

#include "sequencer/MidiEvent.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midipro::seq {

struct Track {
    std::string name = "Track";
    uint8_t channel = 0; // 0~15
    bool muted = false;
    std::vector<MidiEvent> events; // tick 오름차순 유지

    // Note On/Off 쌍을 한 번에 추가한다 (틱 순서는 sortEvents로 보장)
    void addNote(uint32_t tick, uint32_t durationTicks, uint8_t note, uint8_t velocity);
    void addProgramChange(uint32_t tick, uint8_t program);
    void sortEvents();
    uint32_t lengthTicks() const; // 마지막 이벤트의 틱
};

// 피아노 롤 표시용: Note On/Off 쌍을 묶은 구간
struct NoteSpan {
    uint32_t startTick = 0;
    uint32_t endTick = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
};

// 이벤트 목록에서 노트 구간을 추출한다.
// 왜 분리했는가: 순수 로직이라 GUI 없이 테스트 가능해야 해서 (Rule 6).
std::vector<NoteSpan> extractNotes(const Track& track);

// 주어진 구간과 일치하는 Note On/Off 쌍을 트랙에서 제거한다.
// (피아노 롤 편집의 노트 삭제에 쓰인다.) 제거하면 true.
bool removeNote(Track& track, const NoteSpan& span);

// (note, tick) 지점을 포함하는 노트 구간을 찾는다. 없으면 found=false.
NoteSpan noteSpanAt(const Track& track, uint8_t note, uint32_t tick, bool& found);

} // namespace midipro::seq
