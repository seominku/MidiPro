// =============================================================
// MidiPro - gui/BackgroundImage.cpp
// =============================================================

#include "gui/BackgroundImage.h"

#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace midipro::gui {

namespace {

// COM 스마트 해제 (작은 범위라 최소한으로)
template <class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// 메타데이터에서 정수 하나 읽기 (없으면 def)
uint32_t readUInt(IWICMetadataQueryReader* r, const wchar_t* key, uint32_t def) {
    if (!r) return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    uint32_t out = def;
    if (SUCCEEDED(r->GetMetadataByName(key, &v))) {
        if (v.vt == VT_UI2) out = v.uiVal;
        else if (v.vt == VT_UI4) out = v.ulVal;
        else if (v.vt == VT_UI1) out = v.bVal;
    }
    PropVariantClear(&v);
    return out;
}

// 프레임을 32bpp RGBA(스트레이트 알파)로 펴서 dst에 담는다.
// ImGui DX11 블렌드가 비프리멀티플라이드라 PRGBA가 아니라 RGBA를 쓴다.
bool frameToRgba(IWICImagingFactory* fac, IWICBitmapFrameDecode* frame,
                 std::vector<uint8_t>& dst, uint32_t& w, uint32_t& h) {
    IWICFormatConverter* conv = nullptr;
    if (FAILED(fac->CreateFormatConverter(&conv))) return false;
    bool ok = false;
    if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        if (SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0) {
            dst.resize((std::size_t)w * h * 4);
            const UINT stride = w * 4;
            ok = SUCCEEDED(conv->CopyPixels(nullptr, stride, (UINT)dst.size(), dst.data()));
        }
    }
    safeRelease(conv);
    return ok;
}

ID3D11ShaderResourceView* makeTexture(ID3D11Device* dev, const uint8_t* rgba, int w, int h) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = rgba;
    sd.SysMemPitch = (UINT)w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex)) || !tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = td.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, &vd, &srv);
    tex->Release();
    return srv;
}

} // namespace

// 첫 프레임만 RGBA 픽셀로 꺼낸다 (탭·버튼 스킨용 — 아틀라스에 넣어야 해서 작게).
bool decodeImageRgba(const std::string& utf8Path, int maxSide, int& outW, int& outH,
                     std::vector<uint8_t>& outRgba) {
    outW = outH = 0;
    outRgba.clear();
    if (utf8Path.empty() || maxSide <= 0) return false;

    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(ci);

    IWICImagingFactory* fac = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&fac))))
            break;
        const std::wstring wpath = toWide(utf8Path);
        if (wpath.empty()) break;
        if (FAILED(fac->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &dec)))
            break;
        if (FAILED(dec->GetFrame(0, &fr)) || !fr) break;

        uint32_t w = 0, h = 0;
        if (FAILED(fr->GetSize(&w, &h)) || w == 0 || h == 0) break;

        // 아틀라스 자리를 아끼려고 긴 변을 maxSide로 줄인다 (위젯은 작아서 충분하다)
        IWICBitmapSource* src = fr;
        if ((int)w > maxSide || (int)h > maxSide) {
            const double k = (double)maxSide / (double)(w > h ? w : h);
            const uint32_t nw = (uint32_t)std::max(1.0, (double)w * k);
            const uint32_t nh = (uint32_t)std::max(1.0, (double)h * k);
            if (SUCCEEDED(fac->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(fr, nw, nh, WICBitmapInterpolationModeFant)))
                src = sc;
        }

        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            uint32_t fw = 0, fh = 0;
            if (SUCCEEDED(conv->GetSize(&fw, &fh)) && fw > 0 && fh > 0) {
                outRgba.resize((std::size_t)fw * fh * 4);
                if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)outRgba.size(),
                                               outRgba.data()))) {
                    outW = (int)fw;
                    outH = (int)fh;
                    ok = true;
                }
            }
        }
        safeRelease(conv);
    } while (false);

    safeRelease(sc);
    safeRelease(fr);
    safeRelease(dec);
    safeRelease(fac);
    if (needUninit) CoUninitialize();
    if (!ok) outRgba.clear();
    return ok;
}

BackgroundImage::~BackgroundImage() { release(); }

void BackgroundImage::release() {
    for (auto*& f : m_frames) safeRelease(f);
    m_frames.clear();
    m_delays.clear();
    m_total = 0.0;
    m_w = m_h = 0;
    m_path.clear();
}

