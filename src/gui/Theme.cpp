// =============================================================
// MidiPro - gui/Theme.cpp
// =============================================================

#include "gui/Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

// 패널 불투명도가 곱해지는 색들 — 배경 이미지가 이 색들을 통해 비친다.
// (저장된 개별 색을 되살릴 때 이 알파를 지키려고 파일 범위로 뺐다)
static const int kPanelBgCols[] = {
    ImGuiCol_WindowBg, ImGuiCol_ChildBg,     ImGuiCol_FrameBg,
    ImGuiCol_TitleBg,  ImGuiCol_MenuBarBg,   ImGuiCol_ScrollbarBg,
    ImGuiCol_Tab,      ImGuiCol_TabUnfocused, ImGuiCol_DockingEmptyBg,
    ImGuiCol_PopupBg,
};

// 위젯 스킨이 색을 열쇠로 삼아 버튼·탭·제목을 찾으므로(UiSkin.cpp), 이 색들이
// 다른 위젯 색과 완전히 같으면 슬라이더 손잡이·체크 표시까지 같이 이미지가 된다.
// 눈에 안 보일 만큼(1/255) 어긋나게 해서 열쇠를 유일하게 만든다.
static const int kSkinKeyCols[] = {
    ImGuiCol_Button, ImGuiCol_ButtonHovered,     ImGuiCol_ButtonActive,
    ImGuiCol_Tab,    ImGuiCol_TabHovered,        ImGuiCol_TabSelected,
    ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected,
    ImGuiCol_TitleBg, ImGuiCol_TitleBgActive,    ImGuiCol_TitleBgCollapsed,
};

static void makeSkinKeysUnique(ImGuiStyle& s) {
    auto rgb8 = [](const ImVec4& c) {
        return ImGui::ColorConvertFloat4ToU32(c) & 0x00FFFFFFu;
    };
    std::vector<ImU32> used;
    used.reserve(ImGuiCol_COUNT);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        bool isKey = false;
        for (int k : kSkinKeyCols)
            if (k == i) { isKey = true; break; }
        if (!isKey) used.push_back(rgb8(s.Colors[i]));
    }
    for (int k : kSkinKeyCols) {
        ImVec4& c = s.Colors[k];
        for (int tries = 0; tries < 12; ++tries) {
            const ImU32 v = rgb8(c);
            bool clash = false;
            for (ImU32 u : used)
                if (u == v) { clash = true; break; }
            if (!clash) { used.push_back(v); break; }
            c.z += (c.z < 0.98f) ? (1.0f / 255.0f) : -(1.0f / 255.0f);
        }
    }
}

// 파라미터로 스타일 하나를 통째로 만든다 (전역 스타일과 창별 임시 스타일이 공용).
static void buildStyle(const ThemeParams& t, ImGuiStyle& s) {
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

    // 패널 불투명도: 배경 이미지가 비쳐 보이게 창/자식/탭 배경만 반투명으로.
    // 팝업·모달은 글을 읽어야 하므로 덜 투명하게 둔다.
    const float pa = std::clamp(t.panelAlpha, 0.15f, 1.0f);
    if (pa < 1.0f) {
        for (int idx : kPanelBgCols) {
            if (idx == ImGuiCol_PopupBg) continue;
            c[idx].w *= pa;
        }
        c[ImGuiCol_PopupBg].w *= std::max(pa, 0.85f); // 팝업은 최소한의 가독성 유지
    }

    float r = std::clamp(t.rounding, 0.0f, 12.0f);
    s.WindowRounding = r;
    s.ChildRounding = r * 0.75f;
    s.FrameRounding = r * 0.75f;
    s.PopupRounding = r * 0.75f;
    s.GrabRounding = r * 0.75f;
    s.TabRounding = r * 0.75f;
    s.ScrollbarRounding = r;

    // ---- 여백·크기 다듬기 (밋밋함의 상당 부분은 ImGui 기본 여백이 빽빽해서다) ----
    // (아래 값들은 100% 기준. 함수 끝에서 DPI 배율이 한꺼번에 곱해진다)
    // 숨 쉴 공간을 주고, 스크롤바·그랩을 조금 키워 "설계된" 느낌을 낸다.
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);
    s.ItemSpacing = ImVec2(9.0f, 7.0f);
    s.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    s.CellPadding = ImVec2(6.0f, 4.0f);
    s.GrabMinSize = 12.0f;
    s.ScrollbarSize = 13.0f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f; // SeparatorText 밑줄을 조금 굵게
    // 프레임에 아주 옅은 테두리를 줘 입력칸·버튼 경계가 또렷해진다(평면감 완화).
    s.FrameBorderSize = 1.0f;
    c[ImGuiCol_Border] = surface(t, light ? -0.12f : 0.20f, 0.5f);

    // DPI 배율 적용: 여백·둥글기·스크롤바 등 모든 치수를 한 번에 키운다.
    // (테마가 바뀔 때마다 buildStyle이 다시 불리므로 여기서 곱해야 안 잊는다)
    if (uiDpiScale() != 1.0f) s.ScaleAllSizes(uiDpiScale());

    makeSkinKeysUnique(s);
}

