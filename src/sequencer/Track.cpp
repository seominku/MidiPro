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
    ++editStamp; // 노트 캐시 무효화 (GUI가 프레임마다 추출을 반복하지 않게)
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
    if (removedOn) {
        disownNoteFromClips(track, span.note, span.startTick); // 클립 소속에서도 뺀다
        ++track.editStamp;
    }
    return removedOn; // On을 지웠으면 삭제로 간주 (Off는 없을 수도 있음)
}

bool setNoteVelocity(Track& track, const NoteSpan& span, uint8_t velocity) {
    if (velocity == 0) velocity = 1; // 벨로시티 0은 Note Off 의미라 피한다
    if (velocity > 127) velocity = 127;
    for (auto& e : track.events) {
        if (e.isNoteOn() && e.data1 == span.note && e.tick == span.startTick) {
            e.data2 = velocity;
            ++track.editStamp; // 세기도 표시(색)에 쓰여 캐시를 깨야 한다
            return true;
        }
    }
    return false;
}

int quantizeTrack(Track& track, uint32_t gridTicks) {
    if (gridTicks == 0) return 0;
    const auto spans = extractNotes(track);
    int changed = 0;
    for (const auto& s : spans) {
        const uint32_t q = ((s.startTick + gridTicks / 2) / gridTicks) * gridTicks; // 반올림
        if (q == s.startTick) continue;
        const uint32_t dur = s.endTick > s.startTick ? s.endTick - s.startTick : 1;
        removeNote(track, s);
        track.addNote(q, dur, s.note, s.velocity);
        adoptNoteIntoClips(track, s.note, q); // 클립 안이면 소속 유지
        ++changed;
    }
    if (changed > 0) track.sortEvents();
    return changed;
}

int humanizeTrack(Track& track, uint32_t jitterTicks, int jitterVel, uint32_t seed,
                  const std::vector<NoteSpan>& onlyNotes) {
    if (jitterTicks == 0 && jitterVel == 0) return 0;
    const auto spans = onlyNotes.empty() ? extractNotes(track) : onlyNotes;
    // 간단한 LCG: <random> 없이도 seed가 같으면 결과가 같다 (테스트 가능)
    uint32_t rng = seed | 1u;
    const auto jitter = [&rng](int range) {
        if (range <= 0) return 0;
        rng = rng * 1664525u + 1013904223u;
        return (int)(rng % (uint32_t)(range * 2 + 1)) - range;
    };
    int changed = 0;
    for (const auto& s : spans) {
        const int dt = jitter((int)jitterTicks);
        const int dv = jitter(jitterVel);
        if (dt == 0 && dv == 0) continue;
        const uint32_t dur = s.endTick > s.startTick ? s.endTick - s.startTick : 1;
        const int64_t ns = (int64_t)s.startTick + dt;
        const int nv = s.velocity + dv;
        removeNote(track, s);
        const uint32_t nt = (uint32_t)(ns < 0 ? 0 : ns);
        track.addNote(nt, dur, s.note, (uint8_t)(nv < 1 ? 1 : (nv > 127 ? 127 : nv)));
        adoptNoteIntoClips(track, s.note, nt); // 클립 안이면 소속 유지
        ++changed;
    }
    if (changed > 0) track.sortEvents();
    return changed;
}

float autoValueAt(const std::vector<Track::AutoPoint>& pts, uint32_t tick, float fallback) {
    if (pts.empty()) return fallback;
    if (tick <= pts.front().tick) return pts.front().value;
    if (tick >= pts.back().tick) return pts.back().value;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (tick > pts[i].tick) continue;
        const auto& a = pts[i - 1];
        const auto& b = pts[i];
        if (b.tick <= a.tick) return b.value; // 같은 틱에 겹친 점
        const float f = (float)(tick - a.tick) / (float)(b.tick - a.tick);
        return a.value + (b.value - a.value) * f; // 선형 보간
    }
    return pts.back().value;
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

