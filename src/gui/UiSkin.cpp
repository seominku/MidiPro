// =============================================================
// MidiPro - gui/UiSkin.cpp
// =============================================================

#include "gui/UiSkin.h"

#include "gui/BackgroundImage.h"

#include "imgui.h"
#include "imgui_internal.h" // ImFontAtlasTextureBlockQueueUpload

#include <algorithm>
#include <cstring>

namespace midipro::gui {

namespace {

// 아틀라스에 넣을 최대 크기. 버튼/탭은 작게 그려지므로 이 정도면 충분하고,
// 크게 잡으면 폰트 아틀라스가 통째로 커져 낭비가 된다.
constexpr int kMaxSide = 192;

std::uint32_t rgbOf(const ImVec4& c) {
    return ImGui::ColorConvertFloat4ToU32(c) & 0x00FFFFFFu;
}

// 열쇠로 쓰는 색들 (Theme.cpp의 kSkinKeyCols와 같은 목록)
const int kKeyCols[] = {
    ImGuiCol_Button,    ImGuiCol_ButtonHovered,     ImGuiCol_ButtonActive,
    ImGuiCol_Tab,       ImGuiCol_TabHovered,        ImGuiCol_TabSelected,
    ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected,
    ImGuiCol_TitleBg,   ImGuiCol_TitleBgActive,     ImGuiCol_TitleBgCollapsed,
};

} // namespace

bool UiSkinner::any() const {
    for (const auto& s : m_slot)
        if (s.rectId >= 0) return true;
    return false;
}

void UiSkinner::writePixels(Slot& s) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImTextureData* tex = atlas ? atlas->TexData : nullptr;
    if (!atlas || !tex || !tex->Pixels || s.rectId < 0 || s.rgba.empty()) return;
    if (tex->Format != ImTextureFormat_RGBA32 || tex->BytesPerPixel != 4) return;

    ImFontAtlasRect r;
    if (!atlas->GetCustomRect(s.rectId, &r)) return;
    if (r.w < s.w || r.h < s.h) return;

    for (int y = 0; y < s.h; ++y)
        std::memcpy(tex->GetPixelsAt(r.x, r.y + y), &s.rgba[(std::size_t)y * s.w * 4],
                    (std::size_t)s.w * 4);
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, s.w, s.h);

    s.texUid = tex->UniqueID; // 아틀라스가 새로 만들어지면 달라진다
    s.atX = r.x;
    s.atY = r.y;
}

void UiSkinner::sync(const ThemeParams& t) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (!atlas) return;
    m_err.clear();

    for (int i = 0; i < kSkinSlotCount; ++i) {
        Slot& s = m_slot[i];
        const UiSkin& want = t.skins[i];
        s.opacity = want.opacity;
        s.ofsL = want.ofsL;
        s.ofsR = want.ofsR;
        s.ofsT = want.ofsT;
        s.ofsB = want.ofsB;

        if (want.image != s.path) { // 새로 지정했거나 비웠다
            if (s.rectId >= 0) atlas->RemoveCustomRect(s.rectId);
            s.rectId = -1;
            s.rgba.clear();
            s.w = s.h = 0;
            s.texUid = -1;
            s.path = want.image;
            if (!s.path.empty()) {
                if (!decodeImageRgba(s.path, kMaxSide, s.w, s.h, s.rgba) || s.w <= 0) {
                    s.rgba.clear();
                    s.w = s.h = 0;
                    if (!m_err.empty()) m_err += ", ";
                    m_err += uiSkinSlotName(i);
                    continue;
                }
                atlas->TexPixelsUseColors = true; // 컬러 픽셀이 들어간다고 알림
                s.rectId = atlas->AddCustomRect(s.w, s.h);
                if (s.rectId >= 0) writePixels(s);
            }
            continue;
        }

        // 아틀라스가 커지거나 다시 만들어지면 자리도 픽셀도 날아간다 — 다시 써넣는다
        if (s.rectId >= 0) {
            ImTextureData* tex = atlas->TexData;
            ImFontAtlasRect r;
            if (tex && atlas->GetCustomRect(s.rectId, &r) &&
                (tex->UniqueID != s.texUid || r.x != s.atX || r.y != s.atY))
                writePixels(s);
        }
    }
}

