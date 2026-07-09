#pragma once
// =============================================================
// MidiPro - vst/PluginScanner.h
// 표준 위치에 설치된 VST3 플러그인 자동 탐색.
//
// 왜 분리했는가 (Rule 1):
//   플러그인 "찾기"는 파일시스템 탐색일 뿐 SDK와 무관하다. 호스트
//   (Vst3Host)와 떼어 두어 GUI가 목록만 받아 고르게 한다.
// =============================================================

#include <string>
#include <vector>

namespace midipro::vst {

struct PluginEntry {
    std::string name; // 표시용 이름 (예: "Surge XT")
    std::string path; // 로드에 넘길 .vst3 번들 경로
};

// Windows 표준 VST3 폴더들을 훑어 설치된 .vst3 번들을 모은다.
//   - %CommonProgramFiles%\VST3, %CommonProgramFiles(x86)%\VST3
//   - %LOCALAPPDATA%\Programs\Common\VST3
// 이름 오름차순, 중복 경로 제거.
std::vector<PluginEntry> scanVst3Plugins();

} // namespace midipro::vst