// ---- MIDI 구간 조작 (노트 = 스팬 단위, On/Off 짝 보존) ----

void shiftMidiRange(Track& track, uint32_t start, uint32_t end, long dTick) {
    if (dTick == 0 || end <= start) return;
    const auto notes = extractNotes(track);
    std::vector<NoteSpan> moved;
    for (const auto& n : notes)
        if (n.startTick >= start && n.startTick < end) moved.push_back(n);
    for (const auto& n : moved) removeNote(track, n);
    for (auto& e : track.events) { // 남은 노트 이벤트는 범위 밖 노트들 것
        if (e.isNoteOn() || e.isNoteOff()) continue;
        if (e.tick >= start && e.tick < end) {
            const long nt = (long)e.tick + dTick;
            e.tick = (uint32_t)(nt < 0 ? 0 : nt);
        }
    }
    for (const auto& n : moved) {
        const long ns = (long)n.startTick + dTick;
        track.addNote((uint32_t)(ns < 0 ? 0 : ns),
                      n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                      n.velocity);
    }
    track.sortEvents();
}

void copyMidiRange(Track& track, uint32_t start, uint32_t end, uint32_t dTick) {
    if (end <= start) return;
    const auto notes = extractNotes(track);
    std::vector<MidiEvent> addEv;
    for (const auto& e : track.events) {
        if (e.isNoteOn() || e.isNoteOff()) continue;
        if (e.tick >= start && e.tick < end) {
            MidiEvent c = e;
            c.tick += dTick;
            addEv.push_back(c);
        }
    }
    track.events.insert(track.events.end(), addEv.begin(), addEv.end());
    for (const auto& n : notes)
        if (n.startTick >= start && n.startTick < end)
            track.addNote(n.startTick + dTick,
                          n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                          n.velocity);
    track.sortEvents();
}

void eraseMidiRange(Track& track, uint32_t start, uint32_t end) {
    if (end <= start) return;
    const auto notes = extractNotes(track);
    for (const auto& n : notes)
        if (n.startTick >= start && n.startTick < end) removeNote(track, n);
    track.events.erase(std::remove_if(track.events.begin(), track.events.end(),
                                      [&](const MidiEvent& e) {
                                          return !e.isNoteOn() && !e.isNoteOff() &&
                                                 e.tick >= start && e.tick < end;
                                      }),
                       track.events.end());
}

// ---- MIDI 클립 (소유 노트 기준) ----

namespace {
bool isMember(const MidiClip& clip, uint8_t note, uint32_t tick) {
    for (const auto& m : clip.members)
        if (m.first == note && m.second == tick) return true;
    return false;
}
// 살아 있는 멤버 노트 스팬들 (피아노 롤에서 지워진 멤버는 자동 제외)
std::vector<NoteSpan> aliveMembers(const Track& track, const MidiClip& clip) {
    std::vector<NoteSpan> out;
    for (const auto& n : extractNotes(track))
        if (isMember(clip, n.note, n.startTick)) out.push_back(n);
    return out;
}
} // namespace

void adoptNoteIntoClips(Track& track, uint8_t note, uint32_t tick) {
    for (auto& mc : track.midiClips)
        if (tick >= mc.startTick && tick < mc.endTick) {
            if (!isMember(mc, note, tick)) mc.members.push_back({note, tick});
            return; // 겹치면 앞선 클립 소유
        }
}

void disownNoteFromClips(Track& track, uint8_t note, uint32_t tick) {
    for (auto& mc : track.midiClips)
        mc.members.erase(std::remove_if(mc.members.begin(), mc.members.end(),
                                        [&](const std::pair<uint8_t, uint32_t>& m) {
                                            return m.first == note && m.second == tick;
                                        }),
                         mc.members.end());
}

void adoptMidiClipMembers(const Track& track, MidiClip& clip) {
    clip.members.clear();
    for (const auto& n : extractNotes(track))
        if (n.startTick >= clip.startTick && n.startTick < clip.endTick)
            clip.members.push_back({n.note, n.startTick});
}