void UiSkinner::collectKeys(const ThemeParams& base, const WindowStyleOverride* wins) {
    m_keys.clear();
    if (!any()) return;

    // 1) 이번 프레임에 쓰이는 팔레트들 (전체 + 색을 따로 쓰는 창들)
    std::vector<ImGuiStyle> styles;
    styles.push_back(ImGui::GetStyle());
    if (wins) {
        const ImGuiStyle saved = ImGui::GetStyle();
        bool touched = false;
        for (int i = 0; i < kThemeWindowCount; ++i) {
            if (!wins[i].enabled || !wins[i].anyField()) continue;
            applyThemeParams(effectiveParams(base, wins[i]));
            styles.push_back(ImGui::GetStyle());
            touched = true;
        }
        if (touched) ImGui::GetStyle() = saved; // 원래 스타일로 되돌린다
    }

    // 2) 열쇠가 아닌 색들을 모아 둔다. Theme.cpp가 팔레트 안에서는 열쇠 색을
    //    유일하게 만들어 주지만, 창별 팔레트끼리는 겹칠 수 있다 — 그런 색은
    //    아예 쓰지 않는다(엉뚱한 위젯이 이미지로 바뀌는 것보다 안 입히는 게 낫다).
    std::vector<std::uint32_t> block;
    block.reserve(styles.size() * ImGuiCol_COUNT);
    for (const auto& s : styles)
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            bool isKey = false;
            for (int k : kKeyCols)
                if (k == i) { isKey = true; break; }
            if (!isKey) block.push_back(rgbOf(s.Colors[i]));
        }
    std::sort(block.begin(), block.end());
    block.erase(std::unique(block.begin(), block.end()), block.end());

    // 3) 열쇠 등록
    auto add = [&](const ImVec4& c, int slot, float tint) {
        if (m_slot[slot].rectId < 0) return;
        const std::uint32_t k = rgbOf(c);
        if (k == 0x00FFFFFFu) return; // 흰색은 이미지/글자와 겹칠 수 있어 제외
        if (std::binary_search(block.begin(), block.end(), k)) return;
        for (const auto& e : m_keys)
            if (e.rgb == k) return;
        m_keys.push_back(Key{k, slot, tint});
    };
    for (const auto& s : styles) {
        add(s.Colors[ImGuiCol_Button], kSkinButton, 1.0f);
        add(s.Colors[ImGuiCol_ButtonHovered], kSkinButton, 1.20f);
        add(s.Colors[ImGuiCol_ButtonActive], kSkinButton, 0.82f);
        add(s.Colors[ImGuiCol_Tab], kSkinTab, 0.88f);
        add(s.Colors[ImGuiCol_TabHovered], kSkinTab, 1.20f);
        add(s.Colors[ImGuiCol_TabSelected], kSkinTab, 1.0f);
        add(s.Colors[ImGuiCol_TabDimmed], kSkinTab, 0.75f);
        add(s.Colors[ImGuiCol_TabDimmedSelected], kSkinTab, 0.88f);
        add(s.Colors[ImGuiCol_TitleBg], kSkinTitle, 0.85f);
        add(s.Colors[ImGuiCol_TitleBgActive], kSkinTitle, 1.0f);
        add(s.Colors[ImGuiCol_TitleBgCollapsed], kSkinTitle, 0.7f);
    }
}

void UiSkinner::apply(ImDrawData* dd) {
    if (!dd || m_keys.empty()) return;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (!atlas) return;

    // 이번 프레임의 UV (아틀라스가 바뀌었을 수 있으니 매번 새로 읽는다)
    ImVec2 uv0[kSkinSlotCount], uv1[kSkinSlotCount];
    bool ready[kSkinSlotCount] = {};
    for (int i = 0; i < kSkinSlotCount; ++i) {
        ImFontAtlasRect r;
        if (m_slot[i].rectId >= 0 && atlas->GetCustomRect(m_slot[i].rectId, &r)) {
            uv0[i] = r.uv0;
            uv1[i] = r.uv1;
            ready[i] = true;
        }
    }

    for (int n = 0; n < dd->CmdListsCount; ++n) {
        ImDrawList* dl = dd->CmdLists[n];
        ImDrawVert* v = dl->VtxBuffer.Data;
        const int cnt = dl->VtxBuffer.Size;
        int i = 0;
        while (i < cnt) {
            const std::uint32_t rgb = v[i].col & 0x00FFFFFFu;
            const Key* key = nullptr;
            for (const auto& e : m_keys)
                if (e.rgb == rgb) { key = &e; break; }
            if (!key || !ready[key->slot]) { ++i; continue; }

            // 같은 색이 이어지는 구간 = 위젯 사각형 하나 (테두리 정점은 알파만 0이라
            // RGB로만 비교하면 같은 덩어리로 묶인다)
            int j = i;
            float x0 = v[i].pos.x, y0 = v[i].pos.y, x1 = x0, y1 = y0;
            while (j < cnt && (v[j].col & 0x00FFFFFFu) == rgb) {
                x0 = std::min(x0, v[j].pos.x);
                y0 = std::min(y0, v[j].pos.y);
                x1 = std::max(x1, v[j].pos.x);
                y1 = std::max(y1, v[j].pos.y);
                ++j;
            }
            const float w = x1 - x0, h = y1 - y0;
            // 너무 작거나(선/구분자) 화면만 한 덩어리는 건드리지 않는다
            if (j - i < 4 || w < 3.0f || h < 3.0f || w > 4000.0f || h > 4000.0f) {
                i = j;
                continue;
            }

            const int sl = key->slot;
            const Slot& S = m_slot[sl];
            const float tint = key->tint;
            const int tr = (int)std::min(255.0f, 255.0f * tint);
            const float op = std::clamp(S.opacity, 0.0f, 1.0f);

            // 오프셋: 이미지를 붙일 범위를 위젯 사각형에서 각 방향으로 늘리거나 줄인다.
            // 늘리면 그만큼 넘쳐서 잘려 보이고, 줄이면 가장자리는 이미지 끝 색이 늘어난다.
            float mx0 = x0 - S.ofsL, mx1 = x1 + S.ofsR;
            float my0 = y0 - S.ofsT, my1 = y1 + S.ofsB;
            if (mx1 - mx0 < 1.0f) { mx0 = x0; mx1 = x1; } // 뒤집히면 오프셋 무시
            if (my1 - my0 < 1.0f) { my0 = y0; my1 = y1; }
            const float mw = mx1 - mx0, mh = my1 - my0;

            for (int k = i; k < j; ++k) {
                const float u = std::clamp((v[k].pos.x - mx0) / mw, 0.0f, 1.0f);
                const float t = std::clamp((v[k].pos.y - my0) / mh, 0.0f, 1.0f);
                v[k].uv.x = uv0[sl].x + u * (uv1[sl].x - uv0[sl].x);
                v[k].uv.y = uv0[sl].y + t * (uv1[sl].y - uv0[sl].y);
                const unsigned int a = (v[k].col >> IM_COL32_A_SHIFT) & 0xFF;
                v[k].col = IM_COL32(tr, tr, tr, (int)(a * op));
            }
            i = j;
        }
    }
}

} // namespace midipro::gui