bool BackgroundImage::load(ID3D11Device* dev, const std::string& utf8Path) {
    release();
    if (!dev || utf8Path.empty()) return false;

    // 이 스레드에 COM이 이미 올라와 있을 수 있다 (파일 대화상자 등) — 모드가
    // 다르면 RPC_E_CHANGED_MODE가 나는데 그때도 그대로 쓰면 된다.
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(ci);

    IWICImagingFactory* fac = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&fac))))
            break;
        const std::wstring wpath = toWide(utf8Path);
        if (wpath.empty()) break;
        if (FAILED(fac->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &dec)))
            break;
        UINT count = 0;
        if (FAILED(dec->GetFrameCount(&count)) || count == 0) break;

        if (count == 1) { // 정지 이미지 (PNG/JPG/BMP/단일 GIF)
            IWICBitmapFrameDecode* fr = nullptr;
            if (SUCCEEDED(dec->GetFrame(0, &fr)) && fr) {
                std::vector<uint8_t> px;
                uint32_t w = 0, h = 0;
                if (frameToRgba(fac, fr, px, w, h)) {
                    if (auto* srv = makeTexture(dev, px.data(), (int)w, (int)h)) {
                        m_frames.push_back(srv);
                        m_delays.push_back(0.0);
                        m_w = (int)w;
                        m_h = (int)h;
                        ok = true;
                    }
                }
            }
            safeRelease(fr);
            break;
        }

        // 애니메이션 GIF: 전체 화면(논리 스크린) 크기의 캔버스에 프레임을 겹쳐 간다.
        uint32_t canvasW = 0, canvasH = 0;
        {
            IWICMetadataQueryReader* mr = nullptr;
            if (SUCCEEDED(dec->GetMetadataQueryReader(&mr)) && mr) {
                canvasW = readUInt(mr, L"/logscrdesc/Width", 0);
                canvasH = readUInt(mr, L"/logscrdesc/Height", 0);
            }
            safeRelease(mr);
        }
        if (canvasW == 0 || canvasH == 0) { // 메타데이터가 없으면 첫 프레임 크기로
            IWICBitmapFrameDecode* fr = nullptr;
            if (SUCCEEDED(dec->GetFrame(0, &fr)) && fr) fr->GetSize(&canvasW, &canvasH);
            safeRelease(fr);
        }
        if (canvasW == 0 || canvasH == 0) break;

        std::vector<uint8_t> canvas((std::size_t)canvasW * canvasH * 4, 0);
        for (UINT i = 0; i < count; ++i) {
            IWICBitmapFrameDecode* fr = nullptr;
            if (FAILED(dec->GetFrame(i, &fr)) || !fr) { safeRelease(fr); break; }

            uint32_t left = 0, top = 0, delayCs = 10, disposal = 0;
            {
                IWICMetadataQueryReader* mr = nullptr;
                if (SUCCEEDED(fr->GetMetadataQueryReader(&mr)) && mr) {
                    left = readUInt(mr, L"/imgdesc/Left", 0);
                    top = readUInt(mr, L"/imgdesc/Top", 0);
                    delayCs = readUInt(mr, L"/grctlext/Delay", 10);   // 1/100초
                    disposal = readUInt(mr, L"/grctlext/Disposal", 0);
                }
                safeRelease(mr);
            }

            std::vector<uint8_t> px;
            uint32_t fw = 0, fh = 0;
            if (!frameToRgba(fac, fr, px, fw, fh)) { safeRelease(fr); break; }
            safeRelease(fr);

            // GIF는 1비트 투명도라, 알파가 있으면 덮고 없으면 그대로 둔다.
            for (uint32_t y = 0; y < fh; ++y) {
                const uint32_t cy = top + y;
                if (cy >= canvasH) break;
                for (uint32_t x = 0; x < fw; ++x) {
                    const uint32_t cx = left + x;
                    if (cx >= canvasW) break;
                    const uint8_t* s = &px[((std::size_t)y * fw + x) * 4];
                    if (s[3] < 128) continue; // 투명 픽셀 = 이전 화면 유지
                    uint8_t* d = &canvas[((std::size_t)cy * canvasW + cx) * 4];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                }
            }

            if (auto* srv = makeTexture(dev, canvas.data(), (int)canvasW, (int)canvasH)) {
                m_frames.push_back(srv);
                // 0 또는 1(1/100초)은 브라우저 관례대로 0.1초로 본다
                const double sec = (delayCs <= 1 ? 10.0 : (double)delayCs) / 100.0;
                m_delays.push_back(sec);
                m_total += sec;
            }

            // 디스포절 2(배경으로 복원) / 3(이전으로) — 그 자리를 비운다
            if (disposal == 2 || disposal == 3) {
                for (uint32_t y = 0; y < fh; ++y) {
                    const uint32_t cy = top + y;
                    if (cy >= canvasH) break;
                    for (uint32_t x = 0; x < fw; ++x) {
                        const uint32_t cx = left + x;
                        if (cx >= canvasW) break;
                        uint8_t* d = &canvas[((std::size_t)cy * canvasW + cx) * 4];
                        d[0] = d[1] = d[2] = d[3] = 0;
                    }
                }
            }
        }
        if (!m_frames.empty()) {
            m_w = (int)canvasW;
            m_h = (int)canvasH;
            ok = true;
        }
    } while (false);

    safeRelease(dec);
    safeRelease(fac);
    if (needUninit) CoUninitialize();

    if (ok) m_path = utf8Path;
    else release();
    return ok;
}

ID3D11ShaderResourceView* BackgroundImage::frameAt(double timeSec) const {
    if (m_frames.empty()) return nullptr;
    if (m_frames.size() == 1 || m_total <= 0.0) return m_frames[0];
    double t = std::fmod(timeSec, m_total);
    if (t < 0.0) t += m_total;
    for (std::size_t i = 0; i < m_frames.size(); ++i) {
        if (t < m_delays[i]) return m_frames[i];
        t -= m_delays[i];
    }
    return m_frames.back();
}