static float g_uiDpiScale = 1.0f;
void setUiDpiScale(float scale) { g_uiDpiScale = scale > 0.5f ? scale : 1.0f; }
float uiDpiScale() { return g_uiDpiScale; }

void applyThemeParams(const ThemeParams& t) { buildStyle(t, ImGui::GetStyle()); }

// ---- 창별 오버라이드 ----

const char* uiSkinSlotName(int slot) {
    switch (slot) {
    case kSkinButton: return "버튼";
    case kSkinTab: return "탭";
    case kSkinTitle: return "제목 표시줄";
    default: return "?";
    }
}

const char* themeWindowName(int win) {
    static const char* kNames[kThemeWindowCount] = {
        "트랜스포트", "MIDI 장치",  "트랙 목록",   "트랙 뷰",     "믹서",
        "채널",       "성능",       "피아노 롤",   "드럼 트랙",   "어레인지",
        "기타 연습",  "신디사이저", "개인설정",    "내보내기",    "내장 이펙트",
        "VST3",       "기타 도우미", "입력 모니터",
    };
    return (win >= 0 && win < kThemeWindowCount) ? kNames[win] : "?";
}

ThemeParams effectiveParams(const ThemeParams& base, const WindowStyleOverride& ov) {
    ThemeParams t = base; // 켜지 않은 항목은 전체 테마를 그대로 상속
    if (!ov.enabled) return t;
    if (ov.useAccent) {
        t.accent[0] = ov.accent[0];
        t.accent[1] = ov.accent[1];
        t.accent[2] = ov.accent[2];
    }
    if (ov.useBg) t.bg = ov.bg;
    if (ov.useText) t.text = ov.text;
    if (ov.useRounding) t.rounding = ov.rounding;
    if (ov.usePanelAlpha) t.panelAlpha = ov.panelAlpha;
    return t;
}

namespace {
// 창을 그리는 동안 잠시 치워둔 전역 스타일 (창 그리기는 중첩되지 않는다)
ImGuiStyle g_savedStyle;
} // namespace

bool pushWindowStyle(const ThemeParams& base, const WindowStyleOverride& ov) {
    if (!ov.enabled || !ov.anyField()) return false;
    g_savedStyle = ImGui::GetStyle();
    buildStyle(effectiveParams(base, ov), ImGui::GetStyle());
    return true;
}

void popWindowStyle(bool pushed) {
    if (pushed) ImGui::GetStyle() = g_savedStyle;
}

