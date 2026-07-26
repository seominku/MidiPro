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
#include <string>
#include <vector>

namespace midipro::gui {

// 배경 이미지 한 장. 여러 장을 목록 순서대로(앞이 아래) 겹쳐 깐다 —
// 예: 0번에 벽지, 1번에 구석 로고.
struct BgLayer {
    std::string image;   // 파일 경로(UTF-8). 비면 무시
    float opacity = 1.0f;
    int fit = 0;         // 0=채우기 1=맞추기 2=타일 3=직접 지정
    float scale = 1.0f;  // 직접 지정: 원본 대비 배율
    float posX = 0.5f;   // 직접 지정: 0=왼쪽 0.5=가운데 1=오른쪽
    float posY = 0.5f;   // 직접 지정: 0=위 0.5=가운데 1=아래
    bool visible = true; // 잠시 꺼두기
};

// ---- 위젯 스킨 (버튼·탭·제목 표시줄에 입히는 이미지) ----
// 배경 레이어와 달리 창별이 아니라 앱 전체에 한 벌만 둔다 — 창마다 버튼 모양이
// 다르면 오히려 산만하고, 구현도 폰트 아틀라스에 이미지를 얹는 방식이라
// 한 벌로 두는 편이 자연스럽다. GIF는 첫 장만 쓴다(애니메이션 없음).
enum UiSkinSlot { kSkinButton = 0, kSkinTab, kSkinTitle, kSkinSlotCount };
const char* uiSkinSlotName(int slot);

struct UiSkin {
    std::string image; // 비면 스킨 없음 (평소 색 그대로)
    float opacity = 1.0f;
    // 위젯 사각형을 기준으로 이미지를 어디까지 늘려 붙일지 (픽셀).
    // 양수면 그 방향으로 넘쳐서(=잘려) 보이고, 음수면 안쪽으로 당겨진다.
    float ofsL = 0.0f, ofsR = 0.0f, ofsT = 0.0f, ofsB = 0.0f;
};

struct ThemeParams {
    float accent[3] = {0.35f, 0.55f, 0.90f}; // 강조색 (버튼/슬라이더/선택)
    float bg = 0.12f;        // 배경 밝기 (0=검정 ~ 1=흰색)
    float text = 0.95f;      // 글자 밝기
    float rounding = 4.0f;   // 모서리 둥글기 (0~12)
    // 창(패널) 불투명도. 1=꽉 참. 낮추면 뒤의 배경 이미지가 비쳐 보인다.
    float panelAlpha = 1.0f;

    // ---- 배경 이미지/GIF (창 전체 뒤에 깔린다). 여러 장 겹칠 수 있다 ----
    std::vector<BgLayer> bgLayers;

