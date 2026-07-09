// =============================================================
// MidiPro - sequencer/Track.cpp
// =============================================================

#include "sequencer/Track.h"

#include <algorithm>

namespace midipro::seq {

void Track::addNote(uint32_t tick, uint32_t durationTicks, uint8_t note, uint8_t velocity) {
    MidiEvent on;
    on.tick = tick;
    on.status = (uint8_t)(midi::kStatusNoteOn | (channel & midi::kChannelMask));
    on.data1 = note & midi::kDataMask;
    on.data2 = velocity & midi::kDataMask;
    events.push_back(on);

    MidiEvent off;
    off.tick = tick + durationTicks;
    off.status = (uint8_t)(midi::kStatusNoteOff | (channel & midi::kChannelMask));
    off.data1 = note & midi::kDataMask;
    off.data2 = 0;
    events.push_back(off);
}

void Track::addProgramChange(uint32_t tick, uint8_t program) {
    MidiEvent pc;
    pc.tick = tick;
    pc.status = (uint8_t)(midi::kStatusProgramChange | (channel & midi::kChannelMask));
    pc.data1 = program & midi::kDataMask;
    events.push_back(pc);
}

void Track::sortEvents() {
    // 왜 stable_sort인가: 같은 틱에서는 추가된 순서(예: Note Off가
    // 먼저 들어간 경우)를 보존해 재생 순서가 뒤바뀌지 않게 한다.
    std::stable_sort(events.begin(), events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
}

uint32_t Track::lengthTicks() const {
    uint32_t last = 0;
    for (const auto& e : events)
        if (e.tick > last) last = e.tick;
    return last;
}

std::vector<NoteSpan> extractNotes(const Track& track) {
    std::vector<NoteSpan> spans;
    // 진행 중(off를 아직 못 만난) 노트의 spans 인덱스
    std::vector<std::size_t> open;

    for (const auto& e : track.events) {
        if (e.isNoteOn()) {
            NoteSpan span;
            span.startTick = e.tick;
            span.endTick = e.tick; // off를 만나면 갱신
            span.note = e.data1;
            span.velocity = e.data2;
            open.push_back(spans.size());
            spans.push_back(span);
        } else if (e.isNoteOff()) {
            // 같은 음높이의 가장 오래된 열린 노트를 닫는다 (FIFO 매칭)
            for (auto it = open.begin(); it != open.end(); ++it) {
                if (spans[*it].note == e.data1) {
                    spans[*it].endTick = e.tick;
                    open.erase(it);
                    break;
                }
            }
        }
    }

    // off가 없는 노트는 트랙 끝까지 이어진 것으로 처리
    const uint32_t end = track.lengthTicks();
    for (std::size_t index : open)
        spans[index].endTick = end;
    return spans;
}

bool removeNote(Track& track, const NoteSpan& span) {
    // 시작 틱의 Note On 하나와 끝 틱의 Note Off 하나를 제거한다.
    // 한 번씩만 지워 같은 음높이의 다른 노트를 건드리지 않는다.
    bool removedOn = false;
    bool removedOff = false;
    auto& events = track.events;
    for (auto it = events.begin(); it != events.end();) {
        const bool matchOn =
            !removedOn && it->isNoteOn() && it->data1 == span.note && it->tick == span.startTick;
        const bool matchOff =
            !removedOff && it->isNoteOff() && it->data1 == span.note && it->tick == span.endTick;
        if (matchOn) {
            removedOn = true;
            it = events.erase(it);
        } else if (matchOff) {
            removedOff = true;
            it = events.erase(it);
        } else {
            ++it;
        }
    }
    return removedOn; // On을 지웠으면 삭제로 간주 (Off는 없을 수도 있음)
}

NoteSpan noteSpanAt(const Track& track, uint8_t note, uint32_t tick, bool& found) {
    const auto spans = extractNotes(track);
    for (const auto& s : spans) {
        if (s.note == note && tick >= s.startTick && tick < s.endTick) {
            found = true;
            return s;
        }
    }
    found = false;
    return {};
}

} // namespace midipro::seq