bool saveTheme(const ThemeParams& t, const std::filesystem::path& path,
               const WindowStyleOverride* wins) {
    std::ofstream f(path);
    if (!f) return false;
    f << "midipro_theme 1\n";
    f << "accent " << t.accent[0] << ' ' << t.accent[1] << ' ' << t.accent[2] << '\n';
    f << "bg " << t.bg << '\n';
    f << "text " << t.text << '\n';
    f << "rounding " << t.rounding << '\n';
    f << "panelalpha " << t.panelAlpha << '\n';
    // 배경 레이어들 (아래→위 순서). 경로에 공백이 있을 수 있어 줄 끝에 둔다.
    for (const auto& L : t.bgLayers) {
        if (L.image.empty()) continue;
        f << "bglayer " << L.opacity << ' ' << L.fit << ' ' << L.scale << ' ' << L.posX << ' '
          << L.posY << ' ' << (L.visible ? 1 : 0) << ' ' << L.image << '\n';
    }
    // 위젯 스킨 (버튼·탭·제목 — 전체 공통). 경로는 줄 끝.
    for (int i = 0; i < kSkinSlotCount; ++i) {
        if (t.skins[i].image.empty()) continue;
        f << "skinimg " << i << ' ' << t.skins[i].opacity << ' ' << t.skins[i].image << '\n';
        // 오프셋은 따로 둔다 — skinimg는 경로가 줄 끝이라 뒤에 값을 못 붙인다
        const UiSkin& sk = t.skins[i];
        if (sk.ofsL != 0.0f || sk.ofsR != 0.0f || sk.ofsT != 0.0f || sk.ofsB != 0.0f)
            f << "skinofs " << i << ' ' << sk.ofsL << ' ' << sk.ofsR << ' ' << sk.ofsT << ' '
              << sk.ofsB << '\n';
    }
    // 창별 오버라이드 (켜진 창만 한 줄씩 — 옛 파일과 호환된다)
    if (wins) {
        for (int i = 0; i < kThemeWindowCount; ++i) {
            const WindowStyleOverride& o = wins[i];
            // 배경 이미지만 지정한 창도 저장해야 한다 (경로는 공백 허용 -> 줄 끝까지)
            for (const auto& L : o.bgLayers) {
                if (L.image.empty()) continue;
                f << "winbglayer " << i << ' ' << L.opacity << ' ' << L.fit << ' ' << L.scale
                  << ' ' << L.posX << ' ' << L.posY << ' ' << (L.visible ? 1 : 0) << ' '
                  << L.image << '\n';
            }
            if (!o.enabled) continue;
            f << "winov " << i << ' ' << (o.enabled ? 1 : 0) << ' ' << (o.useAccent ? 1 : 0)
              << ' ' << o.accent[0] << ' ' << o.accent[1] << ' ' << o.accent[2] << ' '
              << (o.useBg ? 1 : 0) << ' ' << o.bg << ' ' << (o.useText ? 1 : 0) << ' '
              << o.text << ' ' << (o.useRounding ? 1 : 0) << ' ' << o.rounding << ' '
              << (o.usePanelAlpha ? 1 : 0) << ' ' << o.panelAlpha << '\n';
        }
    }
    // 고급 편집기에서 바꾼 개별 색도 그대로 살리기 위해 전체 색을 저장
    const ImGuiStyle& s = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4& v = s.Colors[i];
        f << "col " << i << ' ' << v.x << ' ' << v.y << ' ' << v.z << ' ' << v.w << '\n';
    }
    return f.good();
}

