// =============================================================
// MidiPro - tests/test_project.cpp
// 프로젝트 통째 저장 왕복 테스트 (Rule 6):
//   곡 + 신스 음색 + 매핑 + VST 참조 + MPE가 보존되는지.
// =============================================================

#include "project/Project.h"

#include <cmath>
#include <iostream>

using namespace midipro;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

void testRoundTrip() {
    project::ProjectData p;
    p.song.bpm = 132.0;
    p.song.ppqn = 480;
    seq::Track t1;
    t1.name = "Lead Guitar";
    t1.channel = 2;
    t1.muted = true;
    t1.addNote(0, 480, 60, 100);
    t1.addNote(480, 240, 67, 90);
    t1.sortEvents();
    p.song.tracks.push_back(t1);

    p.synth.waveform = audio::Waveform::Square;
    p.synth.filterCutoff = 3456.0f;
    p.synth.masterVolume = 0.22f;
    p.mpe = true;
    p.midiMap.bind(74, 0, mapping::ParamTarget::FilterCutoff);
    p.vstInstrumentPath = "C:\\Plugins\\Surge XT.vst3";
    p.vstInstrumentClass = 0;

    const std::string text = project::serialize(p);
    project::ProjectData q;
    CHECK(project::deserialize(q, text));

    // 곡
    CHECK(std::fabs(q.song.bpm - 132.0) < 1e-6);
    CHECK(q.song.ppqn == 480);
    CHECK(q.song.tracks.size() == 1);
    CHECK(q.song.tracks[0].name == "Lead Guitar"); // 공백 포함 이름 보존
    CHECK(q.song.tracks[0].channel == 2);
    CHECK(q.song.tracks[0].muted == true);
    const auto notes = seq::extractNotes(q.song.tracks[0]);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60 && notes[1].note == 67);

    // 신스
    CHECK(q.synth.waveform == audio::Waveform::Square);
    CHECK(std::fabs(q.synth.filterCutoff - 3456.0f) < 0.1f);
    CHECK(std::fabs(q.synth.masterVolume - 0.22f) < 1e-4);

    // 매핑 / MPE / VST
    CHECK(q.mpe == true);
    CHECK(q.midiMap.ccForTarget(mapping::ParamTarget::FilterCutoff) == 74);
    CHECK(q.vstInstrumentPath == "C:\\Plugins\\Surge XT.vst3");
    CHECK(q.vstEffectPath.empty());

    // 우리 포맷이 아니면 거부
    project::ProjectData bad;
    CHECK(!project::deserialize(bad, "hello not a project"));
}

} // namespace

int main() {
    testRoundTrip();
    if (g_failures == 0) {
        std::cout << "[OK] project tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
