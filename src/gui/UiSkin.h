#pragma once
// =============================================================
// MidiPro - gui/UiSkin.h
// 버튼·탭·제목 표시줄에 이미지를 입힌다 (모든 창 공통).
//
// 왜 이런 방식인가:
//   ImGui에는 위젯 스킨 개념이 없고, 버튼 호출부만 수백 군데다. 그래서
//   "그린 뒤에 바꾸는" 방법을 쓴다 —
//     1) 이미지를 폰트 아틀라스 안에 자리를 얻어 써넣는다(AddCustomRect).
//        위젯 사각형은 원래도 아틀라스 텍스처를 쓰므로, 드로우 콜을 쪼갤
//        필요 없이 UV만 바꾸면 그대로 이미지가 된다.
//     2) ImGui::Render() 뒤에 정점을 훑어 "버튼/탭/제목 색"과 같은 색으로
//        칠해진 덩어리를 찾아, 그 덩어리의 바운딩 박스 기준으로 UV를 다시
//        매긴다. 모서리 둥글기·안티에일리어싱 테두리가 그대로 유지된다.
//   창별로 나누지 않고 한 벌만 두는 이유: 창마다 버튼 모양이 다르면 산만하고,
//   아틀라스 자리도 그만큼 더 든다.
//
// 한계(문서화된 동작):
//   - 애니메이션 GIF는 첫 장만 쓴다.
//   - 색을 열쇠로 찾으므로, 패널이 우연히 같은 색으로 직접 그린 사각형도
//     함께 이미지가 될 수 있다. (슬롯별로 끌 수 있다)
// =============================================================

#include "gui/Theme.h"

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

namespace midipro::gui {

class UiSkinner {
public:
    // 테마의 스킨 설정을 반영한다 (경로가 바뀌었을 때만 다시 읽는다).
    // 매 프레임 불러도 되고, 아틀라스가 다시 만들어지면 알아서 다시 써넣는다.
    void sync(const ThemeParams& t);

    // 이번 프레임에 쓰인 "열쇠 색"을 모은다 (전체 테마 + 켜진 창별 오버라이드).
    void collectKeys(const ThemeParams& base, const WindowStyleOverride* wins);

    // ImGui::Render() 뒤, 백엔드에 넘기기 전에 부른다.
    void apply(ImDrawData* dd);

    bool any() const;
    // 마지막 sync에서 읽지 못한 슬롯이 있으면 그 이름 (없으면 nullptr)
    const char* lastError() const { return m_err.empty() ? nullptr : m_err.c_str(); }

private:
    struct Slot {
        std::string path;                 // 지금 올라가 있는 파일
        float opacity = 1.0f;
        float ofsL = 0.0f, ofsR = 0.0f, ofsT = 0.0f, ofsB = 0.0f; // 붙이는 범위(픽셀)
        int w = 0, h = 0;
        std::vector<std::uint8_t> rgba;   // 아틀라스 재생성 시 다시 써넣으려고 보관
        int rectId = -1;                  // ImFontAtlasRectId
        int texUid = -1;                  // 써넣은 시점의 아틀라스 텍스처
        int atX = -1, atY = -1;           // 써넣은 시점의 좌표
    };
    struct Key {
        std::uint32_t rgb;  // 0x00BBGGRR (ImU32에서 알파만 뺀 값)
        int slot;
        float tint;         // 눌림/올림 상태를 살리는 밝기 배수
    };

    Slot m_slot[kSkinSlotCount];
    std::vector<Key> m_keys;
    std::string m_err;

    void writePixels(Slot& s);
};

} // namespace midipro::gui