void shiftMidiClip(Track& track, MidiClip& clip, uint32_t origStart, uint32_t origEnd,
                   long dTick) {
    if (dTick == 0) return;
    const auto mine = aliveMembers(track, clip);
    for (const auto& n : mine) removeNote(track, n);
    // CC 등 노트 외 이벤트는 위치 기준 (원래 범위 안 것)
    for (auto& e : track.events) {
        if (e.isNoteOn() || e.isNoteOff()) continue;
        if (e.tick >= origStart && e.tick < origEnd) {
            const long nt = (long)e.tick + dTick;
            e.tick = (uint32_t)(nt < 0 ? 0 : nt);
        }
    }
    clip.members.clear();
    for (const auto& n : mine) {
        const long ns = (long)n.startTick + dTick;
        const uint32_t nt = (uint32_t)(ns < 0 ? 0 : ns);
        track.addNote(nt, n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                      n.velocity);
        clip.members.push_back({n.note, nt});
    }
    track.sortEvents();
}

MidiClip copyMidiClip(Track& track, const MidiClip& clip, uint32_t dTick) {
    MidiClip nc = clip;
    nc.startTick += dTick;
    nc.endTick += dTick;
    nc.members.clear();
    std::vector<MidiEvent> addEv; // CC 복사 (위치 기준)
    for (const auto& e : track.events) {
        if (e.isNoteOn() || e.isNoteOff()) continue;
        if (e.tick >= clip.startTick && e.tick < clip.endTick) {
            MidiEvent c = e;
            c.tick += dTick;
            addEv.push_back(c);
        }
    }
    track.events.insert(track.events.end(), addEv.begin(), addEv.end());
    for (const auto& n : aliveMembers(track, clip)) {
        track.addNote(n.startTick + dTick,
                      n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                      n.velocity);
        nc.members.push_back({n.note, n.startTick + dTick});
    }
    track.sortEvents();
    return nc;
}

void eraseMidiClip(Track& track, const MidiClip& clip) {
    for (const auto& n : aliveMembers(track, clip)) removeNote(track, n);
    track.events.erase(std::remove_if(track.events.begin(), track.events.end(),
                                      [&](const MidiEvent& e) {
                                          return !e.isNoteOn() && !e.isNoteOff() &&
                                                 e.tick >= clip.startTick &&
                                                 e.tick < clip.endTick;
                                      }),
                       track.events.end());
}

void moveMidiClipToTrack(Track& src, Track& dst, MidiClip& clip, uint32_t origStart,
                         uint32_t origEnd, long dTick) {
    const auto mine = aliveMembers(src, clip);
    for (const auto& n : mine) removeNote(src, n);
    // CC: 원 범위 안 것을 뽑아 dst 채널로 변환해 붙인다
    std::vector<MidiEvent> ccs;
    for (const auto& e : src.events)
        if (!e.isNoteOn() && !e.isNoteOff() && e.tick >= origStart && e.tick < origEnd)
            ccs.push_back(e);
    src.events.erase(std::remove_if(src.events.begin(), src.events.end(),
                                    [&](const MidiEvent& e) {
                                        return !e.isNoteOn() && !e.isNoteOff() &&
                                               e.tick >= origStart && e.tick < origEnd;
                                    }),
                     src.events.end());
    clip.members.clear();
    for (const auto& n : mine) {
        const long ns = (long)n.startTick + dTick;
        const uint32_t nt = (uint32_t)(ns < 0 ? 0 : ns);
        dst.addNote(nt, n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                    n.velocity);
        clip.members.push_back({n.note, nt});
    }
    for (auto e : ccs) {
        const long nt = (long)e.tick + dTick;
        e.tick = (uint32_t)(nt < 0 ? 0 : nt);
        e.status = (uint8_t)((e.status & 0xF0) | (dst.channel & 0x0F));
        dst.events.push_back(e);
    }
    src.sortEvents();
    dst.sortEvents();
}

} // namespace midipro::seq