// 줄의 남은 부분을 값으로 읽는다 (경로에 공백이 있어도 되도록)
static std::string readRestOfLine(std::istream& f) {
    std::string s;
    std::getline(f, s);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

bool loadTheme(ThemeParams& t, const std::filesystem::path& path,
               WindowStyleOverride* wins) {
    std::ifstream f(path);
    if (!f) return false;
    std::string tag;
    int ver = 0;
    f >> tag >> ver;
    if (tag != "midipro_theme") return false;

    ThemeParams loaded;
    struct ColOverride { int idx; ImVec4 v; };
    std::vector<ColOverride> cols;
    // 옛 형식(이미지 한 장)을 읽어 담아 두었다가 마지막에 레이어로 옮긴다
    BgLayer legacyG;
    std::vector<BgLayer> legacyW((std::size_t)kThemeWindowCount);
    std::string key;
    while (f >> key) {
        if (key == "accent") f >> loaded.accent[0] >> loaded.accent[1] >> loaded.accent[2];
        else if (key == "bg") f >> loaded.bg;
        else if (key == "text") f >> loaded.text;
        else if (key == "rounding") f >> loaded.rounding;
        else if (key == "panelalpha") f >> loaded.panelAlpha;
        // ---- 배경 레이어 (현재 형식) ----
        else if (key == "bglayer") {
            BgLayer L;
            int vis = 1;
            f >> L.opacity >> L.fit >> L.scale >> L.posX >> L.posY >> vis;
            L.visible = vis != 0;
            L.image = readRestOfLine(f);
            if (!L.image.empty()) loaded.bgLayers.push_back(L);
        } else if (key == "skinimg") {
            int slot = -1;
            float op = 1.0f;
            f >> slot >> op;
            const std::string p = readRestOfLine(f);
            if (slot >= 0 && slot < kSkinSlotCount && !p.empty()) {
                loaded.skins[slot].image = p;
                loaded.skins[slot].opacity = op;
            }
        } else if (key == "skinofs") {
            int slot = -1;
            float l = 0, r = 0, tp = 0, b = 0;
            f >> slot >> l >> r >> tp >> b;
            if (slot >= 0 && slot < kSkinSlotCount) {
                loaded.skins[slot].ofsL = l;
                loaded.skins[slot].ofsR = r;
                loaded.skins[slot].ofsT = tp;
                loaded.skins[slot].ofsB = b;
            }
        } else if (key == "winbglayer") {
            int idx = -1, vis = 1;
            BgLayer L;
            f >> idx >> L.opacity >> L.fit >> L.scale >> L.posX >> L.posY >> vis;
            L.visible = vis != 0;
            L.image = readRestOfLine(f);
            if (wins && idx >= 0 && idx < kThemeWindowCount && !L.image.empty())
                wins[idx].bgLayers.push_back(L);
        }
        // ---- 옛 형식(이미지 한 장): 읽어서 레이어 한 장으로 옮긴다 ----
        else if (key == "bgimgopacity") f >> legacyG.opacity;
        else if (key == "bgimgfit") f >> legacyG.fit;
        else if (key == "bgimgplace") f >> legacyG.scale >> legacyG.posX >> legacyG.posY;
        else if (key == "bgimg") legacyG.image = readRestOfLine(f);
        else if (key == "winbgplace") {
            int idx = -1;
            float sc = 1.0f, px = 0.5f, py = 0.5f;
            f >> idx >> sc >> px >> py;
            if (idx >= 0 && idx < kThemeWindowCount) {
                legacyW[idx].scale = sc;
                legacyW[idx].posX = px;
                legacyW[idx].posY = py;
            }
        } else if (key == "winbg") {
            int idx = -1;
            float op = 0.5f;
            int fit = 0;
            f >> idx >> op >> fit;
            const std::string p = readRestOfLine(f);
            if (idx >= 0 && idx < kThemeWindowCount) {
                legacyW[idx].image = p;
                legacyW[idx].opacity = op;
                legacyW[idx].fit = fit;
            }
        } else if (key == "col") {
            ColOverride o{};
            f >> o.idx >> o.v.x >> o.v.y >> o.v.z >> o.v.w;
            if (o.idx >= 0 && o.idx < ImGuiCol_COUNT) cols.push_back(o);
        } else if (key == "winov") {
            int idx = -1, en = 0, uA = 0, uB = 0, uT = 0, uR = 0, uP = 0;
            WindowStyleOverride o;
            f >> idx >> en >> uA >> o.accent[0] >> o.accent[1] >> o.accent[2] >> uB >> o.bg >>
                uT >> o.text >> uR >> o.rounding >> uP >> o.panelAlpha;
            o.enabled = en != 0;
            o.useAccent = uA != 0;
            o.useBg = uB != 0;
            o.useText = uT != 0;
            o.useRounding = uR != 0;
            o.usePanelAlpha = uP != 0;
            if (wins && idx >= 0 && idx < kThemeWindowCount) {
                // 레이어가 먼저 읽혔을 수 있으니 배경 목록은 그대로 둔다
                auto keep = std::move(wins[idx].bgLayers);
                wins[idx] = o;
                wins[idx].bgLayers = std::move(keep);
            }
        } else {
            std::string skip;
            std::getline(f, skip);
        }
    }
    // 옛 파일 호환: 레이어 줄이 없고 옛 키만 있으면 한 장짜리 레이어로 옮긴다
    if (loaded.bgLayers.empty() && !legacyG.image.empty())
        loaded.bgLayers.push_back(legacyG);
    if (wins)
        for (int i = 0; i < kThemeWindowCount; ++i)
            if (wins[i].bgLayers.empty() && !legacyW[(std::size_t)i].image.empty())
                wins[i].bgLayers.push_back(legacyW[(std::size_t)i]);

    t = loaded;
    applyThemeParams(t); // 둥글기 등 파생 값 계산
    ImGuiStyle& s = ImGui::GetStyle();
    // 저장된 개별 색이 최종 우선. 다만 패널 불투명도(배경 이미지가 비치는 정도)만은
    // 테마 값이 이긴다 — 예전에 저장된 색의 알파가 1이면 배경이 통째로 가려진다.
    float keepA[IM_ARRAYSIZE(kPanelBgCols)];
    for (int i = 0; i < (int)IM_ARRAYSIZE(kPanelBgCols); ++i)
        keepA[i] = s.Colors[kPanelBgCols[i]].w;
    for (const auto& o : cols) s.Colors[o.idx] = o.v;
    for (int i = 0; i < (int)IM_ARRAYSIZE(kPanelBgCols); ++i)
        s.Colors[kPanelBgCols[i]].w = keepA[i];
    makeSkinKeysUnique(s); // 저장된 색으로 되돌린 뒤에도 스킨 열쇠는 유일해야 한다
    return true;
}

// ---- 테마 공유: 이미지를 파일 안에 함께 담기 ----
namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const std::vector<unsigned char>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = in[i];
        const unsigned b = (i + 1 < in.size()) ? in[i + 1] : 0;
        const unsigned c = (i + 2 < in.size()) ? in[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? kB64[(v >> 6) & 63] : '=';
        out += (i + 2 < in.size()) ? kB64[v & 63] : '=';
    }
    return out;
}

