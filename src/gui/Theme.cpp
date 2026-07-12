// =============================================================
// MidiPro - gui/Theme.cpp
// =============================================================

#include "gui/Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace midipro::gui {

namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// 배경에서 k만큼 대비 방향(어두운 테마=밝게, 밝은 테마=어둡게)으로 이동한 회색.
// 강조색을 아주 살짝 섞어 배경이 완전 무채색으로 죽지 않게 한다.
ImVec4 surface(const ThemeParams& t, float k, float alpha = 1.0f) {
    float dir = (t.bg < 0.5f) ? 1.0f : -1.0f;
    float base = clamp01(t.bg + dir * k);
    float tintAmt = 0.06f;
    return ImVec4(clamp01(base + (t.accent[0] - 0.5f) * tintAmt),
                  clamp01(base + (t.accent[1] - 0.5f) * tintAmt),
                  clamp01(base + (t.accent[2] - 0.5f) * tintAmt), alpha);
}

ImVec4 accentCol(const ThemeParams& t, float mul, float alpha = 1.0f) {
    return ImVec4(clamp01(t.accent[0] * mul), clamp01(t.accent[1] * mul),
                  clamp01(t.accent[2] * mul), alpha);
}

ImVec4 textCol(const ThemeParams& t, float mul, float alpha = 1.0f) {
    float v = clamp01(t.text * mul + (t.text < 0.5f ? (1.0f - mul) * 0.0f : 0.0f));
    return ImVec4(v, v, v, alpha);
}

} // namespace

