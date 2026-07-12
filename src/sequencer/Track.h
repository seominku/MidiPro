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
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 오디오 클립은 audio/ 모듈에 있다. 포인터만 들고 있으므로 전방선언.
namespace midipro::audio { struct AudioClip; }

namespace midipro::seq {

// 트랙 이펙트 체인의 한 슬롯. 실제 처리는 오디오 엔진의 트랙 버스에서
// 이 순서대로 이뤄진다. path/classIndex는 프로젝트 저장 후 복원에 쓴다.
struct TrackPlugin {
    std::string name;           // 표시 이름
    std::string path;           // .vst3 번들 경로 (복원용)
    int classIndex = 0;         // 번들 내 클래스 인덱스
    bool isInstrument = false;  // 악기(VSTi)면 true, 이펙트면 false
    bool enabled = true;        // on/off (실시간 바이패스)
};

// MIDI 클립: 트랙 이벤트 목록 위에 얹는 "구간 객체". 이벤트 자체는 트랙에
// 평평하게 저장되고(피아노 롤/재생 엔진은 그대로), 트랙 뷰가 이 구간을
// 블록으로 그려 이동/복제/삭제 같은 어레인지 조작을 제공한다.
//
// 소유 개념: members = 이 클립 "소속" 노트의 (음, 시작 틱) 키 목록.
// 이동/복제/삭제는 멤버만 대상이라, 남의 노트 위에 놓아도 흡수하지 않는다.
// 멤버는 만들 때(또는 "다시 담기")의 범위 안 노트로 정해지고, 클립 조작이
// 노트를 옮길 때마다 키가 갱신된다. 피아노 롤에서 멤버를 지우면 자동 탈퇴.
struct MidiClip {
    uint32_t startTick = 0;
    uint32_t endTick = 0;
    std::string name = "클립";
    std::vector<std::pair<uint8_t, uint32_t>> members; // (note, startTick)
};

struct Track {
    std::string name = "Track";
    uint8_t channel = 0; // 0~15
    bool muted = false;
    float volume = 1.0f; // 페이더 (0~1.5, 선형)
    float pan = 0.0f;    // -1(좌) ~ +1(우)
    float gain = 1.0f;   // 게인 트림 (0~2, 페이더와 곱해진다)
    float sendLevel = 0.0f; // 리턴 버스(공용 리버브)로 보내는 양 (0~1)
    // 프리즈: MIDI를 오디오로 구워둔 상태. 재생/내보내기에서 이 트랙의
    // MIDI는 건너뛰고(구운 클립이 대신 소리냄) VST 처리도 생략해 CPU를 아낀다.
    bool frozen = false;
    // 기타 트랙 표시: 선택하면 타브(TAB) 악보 창이 열린다 (소리엔 영향 없음)
    bool isGuitar = false;
    // 타브 가져오기 그룹 꼬리표: 같은 악보를 다시 가져오면 새 트랙을 만들지 않고
    // (importKey=악보 이름, importPart=파트 번호)가 같은 트랙을 찾아 덮어쓴다.
    std::string importKey;
    int importPart = -1;
    // 타브 운지 힌트: 악보가 지정한 줄(현). 같은 음이라도 2번줄 9프렛/1번줄 4프렛처럼
    // 운지가 여럿이라, 힌트가 있으면 타브 창이 악보 그대로의 줄에 표시한다.
    struct TabHint {
        uint32_t tick = 0;
        uint8_t note = 0;
        uint8_t strIdx = 0; // 0=1번줄(e) ~ 5=6번줄(E)
    };
    std::vector<TabHint> tabHints;
    std::vector<MidiEvent> events; // tick 오름차순 유지
    std::vector<MidiClip> midiClips; // 어레인지용 MIDI 클립 구간 (start 오름차순)
    std::vector<TrackPlugin> plugins; // 트랙에 표시할 플러그인 목록
    int inputChannelMode = 0;         // ASIO 입력 채널: 0=1+2, 1=입력1, 2=입력2
    // 오디오 클립들(녹음/임포트). 한 트랙에 여러 클립을 서로 다른 위치에 놓을 수 있다.
    std::vector<std::shared_ptr<audio::AudioClip>> clips;

    // 편집 스탬프: 이벤트가 바뀔 때마다 증가 (sortEvents/removeNote/벨로시티 등).
    // GUI가 노트 추출(extractNotes) 결과를 캐시할 때 무효화 기준으로 쓴다.
    uint64_t editStamp = 0;

