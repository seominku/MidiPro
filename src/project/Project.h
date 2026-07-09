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

#include <filesystem>
#include <string>

namespace midipro::project {

struct ProjectData {
    seq::Song song;
    audio::SynthParams synth;
    mapping::MidiMap midiMap;
    bool mpe = false;
    std::string vstInstrumentPath;
    int vstInstrumentClass = 0;
    std::string vstEffectPath;
    int vstEffectClass = 0;
};

// 문자열 직렬화 (테스트/왕복 검증용 공개)
std::string serialize(const ProjectData& p);
bool deserialize(ProjectData& out, const std::string& text);

bool save(const ProjectData& p, const std::filesystem::path& path);
bool load(ProjectData& out, const std::filesystem::path& path);

} // namespace midipro::project
