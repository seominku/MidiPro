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

// 곡 중간의 템포 변경 지점. tick부터 다음 지점(또는 끝)까지 bpm으로 재생.
// ramp=true면 이 지점 bpm에서 "다음 지점 bpm"까지 틱에 선형으로 점진 변화한다
// (점점 빠르게/느리게). 다음 지점이 없으면 일정하게 유지된다.
struct TempoChange {
    uint32_t tick = 0;
    double bpm = 120.0;
    bool ramp = false;
};

// 곡 구간 마커 (Intro/Verse/Chorus 같은 이름표). 재생엔 영향이 없고
// 타임라인 표시/점프용이다.
struct SectionMarker {
    uint32_t tick = 0;
    std::string name;
};

struct Song {
    int ppqn = kDefaultPpqn;
    double bpm = 120.0; // 기본(시작) 템포. tempoChanges가 tick 0을 덮으면 그쪽 우선.
    float masterVolume = 1.0f; // 마스터 페이더 (0~1.5)
    float masterPan = 0.0f;    // 마스터 팬 (-1 좌 ~ +1 우)
    float masterGain = 1.0f;   // 마스터 게인 트림 (0~2, 페이더와 곱해진다)
    std::vector<TempoChange> tempoChanges; // 틱 오름차순 유지 (비면 bpm 고정)
    std::vector<SectionMarker> markers;    // 구간 마커 (틱 오름차순 유지)
    std::vector<Track> tracks;

    uint32_t lengthTicks() const;
};

// ---- 템포 맵 변환 (순수 함수, 유닛 테스트 대상) ----
// 절대 틱 위치의 유효 BPM. tick 이전의 마지막 변경 지점이 적용된다.
double bpmAtTick(const Song& song, double tick);
// 절대 틱 <-> 절대 초 (템포 구간별 적분). 템포 변경이 없으면
// ticksToSeconds/secondsToTicks와 동일한 결과를 낸다.
double songTickToSec(const Song& song, double tick);
double songSecToTick(const Song& song, double sec);

} // namespace midipro::seq