std::vector<unsigned char> b64Decode(const std::string& s) {
    int rev[256];
    for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[(unsigned char)kB64[i]] = i;
    std::vector<unsigned char> out;
    out.reserve(s.size() / 4 * 3);
    unsigned buf = 0;
    int bits = 0;
    for (char ch : s) {
        const int v = rev[(unsigned char)ch];
        if (v < 0) continue; // '=' 이나 공백은 건너뛴다
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<unsigned char> readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize n = f.tellg();
    if (n <= 0) return {};
    std::vector<unsigned char> b((std::size_t)n);
    f.seekg(0);
    f.read((char*)b.data(), n);
    return b;
}

std::string extOf(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "png";
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    if (e.empty() || e.size() > 5) return "png";
    return e;
}

} // namespace

bool exportTheme(const ThemeParams& t, const WindowStyleOverride* wins,
                 const std::filesystem::path& out) {
    // 1) 참조된 이미지들을 모아 번호를 매긴다 (같은 파일은 한 번만)
    std::vector<std::string> paths;
    auto idOf = [&](const std::string& p) -> int {
        if (p.empty()) return -1;
        for (std::size_t i = 0; i < paths.size(); ++i)
            if (paths[i] == p) return (int)i;
        paths.push_back(p);
        return (int)paths.size() - 1;
    };
    // 2) 경로를 "@img:N"으로 바꾼 사본을 만든다 (레이어 전부)
    ThemeParams t2 = t;
    for (auto& L : t2.bgLayers) {
        const int id = idOf(L.image);
        if (id >= 0) L.image = "@img:" + std::to_string(id);
    }
    for (auto& sk : t2.skins) { // 버튼·탭·제목 스킨도 같이 담는다
        const int id = idOf(sk.image);
        if (id >= 0) sk.image = "@img:" + std::to_string(id);
    }

    std::vector<WindowStyleOverride> w2;
    const WindowStyleOverride* w2p = nullptr;
    if (wins) {
        w2.assign(wins, wins + kThemeWindowCount);
        for (auto& o : w2) {
            for (auto& L : o.bgLayers) {
                const int id = idOf(L.image);
                if (id >= 0) L.image = "@img:" + std::to_string(id);
            }
        }
        w2p = w2.data();
    }
    if (!saveTheme(t2, out, w2p)) return false;

    // 3) 이미지 원본을 같은 파일 뒤에 붙인다 (한 줄에 하나 — 옛 로더는 무시)
    std::ofstream f(out, std::ios::app);
    if (!f) return false;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto bytes = readAll(paths[i]);
        if (bytes.empty()) continue; // 파일이 사라졌으면 그림만 빠진다
        f << "imgdata " << i << ' ' << extOf(paths[i]) << ' ' << b64Encode(bytes) << '\n';
    }
    return f.good();
}

