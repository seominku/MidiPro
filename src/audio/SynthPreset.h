#pragma once
// =============================================================
// MidiPro - audio/SynthPreset.h
// 신스 음색 프리셋: 내장 목록 + 파일 저장/불러오기.
//
// 왜 분리했는가 (Rule 1, 6):
//   프리셋 직렬화는 순수 로직(파일 I/O + 텍스트 파싱)이라 GUI나
//   오디오와 무관하게 테스트할 수 있다. 포맷은 사람이 읽고 고치기
//   쉬운 "key value" 텍스트로 둔다.
// =============================================================

#include "audio/Synth.h"

#include <filesystem>
#include <string>
#include <vector>

namespace midipro::audio {

struct NamedPreset {
    std::string name;
    SynthParams params;
};

// 시작 시 고르기 좋은 기본 음색들 (리드/패드/베이스/기타풍 등)
const std::vector<NamedPreset>& builtinPresets();

// 텍스트 파일로 저장/불러오기 (한 줄에 "key value").
bool savePreset(const SynthParams& params, const std::filesystem::path& path);
bool loadPreset(SynthParams& out, const std::filesystem::path& path);

// 문자열 직렬화 (테스트/왕복 검증용으로 공개)
std::string serialize(const SynthParams& params);
bool deserialize(SynthParams& out, const std::string& text);

} // namespace midipro::audio