// 사각형 [p0, p0+sz] 안에 이미지를 배치 방식대로 그린다 (전체 배경·창 배경 공용).
// 영역을 벗어난 부분은 호출자 쪽 클립(창/뷰포트)이 잘라 준다.
static void drawImageInRect(ImDrawList* dl, const BackgroundImage& img, const BgPlacement& pl,
                            double timeSec, ImVec2 p0, ImVec2 sz) {
    const float opacity = pl.opacity;
    int fit = pl.fit;
    if (!img.valid() || opacity <= 0.001f || !dl) return;
    ID3D11ShaderResourceView* srv = img.frameAt(timeSec);
    if (!srv) return;
    if (sz.x <= 0.0f || sz.y <= 0.0f) return;
    const ImU32 tint = IM_COL32(255, 255, 255, (int)(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
    const auto tex = (ImTextureID)srv;
    const float iw = (float)img.width(), ih = (float)img.height();

    if (fit == 3) { // 직접 지정: 배율과 위치를 사용자가 정한다
        const float s = std::clamp(pl.scale, 0.02f, 20.0f);
        const float w = iw * s, h = ih * s;
        // 위치 0=왼쪽/위, 0.5=가운데, 1=오른쪽/아래 (이미지가 크면 그만큼 밀린다)
        const float x = p0.x + (sz.x - w) * pl.posX;
        const float y = p0.y + (sz.y - h) * pl.posY;
        dl->AddImage(tex, ImVec2(x, y), ImVec2(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }

    if (fit == 2) { // 타일 — ImGui DX11 샘플러가 CLAMP라 UV 반복 대신 여러 번 그린다
        const int nx = (int)std::ceil(sz.x / iw), ny = (int)std::ceil(sz.y / ih);
        const int kMaxTiles = 4096; // 아주 작은 이미지로 무한정 그리지 않게
        if (nx > 0 && ny > 0 && nx * ny <= kMaxTiles) {
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    const ImVec2 a(p0.x + x * iw, p0.y + y * ih);
                    dl->AddImage(tex, a, ImVec2(a.x + iw, a.y + ih), ImVec2(0, 0), ImVec2(1, 1),
                                 tint);
                }
            return;
        }
        fit = 0; // 타일이 너무 많으면 채우기로 대체
    }

    const float sx = sz.x / iw, sy = sz.y / ih;
    if (fit == 1) { // 맞추기: 전체가 보이도록 축소, 남는 곳은 여백
        const float s = std::min(sx, sy);
        const ImVec2 d(iw * s, ih * s);
        const ImVec2 a(p0.x + (sz.x - d.x) * 0.5f, p0.y + (sz.y - d.y) * 0.5f);
        dl->AddImage(tex, a, ImVec2(a.x + d.x, a.y + d.y), ImVec2(0, 0), ImVec2(1, 1), tint);
    } else { // 채우기: 화면을 꽉 채우고 넘치는 부분은 UV로 잘라낸다
        const float s = std::max(sx, sy);
        const float uw = sz.x / (iw * s), uh = sz.y / (ih * s); // 보이는 UV 비율
        const float u0 = (1.0f - uw) * 0.5f, v0 = (1.0f - uh) * 0.5f;
        dl->AddImage(tex, p0, ImVec2(p0.x + sz.x, p0.y + sz.y), ImVec2(u0, v0),
                     ImVec2(u0 + uw, v0 + uh), tint);
    }
}

void drawBackgroundImage(const BackgroundImage& img, const BgPlacement& pl, double timeSec) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    drawImageInRect(ImGui::GetBackgroundDrawList(), img, pl, timeSec, vp->Pos, vp->Size);
}

// ---- 창별 배경 (Begin 직후에 한 번 꺼내 쓰는 예약) ----
namespace {
struct PendItem {
    const BackgroundImage* img;
    BgPlacement pl;
};
std::vector<PendItem> g_pend;
double g_pendTime = 0.0;
} // namespace

void addPendingWindowBackground(const BackgroundImage* img, const BgPlacement& pl,
                                double timeSec) {
    if (!img) return;
    g_pend.push_back({img, pl});
    g_pendTime = timeSec;
}

void clearPendingWindowBackground() { g_pend.clear(); }

void drawPendingWindowBackground() {
    if (g_pend.empty()) return;
    // 한 창만 쓰고 반납한다 (그 창이 여는 보조 창엔 안 깔린다)
    const std::vector<PendItem> items = std::move(g_pend);
    g_pend.clear();
    // 창 내용 뒤에 깔리도록 창 자신의 그리기 목록에 먼저, 아래→위 순서로 넣는다.
    // 클립은 ImGui가 창 사각형으로 이미 잡아 준다.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    for (const auto& it : items)
        drawImageInRect(dl, *it.img, it.pl, g_pendTime, p0, sz);
}

} // namespace midipro::gui