bool importTheme(const std::filesystem::path& in, ThemeParams& t, WindowStyleOverride* wins,
                 const std::filesystem::path& assetDir) {
    // 1) 파일에 담긴 이미지들을 꺼내 assetDir에 푼다
    std::map<int, std::string> extracted; // id -> 실제 경로
    {
        std::ifstream f(in);
        if (!f) return false;
        std::error_code ec;
        std::filesystem::create_directories(assetDir, ec);
        const std::string stem = in.stem().string();
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("imgdata ", 0) != 0) continue;
            std::istringstream ls(line);
            std::string key, ext;
            int id = -1;
            ls >> key >> id >> ext;
            std::string b64;
            ls >> b64;
            if (id < 0 || b64.empty()) continue;
            const auto bytes = b64Decode(b64);
            if (bytes.empty()) continue;
            const auto p = assetDir / (stem + "_" + std::to_string(id) + "." + ext);
            std::ofstream o(p, std::ios::binary);
            if (!o) continue;
            o.write((const char*)bytes.data(), (std::streamsize)bytes.size());
            if (o.good()) extracted[id] = p.string();
        }
    }
    // 2) 평소대로 읽는다 (imgdata 줄은 로더가 무시한다)
    if (!loadTheme(t, in, wins)) return false;

    // 3) "@img:N" 을 실제로 푼 파일 경로로 바꾼다
    auto fix = [&](std::string& p) {
        if (p.rfind("@img:", 0) != 0) return;
        const int id = std::atoi(p.c_str() + 5);
        const auto it = extracted.find(id);
        p = (it != extracted.end()) ? it->second : std::string();
    };
    for (auto& L : t.bgLayers) fix(L.image);
    for (auto& sk : t.skins) fix(sk.image); // 못 풀면 빈 문자열 = 스킨 없음
    if (wins)
        for (int i = 0; i < kThemeWindowCount; ++i)
            for (auto& L : wins[i].bgLayers) fix(L.image);
    // 풀지 못한 레이어(원본이 없던 이미지)는 목록에서 빼 준다
    auto dropEmpty = [](std::vector<BgLayer>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const BgLayer& L) { return L.image.empty(); }),
                v.end());
    };
    dropEmpty(t.bgLayers);
    if (wins)
        for (int i = 0; i < kThemeWindowCount; ++i) dropEmpty(wins[i].bgLayers);
    return true;
}

// 프리셋들. 배경 이미지 설정(bgImage 등)은 프리셋이 건드리지 않는다 —
// 색만 바꾸고 사용자가 깔아둔 배경은 그대로 두는 게 자연스럽다.
namespace {
ThemeParams mk(float r, float g, float b, float bg, float text, float rounding,
               float panelAlpha = 1.0f) {
    ThemeParams t;
    t.accent[0] = r; t.accent[1] = g; t.accent[2] = b;
    t.bg = bg;
    t.text = text;
    t.rounding = rounding;
    t.panelAlpha = panelAlpha;
    return t;
}
} // namespace