void applyThemeParams(const ThemeParams& t) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s); // 크기/여백 기본값 유지, 색만 아래서 전부 덮어씀
    ImVec4* c = s.Colors;

    const bool light = t.bg >= 0.5f;
    const ImVec4 txt = textCol(t, 1.0f);
    const ImVec4 txtDim = textCol(t, light ? 2.2f : 0.55f);

    c[ImGuiCol_Text] = txt;
    c[ImGuiCol_TextDisabled] = txtDim;
    c[ImGuiCol_WindowBg] = surface(t, 0.00f);
    c[ImGuiCol_ChildBg] = surface(t, -0.015f);
    c[ImGuiCol_PopupBg] = surface(t, 0.02f, 0.98f);
    c[ImGuiCol_Border] = surface(t, 0.16f, 0.6f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = surface(t, 0.06f);
    c[ImGuiCol_FrameBgHovered] = surface(t, 0.10f);
    c[ImGuiCol_FrameBgActive] = surface(t, 0.14f);
    c[ImGuiCol_TitleBg] = surface(t, -0.03f);
    c[ImGuiCol_TitleBgActive] = accentCol(t, 0.55f);
    c[ImGuiCol_TitleBgCollapsed] = surface(t, -0.03f, 0.75f);
    c[ImGuiCol_MenuBarBg] = surface(t, 0.03f);
    c[ImGuiCol_ScrollbarBg] = surface(t, -0.02f);
    c[ImGuiCol_ScrollbarGrab] = surface(t, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = surface(t, 0.26f);
    c[ImGuiCol_ScrollbarGrabActive] = accentCol(t, 0.9f);
    c[ImGuiCol_CheckMark] = accentCol(t, 1.15f);
    c[ImGuiCol_SliderGrab] = accentCol(t, 1.0f);
    c[ImGuiCol_SliderGrabActive] = accentCol(t, 1.25f);
    c[ImGuiCol_Button] = accentCol(t, 0.75f, 0.65f);
    c[ImGuiCol_ButtonHovered] = accentCol(t, 1.0f, 0.9f);
    c[ImGuiCol_ButtonActive] = accentCol(t, 1.2f);
    c[ImGuiCol_Header] = accentCol(t, 0.85f, 0.45f);
    c[ImGuiCol_HeaderHovered] = accentCol(t, 1.0f, 0.65f);
    c[ImGuiCol_HeaderActive] = accentCol(t, 1.1f, 0.85f);
    c[ImGuiCol_Separator] = surface(t, 0.14f);
    c[ImGuiCol_SeparatorHovered] = accentCol(t, 0.9f, 0.75f);
    c[ImGuiCol_SeparatorActive] = accentCol(t, 1.1f);
    c[ImGuiCol_ResizeGrip] = accentCol(t, 0.8f, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = accentCol(t, 1.0f, 0.6f);
    c[ImGuiCol_ResizeGripActive] = accentCol(t, 1.2f, 0.9f);
    c[ImGuiCol_Tab] = surface(t, 0.05f);
    c[ImGuiCol_TabHovered] = accentCol(t, 1.0f, 0.8f);
    c[ImGuiCol_TabActive] = accentCol(t, 0.7f);
    c[ImGuiCol_TabUnfocused] = surface(t, 0.02f);
    c[ImGuiCol_TabUnfocusedActive] = accentCol(t, 0.45f);
    c[ImGuiCol_DockingPreview] = accentCol(t, 1.0f, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = surface(t, -0.03f);
    c[ImGuiCol_PlotLines] = accentCol(t, 1.1f);
    c[ImGuiCol_PlotLinesHovered] = accentCol(t, 1.3f);
    c[ImGuiCol_PlotHistogram] = accentCol(t, 1.0f);
    c[ImGuiCol_PlotHistogramHovered] = accentCol(t, 1.25f);
    c[ImGuiCol_TableHeaderBg] = surface(t, 0.06f);
    c[ImGuiCol_TableBorderStrong] = surface(t, 0.20f);
    c[ImGuiCol_TableBorderLight] = surface(t, 0.10f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = surface(t, 0.025f, 0.5f);
    c[ImGuiCol_TextSelectedBg] = accentCol(t, 1.0f, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentCol(t, 1.3f, 0.9f);
    c[ImGuiCol_NavHighlight] = accentCol(t, 1.1f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, light ? 0.25f : 0.45f);

    float r = std::clamp(t.rounding, 0.0f, 12.0f);
    s.WindowRounding = r;
    s.ChildRounding = r * 0.75f;
    s.FrameRounding = r * 0.75f;
    s.PopupRounding = r * 0.75f;
    s.GrabRounding = r * 0.75f;
    s.TabRounding = r * 0.75f;
    s.ScrollbarRounding = r;
}

bool saveTheme(const ThemeParams& t, const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "midipro_theme 1\n";
    f << "accent " << t.accent[0] << ' ' << t.accent[1] << ' ' << t.accent[2] << '\n';
    f << "bg " << t.bg << '\n';
    f << "text " << t.text << '\n';
    f << "rounding " << t.rounding << '\n';
    // 고급 편집기에서 바꾼 개별 색도 그대로 살리기 위해 전체 색을 저장
    const ImGuiStyle& s = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4& v = s.Colors[i];
        f << "col " << i << ' ' << v.x << ' ' << v.y << ' ' << v.z << ' ' << v.w << '\n';
    }
    return f.good();
}

bool loadTheme(ThemeParams& t, const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string tag;
    int ver = 0;
    f >> tag >> ver;
    if (tag != "midipro_theme") return false;

    ThemeParams loaded;
    struct ColOverride { int idx; ImVec4 v; };
    std::vector<ColOverride> cols;
    std::string key;
    while (f >> key) {
        if (key == "accent") f >> loaded.accent[0] >> loaded.accent[1] >> loaded.accent[2];
        else if (key == "bg") f >> loaded.bg;
        else if (key == "text") f >> loaded.text;
        else if (key == "rounding") f >> loaded.rounding;
        else if (key == "col") {
            ColOverride o{};
            f >> o.idx >> o.v.x >> o.v.y >> o.v.z >> o.v.w;
            if (o.idx >= 0 && o.idx < ImGuiCol_COUNT) cols.push_back(o);
        } else {
            std::string skip;
            std::getline(f, skip);
        }
    }
    t = loaded;
    applyThemeParams(t); // 둥글기 등 파생 값 계산
    ImGuiStyle& s = ImGui::GetStyle();
    for (const auto& o : cols) s.Colors[o.idx] = o.v; // 저장된 색이 최종 우선
    return true;
}

ThemeParams themeDark() { return ThemeParams{}; }

ThemeParams themeLight() {
    ThemeParams t;
    t.accent[0] = 0.18f; t.accent[1] = 0.45f; t.accent[2] = 0.85f;
    t.bg = 0.92f;
    t.text = 0.10f;
    t.rounding = 4.0f;
    return t;
}

ThemeParams themeMidnight() {
    ThemeParams t;
    t.accent[0] = 0.10f; t.accent[1] = 0.75f; t.accent[2] = 0.65f;
    t.bg = 0.06f;
    t.text = 0.90f;
    t.rounding = 6.0f;
    return t;
}

ThemeParams themeViolet() {
    ThemeParams t;
    t.accent[0] = 0.62f; t.accent[1] = 0.40f; t.accent[2] = 0.95f;
    t.bg = 0.10f;
    t.text = 0.95f;
    t.rounding = 8.0f;
    return t;
}

} // namespace midipro::gui
