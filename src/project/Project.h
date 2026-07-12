#pragma once
// =============================================================
// MidiPro - project/Project.h
// 프로젝트 통째 저장: 곡 + 신스 음색 + 컨트롤러 매핑 + VST 참조 +
// MPE 설정을 하나의 .midipro 텍스트 파일로 저장/불러온다.
//
// 왜 별도 계층인가 (Rule 1, 6):
//   여러 모듈의 상태를 묶는 직렬화는 순수 로직이라 GUI 없이 왕복
//   테스트한다. 신스/매핑은 각 모듈의 기존 직렬화기를 재사용한다.
//   .mid는 노트만 담지만 .midipro는 세션 전체를 담는다.
// =============================================================

#include "audio/Synth.h"
#include "mapping/MidiMap.h"
#include "sequencer/Song.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace midipro::project {

// 오디오 클립 참조. PCM은 프로젝트 옆 사이드카 폴더(<이름>.audio/)에 WAV로
// 저장하고, 프로젝트 텍스트에는 파일명과 배치 정보만 남긴다.
struct AudioRef {
    int trackIndex = 0;
    std::string file;      // 사이드카 폴더 내 파일명 (예: track0.wav)
    uint32_t startTick = 0;
    double speed = 1.0;
    int64_t trimStart = 0;
    int64_t trimLen = 0;
    double fadeInSec = 0.0;  // 페이드 인/아웃 (초)
    double fadeOutSec = 0.0;
    float gain = 1.0f;     // 클립 게인 (tgain 라인, 없으면 1.0)
    bool freezeBounce = false; // 프리즈로 구운 클립 (tbounce 라인)
    std::string clipName;  // 표시용 이름
};

// 버전 노드의 클립 배치 값 (곡 안의 클립과 같은 순서: 트랙순 -> 클립순).
// 클립 객체는 여러 버전이 공유하므로, 객체 안의 배치 필드가 아니라 이 값이
// 그 버전의 진짜 배치다 (gui의 SongSnapshot과 같은 구조).
struct ClipPlace {
    uint32_t startTick = 0;
    double speed = 1.0;
    int64_t trimStart = 0;
    int64_t trimLen = 0;
    double fadeInSec = 0.0;
    double fadeOutSec = 0.0;
    float gain = 1.0f;
};

// 버전 분기 트리의 한 노드 (체크인 시점의 곡 전체).
struct VersionSnap {
    int id = 0;
    int parent = -1; // -1 = 루트
    std::string name;
    std::string note; // 체크인 메모 (한 줄, vnote 라인)
    seq::Song song;
    std::vector<ClipPlace> places;
    std::vector<AudioRef> refs; // load()가 클립을 붙일 때 사용 (저장 시 미사용)
};

struct ProjectData {
    seq::Song song;
    audio::SynthParams synth;
    mapping::MidiMap midiMap;
    bool mpe = false;
    std::string vstInstrumentPath;
    int vstInstrumentClass = 0;
    int vstInstrumentTrack = -1; // VSTi 출력을 보낼 트랙 (-1 = 마스터 직행)
    std::string vstEffectPath;
    int vstEffectClass = 0;
    // 메트로놈/카운트인 클릭 소리 (일반/강조 각각. 샘플 경로가 비면 신스 음 사용)
    int metroClickNote = 77;
    int countInClickNote = 84;
    int accentClickNote = 88;
    int countInAccentClickNote = 91;
    std::string metroSamplePath;
    std::string countInSamplePath;
    std::string accentSamplePath;
    std::string countInAccentSamplePath;
    int metroSigIndex = 0; // 박자: 0=4/4, 1=3/4, 2=6/8
    int countInBeats = 4;  // 카운트인 클릭 횟수
    float metroVolume = 1.0f;   // 클릭 음량 (메트로놈/카운트인)
    float countInVolume = 1.0f;
    // 마스터 리미터 (켜짐 여부 + 게인/실링/릴리스 dB/ms)
    bool masterLimiterOn = true;
    float limiterGainDb = 0.0f;
    float limiterCeilDb = -0.3f;
    float limiterReleaseMs = 120.0f;
    // 센드/리턴 버스 (리턴 레벨 + 공용 리버브 공간/댐핑)
    float returnLevel = 1.0f;
    float returnRoom = 0.5f;
    float returnDamp = 0.4f;
    // 드럼 샘플 배정 (노트 번호, WAV 경로)
    std::vector<std::pair<int, std::string>> drumSamples;
    // 직렬화 왕복용. save()/load()가 실제 WAV 입출력을 담당한다.
    std::vector<AudioRef> audioRefs;
    // 버전 분기 트리 ([versions] 섹션. 비어 있으면 섹션을 쓰지 않아 옛 파일과 동일)
    std::vector<VersionSnap> versions;
    int versionCurrent = -1;
    int versionNextId = 1;
};

// 문자열 직렬화 (테스트/왕복 검증용 공개)
std::string serialize(const ProjectData& p);
bool deserialize(ProjectData& out, const std::string& text);

bool save(const ProjectData& p, const std::filesystem::path& path);
bool load(ProjectData& out, const std::filesystem::path& path);

// 프로젝트 옆 사이드카 폴더(<이름>.audio/) 경로. 오디오 WAV와 VST 상태 파일이 담긴다.
std::filesystem::path sidecarDir(const std::filesystem::path& projectPath);

} // namespace midipro::project