ThemeParams themeDark() { return ThemeParams{}; }
ThemeParams themeLight() { return mk(0.18f, 0.45f, 0.85f, 0.92f, 0.10f, 4.0f); }
ThemeParams themeMidnight() { return mk(0.10f, 0.75f, 0.65f, 0.06f, 0.90f, 6.0f); }
ThemeParams themeViolet() { return mk(0.62f, 0.40f, 0.95f, 0.10f, 0.95f, 8.0f); }
ThemeParams themeEmerald() { return mk(0.16f, 0.80f, 0.45f, 0.09f, 0.93f, 5.0f); }
ThemeParams themeAmber() { return mk(0.95f, 0.65f, 0.18f, 0.11f, 0.94f, 5.0f); }
ThemeParams themeCrimson() { return mk(0.88f, 0.25f, 0.32f, 0.09f, 0.94f, 4.0f); }
ThemeParams themeOcean() { return mk(0.20f, 0.60f, 0.95f, 0.08f, 0.93f, 7.0f); }
ThemeParams themeSlate() { return mk(0.50f, 0.58f, 0.68f, 0.14f, 0.92f, 3.0f); }
ThemeParams themeNord() { return mk(0.53f, 0.75f, 0.82f, 0.17f, 0.92f, 6.0f); }
ThemeParams themeSolarized() { return mk(0.15f, 0.55f, 0.82f, 0.13f, 0.83f, 3.0f); }
ThemeParams themeHighContrast() { return mk(1.00f, 0.85f, 0.10f, 0.02f, 1.00f, 0.0f); }
ThemeParams themeRose() { return mk(0.95f, 0.45f, 0.65f, 0.11f, 0.95f, 9.0f); }
ThemeParams themeSand() { return mk(0.72f, 0.45f, 0.15f, 0.88f, 0.12f, 5.0f); }

// ---- 모던 프리셋 팩 (2026 추가) — 요즘 DAW 감성의 플랫 팔레트 ----
// 녹턴: 아주 짙은 남보라 배경 + 청록 포인트, 큼직한 둥글기.
ThemeParams themeNocturne() { return mk(0.35f, 0.80f, 0.90f, 0.055f, 0.92f, 8.0f); }
// 드라큘라풍: 짙은 회보라 + 보라/핑크 포인트.
ThemeParams themeDracula() { return mk(0.74f, 0.58f, 0.98f, 0.09f, 0.95f, 7.0f); }
// 선셋: 어두운 자주빛 배경 + 주황·산호 포인트, 따뜻한 느낌.
ThemeParams themeSunset() { return mk(0.98f, 0.48f, 0.35f, 0.085f, 0.94f, 7.0f); }
// 스튜디오: 뉴트럴 다크 그레이 + 라임 포인트 (모니터링 장비 느낌).
ThemeParams themeStudio() { return mk(0.70f, 0.90f, 0.30f, 0.10f, 0.90f, 4.0f); }
// 모노: 미니멀 흑백 + 아주 옅은 청회색 포인트.
ThemeParams themeMono() { return mk(0.62f, 0.66f, 0.72f, 0.07f, 0.90f, 3.0f); }

const ThemePreset* themePresets() {
    static const ThemePreset kPresets[] = {
        {"다크 (기본)", &themeDark},   {"라이트", &themeLight},
        {"미드나잇", &themeMidnight},  {"바이올렛", &themeViolet},
        {"에메랄드", &themeEmerald},   {"앰버", &themeAmber},
        {"크림슨", &themeCrimson},     {"오션", &themeOcean},
        {"슬레이트", &themeSlate},     {"노르드", &themeNord},
        {"솔라라이즈드", &themeSolarized}, {"고대비", &themeHighContrast},
        {"로즈", &themeRose},          {"샌드", &themeSand},
        // 모던 팩
        {"녹턴", &themeNocturne},      {"드라큘라", &themeDracula},
        {"선셋", &themeSunset},        {"스튜디오", &themeStudio},
        {"모노", &themeMono},
    };
    return kPresets;
}
int themePresetCount() { return 19; }

} // namespace midipro::gui