    // 볼륨/팬 오토메이션 (틱 오름차순, 점 사이는 선형 보간).
    // 비어 있으면 페이더 값이 그대로 쓰이고, 점이 있으면 그 곡선이 우선한다.
    struct AutoPoint {
        uint32_t tick = 0;
        float value = 1.0f; // 볼륨 0~1.5 / 팬 -1~1
    };
    std::vector<AutoPoint> volAuto;
    std::vector<AutoPoint> panAuto;

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

// 노트의 벨로시티(세기)를 바꾼다. 1~127로 제한. 찾으면 true.
bool setNoteVelocity(Track& track, const NoteSpan& span, uint8_t velocity);

// 트랙의 모든 노트 시작을 gridTicks 격자에 반올림 스냅한다 (길이는 유지).
// 바뀐 노트 수를 돌려준다.
int quantizeTrack(Track& track, uint32_t gridTicks);

// 휴머나이즈: 노트 시작을 ±jitterTicks, 세기를 ±jitterVel 범위에서 무작위로
// 흔든다 (길이는 유지). 기계적으로 정확한 시퀀스에 사람 연주 느낌을 준다.
// onlyNotes가 비어 있지 않으면 그 노트들만 적용. 같은 seed면 결과도 같다
// (결정적 -> 유닛 테스트 가능). 반환: 바뀐 노트 수.
int humanizeTrack(Track& track, uint32_t jitterTicks, int jitterVel, uint32_t seed,
                  const std::vector<NoteSpan>& onlyNotes = {});

// 오토메이션 곡선의 tick 시점 값 (선형 보간). 점이 없으면 fallback.
// 첫 점 이전은 첫 값, 마지막 점 이후는 마지막 값으로 유지된다.
float autoValueAt(const std::vector<Track::AutoPoint>& pts, uint32_t tick, float fallback);

// (note, tick) 지점을 포함하는 노트 구간을 찾는다. 없으면 found=false.
NoteSpan noteSpanAt(const Track& track, uint8_t note, uint32_t tick, bool& found);

// ---- MIDI 구간 조작 (위치 기준 — 구간 복제/어레인지가 쓴다) ----
// [start, end)에서 "시작하는" 노트는 스팬(쌍) 단위로 다뤄, 꼬리가 범위 밖에
// 걸쳐 있어도 On/Off 짝이 깨지지 않는다. 노트 외 이벤트(CC 등)는 낱개 처리.
void shiftMidiRange(Track& track, uint32_t start, uint32_t end, long dTick); // 밀기
void copyMidiRange(Track& track, uint32_t start, uint32_t end, uint32_t dTick); // 복사
void eraseMidiRange(Track& track, uint32_t start, uint32_t end); // 삭제

// ---- MIDI 클립 조작 (멤버 = 소유 노트 기준. 남의 노트를 흡수하지 않는다) ----
// 클립 범위 안에서 시작하는 노트들을 멤버로 담는다 (만들 때/다시 담기)
void adoptMidiClipMembers(const Track& track, MidiClip& clip);
// 멤버 노트(+원래 범위 안의 CC)를 dTick만큼 옮기고 멤버 키를 갱신한다.
// clip의 범위(start/end)는 호출자가 이미 새 위치로 옮겨 둔 상태여야 한다.
void shiftMidiClip(Track& track, MidiClip& clip, uint32_t origStart, uint32_t origEnd,
                   long dTick);
// 멤버 노트(+범위 안 CC)를 +dTick 위치로 복사한 새 클립을 돌려준다
MidiClip copyMidiClip(Track& track, const MidiClip& clip, uint32_t dTick);
// 멤버 노트(+범위 안 CC)를 지운다 (클립 항목 제거는 호출자가)
void eraseMidiClip(Track& track, const MidiClip& clip);
// 클립을 다른 트랙으로: 멤버 노트(+원 범위 CC)를 dst로 옮긴다 (dTick 적용,
// CC는 dst 채널로 변환). clip 범위는 호출자가 이미 새 위치로 옮겨 둔 상태.
void moveMidiClipToTrack(Track& src, Track& dst, MidiClip& clip, uint32_t origStart,
                         uint32_t origEnd, long dTick);
// 에디터가 노트를 "추가/이동"한 뒤 호출: 그 위치를 덮는 클립이 있으면 그 클립
// 소유로 넣는다 (겹치면 앞선 클립). 노트를 그리면 자동으로 클립에 담긴다.
void adoptNoteIntoClips(Track& track, uint8_t note, uint32_t tick);
// 노트 키를 소속에서 뺀다 (removeNote가 자동으로 부른다)
void disownNoteFromClips(Track& track, uint8_t note, uint32_t tick);

} // namespace midipro::seq