    // ---- 위젯 스킨 (모든 창 공통) ----
    UiSkin skins[kSkinSlotCount];
};

// 파라미터로 ImGui 스타일 전체를 다시 만든다 (즉시 적용).
void applyThemeParams(const ThemeParams& t);

// DPI 배율 (1.0 = 100%). App이 시작 시 창 DPI로 지정하면 buildStyle이
// 여백·둥글기 등 모든 스타일 치수에 곱한다 (폰트는 App이 크게 로드).
void setUiDpiScale(float scale);
float uiDpiScale();

// ---- 창별 스타일 오버라이드 ----
// ImGui 스타일은 전역이라, 창을 그리기 직전에 스타일을 바꿔 끼우고 그린 뒤
// 되돌리는 방식으로 "이 창만 다른 색"을 만든다. 켜지 않은 항목은 전체 테마를
// 그대로 상속하므로, 전체 테마를 바꾸면 커스텀 안 한 부분은 같이 따라온다.
enum ThemeWindow {
    kWinTransport = 0, kWinDevices, kWinTrackList, kWinTrackView, kWinMixer,
    kWinMixerCompact, kWinPerf, kWinPianoRoll, kWinDrums, kWinArrange,
    kWinGuitarTab, kWinSynth, kWinPreferences, kWinExport, kWinBuiltinFx,
    kWinVst, kWinGuitarHelper, kWinMonitor,
    kThemeWindowCount
};
const char* themeWindowName(int win);

struct WindowStyleOverride {
    bool enabled = false; // 꺼져 있으면 전체 테마를 그대로 쓴다
    bool useAccent = false;
    float accent[3] = {0.35f, 0.55f, 0.90f};
    bool useBg = false;
    float bg = 0.12f;
    bool useText = false;
    float text = 0.95f;
    bool useRounding = false;
    float rounding = 4.0f;
    bool usePanelAlpha = false;
    float panelAlpha = 1.0f;
    // 이 창만의 배경 이미지들 (비면 전체 배경만 보인다)
    std::vector<BgLayer> bgLayers;
    // 켜진 항목이 하나라도 있는가 (없으면 색 오버라이드가 의미 없다)
    bool anyField() const {
        return useAccent || useBg || useText || useRounding || usePanelAlpha;
    }
};

// 전체 테마에 오버라이드를 얹은 최종 파라미터
ThemeParams effectiveParams(const ThemeParams& base, const WindowStyleOverride& ov);

// 창 하나를 그리기 직전/직후에 부른다. push가 true를 돌려주면 pop에 그대로 넘긴다.
bool pushWindowStyle(const ThemeParams& base, const WindowStyleOverride& ov);
void popWindowStyle(bool pushed);

// 현재 스타일(파라미터 + 고급 편집기로 바꾼 개별 색 + 창별 오버라이드)을 저장/복원.
// wins는 kThemeWindowCount개 배열 (null이면 창별 설정은 다루지 않는다).
bool saveTheme(const ThemeParams& t, const std::filesystem::path& path,
               const struct WindowStyleOverride* wins = nullptr);
bool loadTheme(ThemeParams& t, const std::filesystem::path& path,
               struct WindowStyleOverride* wins = nullptr); // 적용까지 수행

// ---- 테마 공유 (남에게 보내고, 받은 걸 그대로 쓰기) ----
// 배경 이미지를 절대 경로로 두면 다른 PC에서 그림이 사라지므로, 내보낼 때는
// 이미지 자체를 파일 안에 함께 담아 "한 파일"로 만든다. 가져올 때 그 이미지를
// assetDir에 풀고 경로를 그쪽으로 바꿔 넣으므로, 받은 사람은 바로 쓸 수 있다.
bool exportTheme(const ThemeParams& t, const struct WindowStyleOverride* wins,
                 const std::filesystem::path& out);
bool importTheme(const std::filesystem::path& in, ThemeParams& t,
                 struct WindowStyleOverride* wins, const std::filesystem::path& assetDir);

// 프리셋 목록 (이름 + 값). UI는 이 표를 돌기만 하면 되므로 프리셋을
// 추가할 때 Theme.cpp 한 곳만 고치면 된다.
struct ThemePreset {
    const char* name;
    ThemeParams (*make)();
};
const ThemePreset* themePresets(); // 배열 시작
int themePresetCount();

// 프리셋
ThemeParams themeDark();      // 기본 다크
ThemeParams themeLight();     // 라이트
ThemeParams themeMidnight();  // 아주 어두운 청록
ThemeParams themeViolet();    // 보라 강조
ThemeParams themeEmerald();   // 짙은 초록
ThemeParams themeAmber();     // 호박(주황) 강조
ThemeParams themeCrimson();   // 붉은 강조
ThemeParams themeOcean();     // 푸른 바다
ThemeParams themeSlate();     // 무채색 회청
ThemeParams themeNord();      // 노르드풍 차분한 파랑
ThemeParams themeSolarized(); // 솔라라이즈드 다크(따뜻한 갈색 배경)
ThemeParams themeHighContrast(); // 고대비(검정+노랑, 각진 모서리)
ThemeParams themeRose();      // 로즈 핑크
ThemeParams themeSand();      // 밝은 모래빛 라이트
// 모던 팩
ThemeParams themeNocturne();  // 짙은 남보라 + 청록
ThemeParams themeDracula();   // 회보라 + 보라/핑크
ThemeParams themeSunset();    // 자주빛 + 주황/산호
ThemeParams themeStudio();    // 뉴트럴 그레이 + 라임
ThemeParams themeMono();      // 미니멀 흑백 + 청회색

} // namespace midipro::gui
