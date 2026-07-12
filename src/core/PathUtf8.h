#pragma once
// =============================================================
// MidiPro - core/PathUtf8.h
// UTF-8 문자열 -> std::filesystem::path 변환.
//
// 왜 필요한가:
//   파일 대화상자는 경로를 UTF-8 std::string으로 준다. 그런데 Windows에서
//   std::filesystem::path(const std::string&)는 그 바이트를 "네이티브 narrow
//   인코딩(ANSI 코드페이지)"으로 해석한다. 한글처럼 ANSI로 변환되지 않는
//   경로를 만나면 변환 예외(std::system_error)를 던지고, 잡는 곳이 없으면
//   프로세스가 terminate된다. (증상: 저장 버튼을 누르면 앱이 꺼짐)
//
//   u8path()는 입력을 UTF-8로 해석하므로 이 문제가 없다. 경로를 다루는
//   모든 GUI 호출 지점은 반드시 이 함수를 거쳐야 한다.
// =============================================================

#include <filesystem>
#include <string>

namespace midipro::core {

inline std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::u8path(utf8);
}

// 반대 방향: path -> UTF-8 std::string (표시/저장용)
// (C++17에서 u8string()은 UTF-8 인코딩된 std::string을 돌려준다)
inline std::string pathToUtf8(const std::filesystem::path& p) { return p.u8string(); }

} // namespace midipro::core
