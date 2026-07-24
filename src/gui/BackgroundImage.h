#pragma once
// =============================================================
// MidiPro - gui/BackgroundImage.h
// 창 배경에 깔 이미지/움직이는 GIF.
//
// 디코딩은 Windows 내장 WIC를 쓴다 (PNG·JPG·BMP·GIF). 외부 라이브러리를
// 새로 들이지 않아도 되고, GIF는 프레임별 지연·부분 갱신(디스포절)까지
// 규격대로 합성한다. 올라간 프레임은 DX11 텍스처로 보관한다.
// =============================================================

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace midipro::gui {

// 이미지 첫 프레임을 RGBA 픽셀로 꺼낸다 (긴 변이 maxSide를 넘으면 줄인다).
// 위젯 스킨처럼 폰트 아틀라스에 직접 써넣어야 할 때 쓴다.
bool decodeImageRgba(const std::string& utf8Path, int maxSide, int& outW, int& outH,
                     std::vector<std::uint8_t>& outRgba);

class BackgroundImage {
public:
    BackgroundImage() = default;
    ~BackgroundImage();
    BackgroundImage(const BackgroundImage&) = delete;
    BackgroundImage& operator=(const BackgroundImage&) = delete;

    // utf8Path의 이미지를 읽어 텍스처로 올린다. 실패하면 false(기존 것은 비운다).
    bool load(ID3D11Device* dev, const std::string& utf8Path);
    void release();

    bool valid() const { return !m_frames.empty(); }
    int width() const { return m_w; }
    int height() const { return m_h; }
    int frameCount() const { return (int)m_frames.size(); }
    bool animated() const { return m_frames.size() > 1; }
    const std::string& path() const { return m_path; }

    // timeSec 시점에 보여줄 프레임 (정지 이미지면 항상 같은 것).
    ID3D11ShaderResourceView* frameAt(double timeSec) const;

private:
    std::vector<ID3D11ShaderResourceView*> m_frames;
    std::vector<double> m_delays; // 프레임별 표시 시간(초)
    double m_total = 0.0;         // 한 바퀴 길이(초)
    int m_w = 0, m_h = 0;
    std::string m_path;
};

// 배경을 어떻게 앉힐지. fit이 '직접 지정'일 때만 scale/pos를 쓴다.
struct BgPlacement {
    float opacity = 1.0f;
    int fit = 0; // 0=채우기(잘림) 1=맞추기(여백) 2=타일 3=직접 지정
    // fit==3: 원본 크기 대비 배율, 그리고 놓을 위치.
    // 위치는 CSS background-position과 같은 방식 — 0=왼쪽/위, 0.5=가운데,
    // 1=오른쪽/아래. 이미지가 영역보다 크면 그만큼 밀어서 보여줄 부분을 고른다.
    float scale = 1.0f;
    float posX = 0.5f;
    float posY = 0.5f;
};

// 배경을 뷰포트 전체에 그린다 (모든 창 뒤).
void drawBackgroundImage(const BackgroundImage& img, const BgPlacement& pl, double timeSec);

// ---- 창별 배경 ----
// ImGui는 창 배경을 스스로 그리므로, 이미지는 그 창의 Begin() 직후에
// 그려 넣어야 내용 뒤에 깔린다. 창을 그리기 직전에 setPending...으로
// 예약해 두면, 그 창의 Begin 뒤에서 draw...가 한 번 꺼내 쓴다.
// 겹칠 이미지를 아래에서 위 순서로 하나씩 예약한다.
void addPendingWindowBackground(const BackgroundImage* img, const BgPlacement& pl,
                                double timeSec);
void drawPendingWindowBackground(); // 창 Begin 직후 호출 (예약이 없으면 아무 일도 안 함)
void clearPendingWindowBackground(); // 창을 안 그렸을 때 예약을 흘리지 않도록

} // namespace midipro::gui
