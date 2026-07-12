#pragma once
// =============================================================
// MidiPro - gui/Theme.h
// UI 테마: 간단 파라미터(강조색/배경/글자/둥글기)로 ImGui 스타일
// 전체를 생성하고, 파일로 저장/복원한다.
//
// 왜 파라미터 방식인가:
//   ImGui 색상은 50개가 넘어 하나하나 만지게 하면 부담스럽다. 핵심
//   파라미터 몇 개로 조화로운 팔레트를 만들고, 세부가 필요한 사용자는
//   고급 편집기(ImGui::ShowStyleEditor)로 개별 색을 덮어쓴다.
//   개별 오버라이드까지 통째로 저장되므로 어느 쪽으로 꾸며도 유지된다.
// =============================================================

#include <filesystem>

namespace midipro::gui {

struct ThemeParams {
    float accent[3] = {0.35f, 0.55f, 0.90f}; // 강조색 (버튼/슬라이더/선택)
    float bg = 0.12f;        // 배경 밝기 (0=검정 ~ 1=흰색)
    float text = 0.95f;      // 글자 밝기
    float rounding = 4.0f;   // 모서리 둥글기 (0~12)
};

// 파라미터로 ImGui 스타일 전체를 다시 만든다 (즉시 적용).
void applyThemeParams(const ThemeParams& t);

// 현재 스타일(파라미터 + 고급 편집기로 바꾼 개별 색 포함)을 저장/복원.
bool saveTheme(const ThemeParams& t, const std::filesystem::path& path);
bool loadTheme(ThemeParams& t, const std::filesystem::path& path); // 적용까지 수행

// 프리셋
ThemeParams themeDark();     // 기본 다크
ThemeParams themeLight();    // 라이트
ThemeParams themeMidnight(); // 아주 어두운 청록
ThemeParams themeViolet();   // 보라 강조

} // namespace midipro::gui
