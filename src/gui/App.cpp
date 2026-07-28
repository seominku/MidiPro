// =============================================================
// MidiPro - gui/App.cpp
// Win32 + D3D11 + ImGui 부트스트랩. 구조는 ImGui 공식 예제
// (example_win32_directx11)를 따르되 MidiPro 상태/패널을 연결했다.
// =============================================================

#include "core/PathUtf8.h"
#include "gui/App.h"
#include "gui/PanelsInternal.h" // addTrackEq (새 트랙 기본 EQ)
#include "gui/BackgroundImage.h"
#include "gui/Icons.h"
#include "gui/Settings.h"
#include "gui/UiSkin.h"
#include "gui/Panels.h"

#include "audio/AudioClip.h"
#include "audio/BuiltinFx.h"
#include "audio/Mp3Writer.h"
#include "audio/SynthPreset.h"
#include "audio/WavFile.h"
#include "project/Project.h"
#include "sequencer/SmfFile.h"

#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder(기본 도킹 레이아웃 구성)
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h> // 파일 드래그드롭(WM_DROPFILES)
#include <shobjidl.h> // IFileOpenDialog (폴더 선택)

#include <algorithm>
#include <cstdlib> // strtod (내장 이펙트 파라미터 파싱)
#include <tchar.h>
#include <utility>

// ImGui의 Win32 메시지 핸들러 전방 선언
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace midipro::gui {

namespace {

// ---- 탐색기에서 드롭된 파일 (WM_DROPFILES가 채우고 메인 루프가 소비) ----
std::vector<std::string> g_droppedFiles;
POINT g_dropPoint = {0, 0};

// ---- D3D11 전역 (예제 구조 유지) ----
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) // 하드웨어 미지원 시 WARP(소프트웨어)로 폴백
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level,
                                           &g_context);
    if (FAILED(hr)) return false;

    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                       DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; // Alt 메뉴 비활성화
        break;
    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wParam;
        DragQueryPoint(hdrop, &g_dropPoint); // 클라이언트 좌표
        const UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t wpath[MAX_PATH] = L"";
            if (DragQueryFileW(hdrop, i, wpath, MAX_PATH)) {
                char utf8[MAX_PATH * 4] = "";
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8, sizeof(utf8), nullptr, nullptr);
                g_droppedFiles.emplace_back(utf8);
            }
        }
        DragFinish(hdrop);
        return 0;
    }
    case WM_DPICHANGED: {
        // 퍼모니터 DPI: 다른 배율의 모니터로 옮기면 Windows가 권장 크기를 준다.
        // (폰트 재구축은 아직 안 한다 — 크기만 맞추고, 배율은 시작 시 값 유지)
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// UTF-8 경로의 파일을 바이트로 읽는다 (한글 경로 지원 — 와이드 경로 사용).
std::vector<uint8_t> readFileBytes(const std::string& utf8) {
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring wpath((std::size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wpath[0], wlen);
    std::ifstream f(wpath.c_str(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize size = f.tellg();
    if (size <= 0) return {};
    std::vector<uint8_t> buf((std::size_t)size);
    f.seekg(0);
    f.read((char*)buf.data(), size);
    return buf;
}

// .mid를 통째로 새 곡으로 여는 대신, 그 노트들을 지정 트랙에 재생 위치부터
// 얹는다 (트랙별 MIDI 불러오기 / 드래그 임포트). ppqn이 다르면 틱을 환산한다.
static bool importMidiIntoTrack(AppState& st, int target, const std::string& utf8Path) {
    if (target < 0 || target >= (int)st.song.tracks.size()) return false;
    seq::Song loaded;
    if (!seq::smf::load(loaded, core::pathFromUtf8(utf8Path))) return false;
    const double scale =
        loaded.ppqn > 0 ? (double)st.song.ppqn / (double)loaded.ppqn : 1.0;
    const uint32_t base = st.playPosTick;
    auto& dst = st.song.tracks[(std::size_t)target];
    int added = 0;
    for (auto& srcT : loaded.tracks) {
        for (const auto& ns : seq::extractNotes(srcT)) {
            const uint32_t s = base + (uint32_t)std::llround((double)ns.startTick * scale);
            const uint32_t dur = std::max<uint32_t>(
                1, (uint32_t)std::llround((double)(ns.endTick - ns.startTick) * scale));
            dst.addNote(s, dur, ns.note, ns.velocity ? ns.velocity : 100);
            ++added;
        }
    }
    dst.sortEvents();
    return added > 0;
}

// 경로가 .mid / .midi로 끝나는가 (대소문자 무시)
static bool isMidiPath(const std::string& p) {
    std::string l = p;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return (l.size() > 4 && l.compare(l.size() - 4, 4, ".mid") == 0) ||
           (l.size() > 5 && l.compare(l.size() - 5, 5, ".midi") == 0);
}

// Win32 파일 대화상자. 성공 시 경로를 반환한다.
// outFilterIndex: 사용자가 고른 필터(1부터) — WAV/MP3처럼 형식을 가를 때 쓴다.
std::string fileDialog(HWND owner, bool save, const wchar_t* filter = L"MIDI 파일 (*.mid)\0*.mid\0모든 파일\0*.*\0",
                       const wchar_t* defExt = L"mid", int* outFilterIndex = nullptr) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                     : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (outFilterIndex) *outFilterIndex = (int)ofn.nFilterIndex;
    if (!ok) return {};

    // UTF-16 경로 -> UTF-8
    char utf8[MAX_PATH * 4] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return std::string(utf8);
}

// 폴더 선택 대화상자 (IFileOpenDialog + FOS_PICKFOLDERS). 취소 시 빈 문자열.
std::string pickFolderDialog(HWND owner) {
    std::string result;
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                    char utf8[MAX_PATH * 4] = "";
                    WideCharToMultiByte(CP_UTF8, 0, psz, -1, utf8, sizeof(utf8), nullptr,
                                        nullptr);
                    result = utf8;
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
    return result;
}

} // namespace

App::App(midi::IMidiInput& input, midi::MidiOutputRouter& output, audio::ISynthControl& synth,
         audio::IVstHostControl& vst, audio::IMidi2Input& midi2, audio::IAudioClips& audioClips,
         audio::IAudioInput& audioInput) {
    m_state.input = &input;
    m_state.output = &output;
    m_state.synth = &synth;
    m_state.vst = &vst;
    m_state.midi2 = &midi2;
    m_state.audioClips = &audioClips;
    m_state.audioInput = &audioInput;
    // Player는 라우터(IMidiOutput)에만 의존한다 — 실제 출력 대상은 라우터가 결정.
    m_state.player = std::make_unique<seq::Player>(output);
    // 초기 음색을 신스에 한 번 전달 (스트림이 열리면 콜백이 적용).
    m_state.synth->setParams(m_state.synthParams);
}

App::~App() = default;

// ---------------------------------------------------------
// VST 플러그인 상태(패치) 저장/복원.
// 프로젝트 텍스트에는 플러그인 경로만 있고, 내부 음색/노브 상태는
// 사이드카 폴더의 vst_*.bin 파일에 담는다 (VST3 표준 getState/setState).
// ---------------------------------------------------------
void App::saveVstStates(const std::filesystem::path& projectPath) {
    if (!m_state.vst) return;
    namespace fs = std::filesystem;
    const fs::path dir = project::sidecarDir(projectPath);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return;

    // 이전 상태 파일 제거 (플러그인 구성이 바뀌었을 수 있으므로)
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string fn = e.path().filename().string();
        if (fn.rfind("vst_", 0) == 0) stale.push_back(e.path());
    }
    for (const auto& p : stale) fs::remove(p, ec);

    const auto writeState = [&dir](vst::Vst3Host* h, const std::string& name) {
        if (!h) return;
        std::vector<uint8_t> bytes;
        if (!h->saveState(bytes) || bytes.empty()) return;
        std::ofstream out(dir / name, std::ios::binary);
        if (out) out.write(reinterpret_cast<const char*>(bytes.data()),
                           (std::streamsize)bytes.size());
    };

    for (int ch = 0; ch < 16; ++ch) {
        writeState(m_state.vst->trackInstrumentHost(ch),
                   "vst_ch" + std::to_string(ch) + "_inst.bin");
        const int n = m_state.vst->trackEffectCount(ch);
        for (int j = 0; j < n; ++j)
            writeState(m_state.vst->trackEffectHost(ch, j),
                       "vst_ch" + std::to_string(ch) + "_fx" + std::to_string(j) + ".bin");
    }
    if (m_state.vst->instrumentActive())
        writeState(&m_state.vst->instrumentHost(), "vst_master_inst.bin");
    if (m_state.vst->effectActive())
        writeState(&m_state.vst->effectHost(), "vst_master_fx.bin");
}

bool App::loadClickSampleFile(int kind, const std::string& utf8Path) {
    if (!m_state.audioClips || utf8Path.empty() || kind < 0 || kind > 3) return false;

    // 확장자에 따라 WAV/MP3 디코드
    std::shared_ptr<audio::AudioClip> clip;
    std::string lower = utf8Path;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0) {
        clip = audio::readWavFile(core::pathFromUtf8(utf8Path), "click");
    } else {
        const std::vector<uint8_t> bytes = readFileBytes(utf8Path);
        if (!bytes.empty()) clip = audio::decodeMp3(bytes.data(), bytes.size(), "click");
    }
    if (!clip || clip->frames() == 0) return false;

    // 클릭이 너무 길면 곤란하므로 최대 2초로 자른다
    const std::size_t maxFrames = (std::size_t)clip->sampleRate * 2 * (std::size_t)clip->channels;
    if (clip->pcm.size() > maxFrames) clip->pcm.resize(maxFrames);

    m_state.audioClips->setClickSample(kind, clip);
    std::string* const paths[4] = {&m_state.metroSamplePath, &m_state.countInSamplePath,
                                   &m_state.accentSamplePath,
                                   &m_state.countInAccentSamplePath};
    *paths[kind] = utf8Path;
    return true;
}

// ---------------------------------------------------------
// 내보내기: 재생 없이 오프라인 렌더 (곡 길이와 무관하게 몇 초면 끝난다)
// ---------------------------------------------------------
std::string App::runOfflineExport() {
    if (!m_state.audioClips) return "오디오 엔진을 사용할 수 없습니다";
    const double sr = m_state.audioClips->engineSampleRate();
    if (sr <= 0.0) return "샘플레이트를 알 수 없습니다";
    const int ppqn = m_state.song.ppqn;
    const uint32_t tpb = songTicksPerBar(m_state); // 박자표(4/4·3/4·6/8) 반영

    // 구간 결정: 사용자 지정(틱 = 마디/시간/드래그로 설정) 또는 전체(내용 끝까지)
    uint32_t startTick = 0, endTick = 0;
    if (m_state.exportCustomRange) {
        startTick = m_state.exportStartTick;
        endTick = m_state.exportEndTick;
        if (endTick <= startTick) return "구간이 잘못됐습니다 (끝 지점을 확인)";
    } else {
        endTick = songEndTicks(m_state);
        if (endTick == 0) return "내보낼 내용이 없습니다 (노트나 오디오를 먼저 넣으세요)";
    }

    // MIDI 이벤트 평탄화 (뮤트 트랙 제외, 구간 내만)
    struct Ev {
        uint32_t tick;
        uint8_t s, d1, d2;
    };
    std::vector<Ev> evs;
    for (const auto& t : m_state.song.tracks) {
        if (t.muted) continue;
        if (t.frozen) continue; // 프리즈: 구운 오디오 클립이 대신 들어간다
        for (const auto& e : t.events)
            if (e.tick >= startTick && e.tick < endTick)
                evs.push_back({e.tick, e.status, e.data1, e.data2});
    }
    std::stable_sort(evs.begin(), evs.end(),
                     [](const Ev& a, const Ev& b) { return a.tick < b.tick; });

    // 재생 중이면 정지 (오프라인 렌더는 스트림을 멈춘다)
    if (m_state.player) m_state.player->stop();
    m_state.audioClips->stopAudio();

    // 절대 위치 변환은 템포 맵(곡 중간 템포 변경)을 반영한다
    const int64_t startFrame = (int64_t)(seq::songTickToSec(m_state.song, startTick) * sr);
    const int64_t endFrame = (int64_t)(seq::songTickToSec(m_state.song, endTick) * sr);
    const int64_t tailFrames = (int64_t)(2.0 * sr); // 릴리스/리버브 여운

    // 오토메이션이 있으면 블록마다 그 시점 볼륨/팬을 반영한다
    bool anyAuto = false;
    for (const auto& t2 : m_state.song.tracks)
        if (!t2.volAuto.empty() || !t2.panAuto.empty()) anyAuto = true;
    const auto applyAutoAt = [&](int64_t frame, int unmutedTrack) {
        if (!anyAuto) return;
        const uint32_t tickNow = (uint32_t)(std::max)( // 괄호: windows.h max 매크로 회피
            0.0, seq::songSecToTick(m_state.song, (double)frame / sr));
        rebuildAudioMixAt(m_state, tickNow); // 클립 게인에도 반영
        if (m_state.synth)
            for (std::size_t k = 0; k < m_state.song.tracks.size(); ++k) {
                const auto& t2 = m_state.song.tracks[k];
                const bool mute = t2.muted && (int)k != unmutedTrack;
                const float av = seq::autoValueAt(t2.volAuto, tickNow, t2.volume);
                const float ap = seq::autoValueAt(t2.panAuto, tickNow, t2.pan);
                m_state.synth->setChannelMix(t2.channel, mute ? 0.0f : av * t2.gain, ap);
            }
    };

    // ── 스템/선택 트랙: 트랙의 버스(포스트 FX·페이더, 마스터 이펙트 제외)를
    //    따로 렌더해 "파일이름_트랙이름" 파일로 저장한다 ──
    if (m_state.exportMode != 0) {
        const bool onlySelected = m_state.exportMode == 2;
        if (onlySelected && m_state.selectedTrack >= (int)m_state.song.tracks.size())
            return "선택된 트랙이 없습니다";
        const auto sanitize = [](std::string s) {
            for (auto& c2 : s)
                if (c2 == '\\' || c2 == '/' || c2 == ':' || c2 == '*' || c2 == '?' ||
                    c2 == '"' || c2 == '<' || c2 == '>' || c2 == '|')
                    c2 = '_';
            return s.empty() ? std::string("track") : s;
        };
        constexpr unsigned kSBlock = 1024;
        std::vector<float> sblock((std::size_t)kSBlock * 2);
        int written = 0;

        // 옵션: 스템에 마스터 리미터 적용 (마스터와 같은 세팅으로 각 스템을 누른다)
        const auto applyStemLimiter = [&](audio::AudioClip& stem) {
            if (!m_state.exportStemLimiter) return;
            audio::BuiltinFx lim(audio::BuiltinFx::kLimiter);
            if (m_state.audioClips)
                if (auto* ml = m_state.audioClips->masterLimiter())
                    for (int pi = 0; pi < 3; ++pi) lim.setParam(pi, ml->param(pi));
            const std::size_t nfr = stem.frames();
            std::vector<float> lb(1024), rb(1024);
            for (std::size_t f2 = 0; f2 < nfr; f2 += 1024) {
                const int n2 = (int)(std::min)((std::size_t)1024, nfr - f2);
                for (int i2 = 0; i2 < n2; ++i2) {
                    lb[(std::size_t)i2] = stem.pcm[(f2 + (std::size_t)i2) * 2];
                    rb[(std::size_t)i2] = stem.pcm[(f2 + (std::size_t)i2) * 2 + 1];
                }
                float* ch2[2] = {lb.data(), rb.data()};
                lim.process(ch2, n2, (double)stem.sampleRate);
                for (int i2 = 0; i2 < n2; ++i2) {
                    stem.pcm[(f2 + (std::size_t)i2) * 2] = lb[(std::size_t)i2];
                    stem.pcm[(f2 + (std::size_t)i2) * 2 + 1] = rb[(std::size_t)i2];
                }
            }
        };
        for (std::size_t ti = 0; ti < m_state.song.tracks.size(); ++ti) {
            const auto& t = m_state.song.tracks[ti];
            if (onlySelected && (int)ti != m_state.selectedTrack) continue;
            if (t.muted && !onlySelected) continue; // 선택 트랙은 뮤트여도 뽑는다
            const bool hasMidi = !t.frozen && !t.events.empty(); // 프리즈면 클립이 대신
            if (!hasMidi && t.clips.empty()) continue;           // 빈 트랙은 건너뜀

            std::vector<Ev> tevs;
            if (hasMidi)
                for (const auto& e : t.events)
                    if (e.tick >= startTick && e.tick < endTick)
                        tevs.push_back({e.tick, e.status, e.data1, e.data2});
            std::stable_sort(tevs.begin(), tevs.end(),
                             [](const Ev& a, const Ev& b) { return a.tick < b.tick; });

            const int bus = t.channel & 0x0F;
            audio::AudioClip stem;
            stem.channels = 2;
            stem.sampleRate = (int)sr;
            stem.pcm.reserve((std::size_t)((endFrame - startFrame + tailFrames) * 2));

            m_state.audioClips->beginOfflineRender(startFrame);
            std::size_t ei = 0;
            for (int64_t f = startFrame; f < endFrame; f += kSBlock) {
                const unsigned n = (unsigned)std::min<int64_t>(kSBlock, endFrame - f);
                const double blockEndSec = (double)(f + n) / sr;
                while (ei < tevs.size() &&
                       seq::songTickToSec(m_state.song, tevs[ei].tick) < blockEndSec + 1e-9) {
                    m_state.audioClips->queueMidi(tevs[ei].s, tevs[ei].d1, tevs[ei].d2);
                    ++ei;
                }
                applyAutoAt(f, (int)ti); // 렌더 대상 트랙은 뮤트여도 소리내야 한다
                m_state.audioClips->renderOfflineBlockBus(bus, sblock.data(), n,
                                                          /*preFx=*/false);
                stem.pcm.insert(stem.pcm.end(), sblock.begin(),
                                sblock.begin() + (std::size_t)n * 2);
            }
            m_state.audioClips->queueMidi((uint8_t)(0xB0 | bus), 123, 0); // 노트 릴리스
            for (int64_t f = 0; f < tailFrames; f += kSBlock) {
                const unsigned n = (unsigned)std::min<int64_t>(kSBlock, tailFrames - f);
                m_state.audioClips->renderOfflineBlockBus(bus, sblock.data(), n,
                                                          /*preFx=*/false);
                stem.pcm.insert(stem.pcm.end(), sblock.begin(),
                                sblock.begin() + (std::size_t)n * 2);
            }
            m_state.audioClips->endOfflineRender();
            stem.trimLen = (int64_t)stem.frames();
            applyStemLimiter(stem);

            std::filesystem::path sp = core::pathFromUtf8(m_state.exportDir);
            sp /= core::pathFromUtf8(m_state.exportFileName + "_" + sanitize(t.name));
            sp.replace_extension(m_state.exportUseMp3 ? L".mp3" : L".wav");
            const bool ok = m_state.exportUseMp3 ? audio::writeMp3File(stem, sp, 192)
                                                 : audio::writeWavFile(stem, sp);
            if (!ok) return "스템 내보내기 실패: " + t.name;
            ++written;
        }
        // 리턴 리버브 스템: Send를 쓰는 트랙이 있으면 공용 리버브 채널도 뽑는다
        bool anySend = false;
        for (const auto& t2 : m_state.song.tracks)
            if (!t2.muted && t2.sendLevel > 0.001f) anySend = true;
        if (anySend && !onlySelected) {
            audio::AudioClip stem;
            stem.channels = 2;
            stem.sampleRate = (int)sr;
            stem.pcm.reserve((std::size_t)((endFrame - startFrame + tailFrames) * 2));
            m_state.audioClips->beginOfflineRender(startFrame);
            std::size_t ei = 0;
            for (int64_t f = startFrame; f < endFrame; f += kSBlock) {
                const unsigned n = (unsigned)std::min<int64_t>(kSBlock, endFrame - f);
                const double blockEndSec = (double)(f + n) / sr;
                while (ei < evs.size() &&
                       seq::songTickToSec(m_state.song, evs[ei].tick) < blockEndSec + 1e-9) {
                    m_state.audioClips->queueMidi(evs[ei].s, evs[ei].d1, evs[ei].d2);
                    ++ei;
                }
                applyAutoAt(f, -1);
                m_state.audioClips->renderOfflineBlockReturn(sblock.data(), n);
                stem.pcm.insert(stem.pcm.end(), sblock.begin(),
                                sblock.begin() + (std::size_t)n * 2);
            }
            for (int ch2i = 0; ch2i < 16; ++ch2i) // 여운을 위해 릴리스
                m_state.audioClips->queueMidi((uint8_t)(0xB0 | ch2i), 123, 0);
            for (int64_t f = 0; f < tailFrames; f += kSBlock) {
                const unsigned n = (unsigned)std::min<int64_t>(kSBlock, tailFrames - f);
                m_state.audioClips->renderOfflineBlockReturn(sblock.data(), n);
                stem.pcm.insert(stem.pcm.end(), sblock.begin(),
                                sblock.begin() + (std::size_t)n * 2);
            }
            m_state.audioClips->endOfflineRender();
            stem.trimLen = (int64_t)stem.frames();
            applyStemLimiter(stem);
            std::filesystem::path sp = core::pathFromUtf8(m_state.exportDir);
            sp /= core::pathFromUtf8(m_state.exportFileName + "_리턴리버브");
            sp.replace_extension(m_state.exportUseMp3 ? L".mp3" : L".wav");
            if (m_state.exportUseMp3 ? audio::writeMp3File(stem, sp, 192)
                                     : audio::writeWavFile(stem, sp))
                ++written;
        }

        if (written == 0)
            return onlySelected ? "선택한 트랙에 내보낼 내용이 없습니다"
                                : "내보낼 트랙이 없습니다 (뮤트/빈 트랙 제외)";
        m_state.showExportDialog = false;
        if (onlySelected) return "선택 트랙 내보내기 완료";
        return "스템 " + std::to_string(written) + "개 내보내기 완료 (" +
               (m_state.exportUseMp3 ? "MP3" : "WAV") + ")";
    }

    audio::AudioClip out;
    out.channels = 2;
    out.sampleRate = (int)sr;
    out.pcm.reserve((std::size_t)((endFrame - startFrame + tailFrames) * 2));

    constexpr unsigned kBlock = 1024;
    std::vector<float> block((std::size_t)kBlock * 2);

    m_state.audioClips->beginOfflineRender(startFrame);
    std::size_t ei = 0;
    for (int64_t f = startFrame; f < endFrame; f += kBlock) {
        const unsigned n = (unsigned)std::min<int64_t>(kBlock, endFrame - f);
        // 이 블록 구간에 속한 이벤트를 밀어 넣는다
        const double blockEndSec = (double)(f + n) / sr;
        while (ei < evs.size() &&
               seq::songTickToSec(m_state.song, evs[ei].tick) < blockEndSec + 1e-9) {
            m_state.audioClips->queueMidi(evs[ei].s, evs[ei].d1, evs[ei].d2);
            ++ei;
        }
        applyAutoAt(f, -1);
        m_state.audioClips->renderOfflineBlock(block.data(), n);
        out.pcm.insert(out.pcm.end(), block.begin(), block.begin() + (std::size_t)n * 2);
    }
    // 구간 끝: 모든 노트를 릴리스시키고 여운을 렌더한다 (자연스러운 끝맺음)
    for (int ch = 0; ch < 16; ++ch)
        m_state.audioClips->queueMidi((uint8_t)(0xB0 | ch), 123, 0); // All Notes Off
    for (int64_t f = 0; f < tailFrames; f += kBlock) {
        const unsigned n = (unsigned)std::min<int64_t>(kBlock, tailFrames - f);
        m_state.audioClips->renderOfflineBlock(block.data(), n);
        out.pcm.insert(out.pcm.end(), block.begin(), block.begin() + (std::size_t)n * 2);
    }
    m_state.audioClips->endOfflineRender();
    out.trimLen = (int64_t)out.frames();

    // 파일 저장: 폴더 + 파일이름 + 형식에 맞는 확장자
    std::filesystem::path p = core::pathFromUtf8(m_state.exportDir);
    p /= core::pathFromUtf8(m_state.exportFileName);
    p.replace_extension(m_state.exportUseMp3 ? L".mp3" : L".wav");

    bool ok;
    if (m_state.exportUseMp3)
        ok = audio::writeMp3File(out, p, /*kbps=*/192);
    else
        ok = audio::writeWavFile(out, p);
    if (ok) m_state.showExportDialog = false;
    return ok ? (m_state.exportUseMp3 ? "MP3 내보내기 완료 (192kbps)" : "WAV 내보내기 완료")
              : "내보내기 실패 (경로/형식 확인)";
}

// ---------------------------------------------------------
// 프로젝트 저장/불러오기 (메뉴와 자동 저장/복구가 공용으로 쓴다)
// ---------------------------------------------------------
project::ProjectData App::buildProjectData() const {
    project::ProjectData pd;
    pd.song = m_state.song;
    pd.synth = m_state.synthParams;
    pd.midiMap = m_state.midiMap;
    pd.mpe = m_state.synth && m_state.synth->mpeMode();
    pd.vstInstrumentPath = m_state.vstInstrumentPath;
    pd.vstInstrumentClass = m_state.vstInstrumentClass;
    pd.vstInstrumentTrack = m_state.vstInstrumentTrack;
    pd.vstEffectPath = m_state.vstEffectPath;
    pd.vstEffectClass = m_state.vstEffectClass;
    pd.metroClickNote = m_state.metroClickNote;
    pd.countInClickNote = m_state.countInClickNote;
    pd.accentClickNote = m_state.accentClickNote;
    pd.countInAccentClickNote = m_state.countInAccentClickNote;
    pd.metroSamplePath = m_state.metroSamplePath;
    pd.countInSamplePath = m_state.countInSamplePath;
    pd.accentSamplePath = m_state.accentSamplePath;
    pd.countInAccentSamplePath = m_state.countInAccentSamplePath;
    pd.metroSigIndex = m_state.metroSigIndex;
    pd.countInBeats = m_state.countInBeats;
    pd.metroVolume = m_state.metroVolume;
    pd.countInVolume = m_state.countInVolume;
    if (m_state.audioClips) { // 마스터 리미터 상태/파라미터
        pd.masterLimiterOn = m_state.audioClips->masterLimiterOn();
        if (auto* lim = m_state.audioClips->masterLimiter()) {
            pd.limiterGainDb = lim->param(0);
            pd.limiterCeilDb = lim->param(1);
            pd.limiterReleaseMs = lim->param(2);
        }
        pd.returnLevel = m_state.audioClips->returnLevel(); // 센드/리턴
        if (auto* rv = m_state.audioClips->returnReverb()) {
            pd.returnRoom = rv->param(0);
            pd.returnDamp = rv->param(1);
        }
    }
    pd.drumSamples.assign(m_state.drumSamplePaths.begin(), m_state.drumSamplePaths.end());
    // 버전 분기 트리 (gui SongSnapshot -> project VersionSnap, 필드 1:1)
    for (const auto& v : m_state.versions) {
        project::VersionSnap vs;
        vs.id = v.id;
        vs.parent = v.parent;
        vs.name = v.name;
        vs.note = v.note;
        vs.song = v.snap.song;
        for (const auto& p : v.snap.places)
            vs.places.push_back({p.startTick, p.speed, p.trimStart, p.trimLen, p.fadeInSec,
                                 p.fadeOutSec, p.gain});
        pd.versions.push_back(std::move(vs));
    }
    pd.versionCurrent = m_state.versionCurrent;
    pd.versionNextId = m_state.versionNextId;
    return pd;
}

bool App::saveProjectTo(const std::filesystem::path& path) {
    if (!project::save(buildProjectData(), path)) return false;
    saveVstStates(path);
    m_state.projectPath = core::pathToUtf8(path); // 이후 "저장"/외부 reload가 쓸 경로
    return true;
}

// 패키지 내보내기: folder에 프로젝트(.midipro) + drumsamples\를 만든다.
// 프로젝트 저장이 오디오 클립 WAV와 VST 상태를 사이드카 폴더로 함께 쓰므로,
// 드럼 샘플만 복사해 상대 경로로 저장하면 폴더째 다른 PC로 옮길 수 있다.
std::string App::exportPackage(const std::string& folderUtf8) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path folder = core::pathFromUtf8(folderUtf8);
    fs::create_directories(folder, ec);

    // 드럼 샘플을 동봉하고, 저장하는 동안만 경로를 상대로 바꿔치기한다
    const auto backup = m_state.drumSamplePaths;
    if (!backup.empty()) {
        const fs::path sdir = folder / L"drumsamples";
        fs::create_directories(sdir, ec);
        std::map<int, std::string> rel;
        for (const auto& ds : backup) {
            const fs::path srcP = core::pathFromUtf8(ds.second);
            fs::path dstP = sdir / srcP.filename();
            std::error_code e1, e2;
            if (fs::exists(dstP, e1)) // 다른 폴더의 동명 파일: 노트 번호로 구분
                dstP = sdir / (std::to_wstring(ds.first) + L"_" + srcP.filename().wstring());
            fs::copy_file(srcP, dstP, fs::copy_options::overwrite_existing, e2);
            rel[ds.first] = e2 ? ds.second // 복사 실패 시 원 경로 그대로
                               : core::pathToUtf8(fs::path(L"drumsamples") / dstP.filename());
        }
        m_state.drumSamplePaths = rel;
    }

    // 프로젝트 이름: 현재(최근) 프로젝트 파일명, 없으면 project
    std::wstring name = L"project";
    if (!m_state.recentProjects.empty()) {
        const fs::path cur = core::pathFromUtf8(m_state.recentProjects.front());
        if (!cur.stem().empty()) name = cur.stem().wstring();
    }
    const fs::path proj = folder / (name + L".midipro");
    const bool ok = saveProjectTo(proj);
    m_state.drumSamplePaths = backup; // 이 세션은 원래(절대) 경로를 유지한다
    if (!ok) return "패키지 내보내기 실패 (폴더 권한을 확인하세요)";
    return "패키지 내보내기 완료: " + folderUtf8;
}

bool App::loadProjectFrom(const std::filesystem::path& path) {
    project::ProjectData pd;
    if (!project::load(pd, path)) return false;

    if (m_state.player) m_state.player->stop();
    m_state.snapshot();
    m_state.song = std::move(pd.song);
    // 구버전(소유 개념 이전) 프로젝트의 MIDI 클립: 멤버가 없으면 범위로 담는다
    for (auto& t : m_state.song.tracks)
        for (auto& mc : t.midiClips)
            if (mc.members.empty()) seq::adoptMidiClipMembers(t, mc);
    m_state.selectedTrack = 0;

    // 버전 분기 트리 복원 (project VersionSnap -> gui SongSnapshot)
    m_state.versions.clear();
    int maxId = 0;
    for (auto& vs : pd.versions) {
        AppState::VersionNode n;
        n.id = vs.id;
        n.parent = vs.parent;
        n.name = vs.name;
        n.note = vs.note;
        n.snap.song = std::move(vs.song);
        for (const auto& p : vs.places)
            n.snap.places.push_back({p.startTick, p.speed, p.trimStart, p.trimLen, p.fadeInSec,
                                     p.fadeOutSec, p.gain});
        maxId = (std::max)(maxId, n.id); // 괄호: windows.h max 매크로 회피
        m_state.versions.push_back(std::move(n));
    }
    m_state.versionCurrent = pd.versionCurrent;
    m_state.versionNextId = (std::max)(pd.versionNextId, maxId + 1);
    m_state.versionCtxNode = -1;
    m_state.synthParams = pd.synth;
    m_state.midiMap = pd.midiMap;
    if (m_state.synth) {
        m_state.synth->setParams(m_state.synthParams);
        m_state.synth->setMpeMode(pd.mpe);
    }

    // VST 참조 복원 (경로가 있으면 로드)
    if (m_state.vst) {
        std::string err;
        if (!pd.vstInstrumentPath.empty() &&
            m_state.vst->loadInstrument(pd.vstInstrumentPath, pd.vstInstrumentClass, err)) {
            m_state.vstInstrumentPath = pd.vstInstrumentPath;
            m_state.vstInstrumentClass = pd.vstInstrumentClass;
        }
        m_state.vstInstrumentTrack = pd.vstInstrumentTrack;
        if (!pd.vstEffectPath.empty() &&
            m_state.vst->loadEffect(pd.vstEffectPath, pd.vstEffectClass, err)) {
            m_state.vstEffectPath = pd.vstEffectPath;
            m_state.vstEffectClass = pd.vstEffectClass;
        }

        // 트랙 이펙트 체인 + 트랙 악기 복원: 채널마다 비우고 저장된 순서대로
        // 다시 로드한다. 못 불러온 항목은 목록에서 제거.
        for (int ch = 0; ch < 16; ++ch) {
            m_state.vst->clearTrackEffects(ch);
            m_state.vst->clearTrackInstrument(ch);
        }
        for (auto& t : m_state.song.tracks) {
            const int ch = t.channel & 0x0F;
            for (auto it = t.plugins.begin(); it != t.plugins.end();) {
                bool ok = false;
                if (!it->path.empty()) {
                    if (!it->isInstrument && it->path.rfind("builtin:", 0) == 0) {
                        // 내장 이펙트: "builtin:eq|p0,p1,p2,p3" (파라미터 포함)
                        std::string spec = it->path.substr(8), csv;
                        const auto bar = spec.find('|');
                        if (bar != std::string::npos) {
                            csv = spec.substr(bar + 1);
                            spec.resize(bar);
                        }
                        const int bt = audio::BuiltinFx::typeFromToken(spec.c_str());
                        if (bt >= 0 && m_state.vst->addBuiltinTrackEffect(ch, bt)) {
                            const int idx = m_state.vst->trackEffectCount(ch) - 1;
                            m_state.vst->setTrackEffectEnabled(ch, idx, it->enabled);
                            // classIndex = 사이드체인 키 버스 (내장 이펙트 전용 재활용)
                            m_state.vst->setTrackEffectSidechain(ch, idx, it->classIndex);
                            if (auto* bf = m_state.vst->trackEffectBuiltin(ch, idx)) {
                                const char* p = csv.c_str();
                                for (int pi = 0; pi < audio::BuiltinFx::kNumParams && *p;
                                     ++pi) {
                                    char* endp = nullptr;
                                    bf->setParam(pi, (float)std::strtod(p, &endp));
                                    if (!endp || *endp != ',') break;
                                    p = endp + 1;
                                }
                            }
                            ok = true;
                        }
                    } else if (it->isInstrument) {
                        ok = m_state.vst->loadTrackInstrument(ch, it->path, it->classIndex, err);
                    } else if (m_state.vst->loadTrackEffect(ch, it->path, it->classIndex, err)) {
                        const int idx = m_state.vst->trackEffectCount(ch) - 1;
                        m_state.vst->setTrackEffectEnabled(ch, idx, it->enabled);
                        ok = true;
                    }
                }
                it = ok ? std::next(it) : t.plugins.erase(it);
            }
        }

        // 플러그인 상태(패치) 복원 — 로드가 다 끝난 뒤에
        loadVstStates(path);
    }

    // 메트로놈/카운트인/강조 클릭 소리 + 박자 복원
    m_state.metroClickNote = pd.metroClickNote;
    m_state.countInClickNote = pd.countInClickNote;
    m_state.accentClickNote = pd.accentClickNote;
    m_state.countInAccentClickNote = pd.countInAccentClickNote;
    m_state.metroSigIndex = pd.metroSigIndex;
    m_state.countInBeats = pd.countInBeats;
    m_state.metroVolume = pd.metroVolume;
    m_state.countInVolume = pd.countInVolume;
    if (m_state.audioClips) { // 마스터 리미터 복원
        m_state.audioClips->setMasterLimiter(pd.masterLimiterOn);
        if (auto* lim = m_state.audioClips->masterLimiter()) {
            lim->setParam(0, pd.limiterGainDb);
            lim->setParam(1, pd.limiterCeilDb);
            lim->setParam(2, pd.limiterReleaseMs);
        }
        m_state.audioClips->setReturnLevel(pd.returnLevel); // 센드/리턴 복원
        if (auto* rv = m_state.audioClips->returnReverb()) {
            rv->setParam(0, pd.returnRoom);
            rv->setParam(1, pd.returnDamp);
        }
        // 드럼 샘플 복원 (전부 비우고 저장된 배정만 다시 로드).
        // 상대 경로(패키지 내보내기)는 프로젝트 파일 옆에서 찾는다.
        for (int n = 0; n < 128; ++n) m_state.audioClips->setDrumSample((uint8_t)n, nullptr);
        m_state.drumSamplePaths.clear();
        for (const auto& ds : pd.drumSamples) {
            std::filesystem::path sp = core::pathFromUtf8(ds.second);
            if (sp.is_relative()) sp = path.parent_path() / sp;
            assignDrumSample(m_state, ds.first, core::pathToUtf8(sp));
        }
    }
    m_state.metroSamplePath.clear();
    m_state.countInSamplePath.clear();
    m_state.accentSamplePath.clear();
    m_state.countInAccentSamplePath.clear();
    if (m_state.audioClips)
        for (int k = 0; k < 4; ++k) m_state.audioClips->setClickSample(k, nullptr);
    if (!pd.metroSamplePath.empty()) loadClickSampleFile(0, pd.metroSamplePath);
    if (!pd.countInSamplePath.empty()) loadClickSampleFile(1, pd.countInSamplePath);
    if (!pd.accentSamplePath.empty()) loadClickSampleFile(2, pd.accentSamplePath);
    if (!pd.countInAccentSamplePath.empty()) loadClickSampleFile(3, pd.countInAccentSamplePath);
    m_state.projectPath = core::pathToUtf8(path); // 지금 열려 있는 파일
    return true;
}

// ---------------------------------------------------------
// 외부 제어 명령 (ControlServer가 UI 스레드에서 부른다)
//
// 요청은 "명령 인자..." 한 줄, 응답은 JSON 한 줄.
// 실행은 전부 기존 헬퍼(startPlayback/stopTransport/seekTo/loadProjectFrom...)를
// 그대로 쓴다 — 버튼을 누른 것과 같은 경로라 새로운 위험이 없다.
// ---------------------------------------------------------
std::string App::handleControlCommand(const std::string& line) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    const auto rest = [&]() { // 줄 끝까지 (경로에 공백이 흔하다)
        std::string r;
        std::getline(ls, r);
        while (!r.empty() && (r.front() == ' ' || r.front() == '\t')) r.erase(0, 1);
        while (!r.empty() && (r.back() == ' ' || r.back() == '\r')) r.pop_back();
        return r;
    };
    const auto fail = [](const std::string& why) {
        return "{\"ok\":false,\"error\":\"" + jsonEscape(why) + "\"}";
    };
    const auto okMsg = [](const std::string& msg) {
        return "{\"ok\":true,\"message\":\"" + jsonEscape(msg) + "\"}";
    };

    const int ppqn = m_state.song.ppqn > 0 ? m_state.song.ppqn : seq::kDefaultPpqn;
    const bool playing = m_state.player && m_state.player->isPlaying();
    if (playing) m_state.playPosTick = m_state.player->currentTick();

    // 앱 버전이 아니라 "제어 프로토콜" 버전을 준다 — 호환성에 필요한 건 이쪽이고,
    // 버전 문자열을 또 한 군데 늘리지 않으려는 것도 있다.
    if (cmd == "ping") return "{\"ok\":true,\"app\":\"MidiPro\",\"protocol\":1}";

    if (cmd == "status") {
        std::ostringstream o;
        o << "{\"ok\":true"
          << ",\"playing\":" << (playing ? "true" : "false")
          << ",\"recording\":" << (m_state.recording ? "true" : "false")
          << ",\"positionTick\":" << m_state.playPosTick
          << ",\"positionBeat\":" << ((double)m_state.playPosTick / (double)ppqn)
          << ",\"bpm\":" << m_state.song.bpm << ",\"ppqn\":" << ppqn
          << ",\"projectPath\":\"" << jsonEscape(m_state.projectPath) << "\""
          << ",\"trackCount\":" << m_state.song.tracks.size() << ",\"tracks\":[";
        for (std::size_t i = 0; i < m_state.song.tracks.size(); ++i) {
            const auto& t = m_state.song.tracks[i];
            int notes = 0;
            for (const auto& e : t.events)
                if (e.isNoteOn()) ++notes;
            if (i) o << ',';
            o << "{\"index\":" << i << ",\"name\":\"" << jsonEscape(t.name) << "\""
              << ",\"channel\":" << (int)t.channel << ",\"muted\":" << (t.muted ? "true" : "false")
              << ",\"notes\":" << notes << "}";
        }
        o << "]}";
        return o.str();
    }

    if (cmd == "play") {
        if (!playing) startPlayback(m_state);
        return okMsg("재생");
    }
    if (cmd == "stop") {
        stopTransport(m_state);
        silenceOutput(m_state);
        m_state.statusMessage = "정지";
        return okMsg("정지");
    }
    if (cmd == "toggle") {
        if (playing) {
            stopTransport(m_state);
            silenceOutput(m_state);
            m_state.statusMessage = "정지";
        } else {
            startPlayback(m_state);
        }
        return okMsg(playing ? "정지" : "재생");
    }
    if (cmd == "rewind") {
        seekTo(m_state, 0, /*scrollView=*/true);
        return okMsg("처음으로");
    }
    if (cmd == "seek") {
        double beat = -1.0;
        ls >> beat;
        if (!(beat >= 0.0)) return fail("seek: 박 위치(0 이상)가 필요합니다");
        seekTo(m_state, (uint32_t)(beat * ppqn + 0.5), /*scrollView=*/true);
        return okMsg("이동: " + std::to_string(beat) + "박");
    }
    if (cmd == "tempo") {
        double bpm = 0.0;
        ls >> bpm;
        if (!(bpm >= 20.0 && bpm <= 400.0)) return fail("tempo: 20~400 사이여야 합니다");
        m_state.snapshot();
        m_state.song.bpm = bpm;
        if (m_state.player) m_state.player->setBpm(bpm);
        return okMsg("템포 " + std::to_string((int)bpm) + " BPM");
    }

    if (cmd == "save") {
        if (m_state.projectPath.empty())
            return fail("아직 파일로 저장한 적 없는 곡입니다 — 앱에서 한 번 저장하거나 open으로 여세요");
        const std::string p = m_state.projectPath;
        if (!saveProjectTo(core::pathFromUtf8(p))) return fail("저장 실패: " + p);
        addRecentProject(p);
        m_state.statusMessage = "프로젝트 저장 완료: " + p;
        return okMsg("저장: " + p);
    }
    if (cmd == "open" || cmd == "reload") {
        std::string p = (cmd == "open") ? rest() : m_state.projectPath;
        if (p.empty())
            return fail(cmd == "open" ? "open: 파일 경로가 필요합니다"
                                      : "열려 있는 프로젝트 파일이 없습니다");
        std::error_code ec;
        if (!std::filesystem::exists(core::pathFromUtf8(p), ec)) return fail("파일이 없습니다: " + p);
        stopTransport(m_state);
        if (!loadProjectFrom(core::pathFromUtf8(p))) return fail("불러오기 실패: " + p);
        addRecentProject(p);
        m_state.statusMessage =
            (cmd == "reload" ? "다시 불러옴: " : "프로젝트 불러오기 완료: ") + p;
        return okMsg((cmd == "reload" ? "다시 불러옴: " : "열었습니다: ") + p);
    }

    return fail("모르는 명령: " + cmd +
                " (쓸 수 있는 것: ping status play stop toggle rewind seek tempo save open reload)");
}

// ---------------------------------------------------------
// 성능 창용 시스템 지표 (CPU 사용률 / 온도)
// ---------------------------------------------------------
namespace {

// 시스템 전체 CPU 사용률(%). GetSystemTimes의 두 호출 사이 델타로 계산한다.
// 첫 호출은 기준값만 잡고 -1을 돌려준다.
float querySystemCpuPercent() {
    static unsigned long long prevIdle = 0, prevTotal = 0;
    static bool primed = false;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1.0f;
    const auto q = [](const FILETIME& f) {
        return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
    };
    const unsigned long long idle = q(fi);
    const unsigned long long total = q(fk) + q(fu); // kernel 시간엔 idle이 포함된다
    const unsigned long long dIdle = idle - prevIdle;
    const unsigned long long dTotal = total - prevTotal;
    prevIdle = idle;
    prevTotal = total;
    if (!primed) {
        primed = true;
        return -1.0f;
    }
    if (dTotal == 0) return -1.0f;
    return (float)(100.0 * (1.0 - (double)dIdle / (double)dTotal));
}

} // namespace

// ---------------------------------------------------------
// 최근 프로젝트 목록 (recent.txt: UTF-8 경로 한 줄씩, 최신이 앞)
// ---------------------------------------------------------
void App::loadRecentList() {
    m_state.recentProjects.clear();
    std::ifstream in(autosaveDir() / L"recent.txt");
    std::string line;
    while (std::getline(in, line) && m_state.recentProjects.size() < 8) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) m_state.recentProjects.push_back(line);
    }
}

void App::addRecentProject(const std::string& utf8Path) {
    if (utf8Path.empty()) return;
    auto& v = m_state.recentProjects;
    v.erase(std::remove(v.begin(), v.end(), utf8Path), v.end()); // 중복 제거
    v.insert(v.begin(), utf8Path);
    if (v.size() > 8) v.resize(8);
    std::ofstream out(autosaveDir() / L"recent.txt", std::ios::binary);
    for (const auto& p : v) out << p << "\n";
}

// ---------------------------------------------------------
// 자동 저장 + 크래시 복구
// ---------------------------------------------------------
std::filesystem::path App::autosaveDir() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    std::filesystem::path dir =
        base && base[0] ? std::filesystem::path(base) : std::filesystem::temp_directory_path();
    dir /= L"MidiPro";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void App::maybeAutosave() {
    constexpr double kIntervalSec = 180.0; // 3분마다 검사
    const double now = ImGui::GetTime();
    if (now - m_lastAutosaveCheck < kIntervalSec) return;
    m_lastAutosaveCheck = now;

    // 변경이 없으면 저장하지 않는다 (텍스트 직렬화는 저렴, WAV 쓰기는 비싸다)
    project::ProjectData pd = buildProjectData();
    std::string text = project::serialize(pd);
    if (text == m_lastAutosaveText) return;

    if (project::save(pd, autosaveDir() / L"autosave.midipro")) {
        saveVstStates(autosaveDir() / L"autosave.midipro");
        m_lastAutosaveText = std::move(text);
        m_state.statusMessage = "자동 저장됨";
    }
}

void App::loadVstStates(const std::filesystem::path& projectPath) {
    if (!m_state.vst) return;
    namespace fs = std::filesystem;
    const fs::path dir = project::sidecarDir(projectPath);

    const auto loadState = [&dir](vst::Vst3Host* h, const std::string& name) {
        if (!h) return;
        std::ifstream in(dir / name, std::ios::binary | std::ios::ate);
        if (!in) return; // 상태 파일 없음 (기본 패치 유지)
        const std::streamsize sz = in.tellg();
        if (sz <= 0) return;
        std::vector<uint8_t> bytes((std::size_t)sz);
        in.seekg(0);
        if (in.read(reinterpret_cast<char*>(bytes.data()), sz))
            h->loadState(bytes.data(), bytes.size());
    };

    for (int ch = 0; ch < 16; ++ch) {
        loadState(m_state.vst->trackInstrumentHost(ch),
                  "vst_ch" + std::to_string(ch) + "_inst.bin");
        const int n = m_state.vst->trackEffectCount(ch);
        for (int j = 0; j < n; ++j)
            loadState(m_state.vst->trackEffectHost(ch, j),
                      "vst_ch" + std::to_string(ch) + "_fx" + std::to_string(j) + ".bin");
    }
    if (m_state.vst->instrumentActive())
        loadState(&m_state.vst->instrumentHost(), "vst_master_inst.bin");
    if (m_state.vst->effectActive())
        loadState(&m_state.vst->effectHost(), "vst_master_fx.bin");
}

int App::run() {
    // ---- 창 등록/생성 ----
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandle(nullptr),
                      nullptr,    nullptr,    nullptr, nullptr, L"MidiProWnd", nullptr};
    // 앱 아이콘 (리소스 ID 1 — src/app.rc). 타이틀바/작업표시줄에 표시된다.
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MidiPro", WS_OVERLAPPEDWINDOW, 100, 100,
                              1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // 창 DPI로 UI 배율 결정 (150% 화면이면 1.5). 폰트는 이 배율만큼 크게 로드하고
    // 스타일 치수는 buildStyle이 곱한다 — 4K에서 흐릿하지 않고 크기는 그대로.
    {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto getDpi = user32 ? (GetDpiFn)(void*)GetProcAddress(user32, "GetDpiForWindow")
                             : nullptr;
        const UINT dpi = getDpi ? getDpi(hwnd) : 96;
        setUiDpiScale((float)dpi / 96.0f);
        // 처음 창 크기(1280x800)도 배율만큼 키운다 (최대화 전 복원 크기)
        if (uiDpiScale() != 1.0f)
            SetWindowPos(hwnd, nullptr, 0, 0, (int)(1280 * uiDpiScale()),
                         (int)(800 * uiDpiScale()), SWP_NOMOVE | SWP_NOZORDER);
    }

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED); // 화면을 꽉 채워 시작
    UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE); // 탐색기에서 오디오 파일 드래그드롭 허용

    // ---- ImGui 초기화 ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 키보드 내비게이션(방향키로 위젯 포커스 이동)은 쓰지 않는다 — 방향키를
    // 가로 스크롤 단축키로 쓰면 슬라이더가 엉뚱하게 선택되는 문제가 생긴다.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // 창 위치/크기/도킹 배치를 고정 경로에 저장한다. 기본값(imgui.ini)은
    // 실행 폴더(CWD)에 쓰여서 바로가기/더블클릭 등 실행 방법에 따라 유실된다.
    static std::string layoutIni; // ImGui가 이 포인터를 계속 참조하므로 수명 유지
    layoutIni = (autosaveDir() / L"layout.ini").string();
    io.IniFilename = layoutIni.c_str();

    // 테마: 저장된 사용자 테마가 있으면 복원, 없으면 기본 다크
    if (!loadTheme(m_state.theme, autosaveDir() / L"theme.ini", m_state.windowStyles))
        applyThemeParams(m_state.theme);
    loadRecentList(); // 최근 프로젝트 목록 복원

    // 장치 설정 복원: 마지막에 쓰던 MIDI 입력/출력을 이름으로 찾아 그대로 연다.
    // (번호로 저장하면 장치를 꽂았다 뺐다 할 때 엉뚱한 게 열린다)
    AppSettings settings;
    loadSettings(settings, autosaveDir() / L"settings.ini");
    m_state.softThru = settings.softThru;
    m_state.startScreenOnLaunch = settings.startScreenOnLaunch;
    m_state.showStartScreen = settings.startScreenOnLaunch; // 켜기로 돼 있으면 시작 시 표시
    m_state.controlPipeOn = settings.controlPipe;
    // 외부 제어 통로. 두 번째 인스턴스면 조용히 실패하고 통로 없이 돈다.
    if (m_state.controlPipeOn)
        m_control.start([this](const std::string& line) { return handleControlCommand(line); });
    auto findPort = [](const std::vector<std::string>& ports, const std::string& name) {
        for (int i = 0; i < (int)ports.size(); ++i)
            if (ports[(std::size_t)i] == name) return i;
        return -1;
    };
    if (m_state.input && !settings.midiInPort.empty()) {
        const int idx = findPort(m_state.input->listPorts(), settings.midiInPort);
        if (idx >= 0) {
            m_state.selectedInputPort = idx;
            if (settings.midiInAutoOpen && !m_state.input->isOpen())
                m_state.input->openPort((unsigned)idx);
        }
    }
    if (m_state.output && !settings.midiOutPort.empty()) {
        const int idx = findPort(m_state.output->listPorts(), settings.midiOutPort);
        if (idx >= 0) {
            m_state.selectedOutputPort = idx;
            if (settings.midiOutAutoOpen && !m_state.output->isOpen())
                m_state.output->openPort((unsigned)idx);
        }
    }

    // 한글 폰트: Windows 기본 맑은 고딕 + 한국어 + 기호(화살표·미디어·도형) 글리프.
    // 기호 범위를 넉넉히 넣어 ▶ ■ ● 같은 아이콘 글리프가 확실히 들어오게 한다.
    static ImVector<ImWchar> s_ranges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesKorean());
        const ImWchar extra[] = {
            0x2010, 0x205E, // 일반 문장부호
            0x2190, 0x21FF, // 화살표
            0x2300, 0x23FF, // 기술기호(⏮ ⏹ ⏺ 등)
            0x25A0, 0x25FF, // 도형(▶ ■ ● ◀ ▲ ▼)
            0x2600, 0x26FF, // 기타 기호(♪ ♩ ☰ 등)
            0x2705, 0x27BF, // 체크·화살표 딩벳
            0,
        };
        b.AddRanges(extra);
        b.BuildRanges(&s_ranges);
    }
    // DPI 배율만큼 폰트를 크게 로드한다 — 150% 화면에서 16px 대신 24px로
    // 래스터해 흐릿함 없이 같은 크기로 보인다.
    const float fs = uiDpiScale();
    ImFontConfig fontCfg;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f * fs, &fontCfg,
                                 s_ranges.Data);
    // 아이콘 폰트(Segoe MDL2 Assets, Windows 내장)를 본문 폰트에 병합한다.
    // 문자열 안에 ICON_PLAY 같은 글리프를 섞어 쓸 수 있게 된다 (gui/Icons.h).
    static const ImWchar s_iconRange[] = {ICONS_RANGE_BEGIN, ICONS_RANGE_END, 0};
    {
        ImFontConfig iconCfg;
        iconCfg.MergeMode = true;
        iconCfg.GlyphOffset = ImVec2(0.0f, 2.0f * fs); // 한글 폰트와 세로 중심 맞춤
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", 14.0f * fs, &iconCfg,
                                     s_iconRange);
    }
    // 제목/섹션용 굵은 헤더 폰트 (없으면 일반 맑은 고딕으로 대체). 위계를 준다.
    ImFont* headerFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgunbd.ttf",
                                                      20.0f * fs, &fontCfg, s_ranges.Data);
    if (!headerFont)
        headerFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 20.0f * fs,
                                                  &fontCfg, s_ranges.Data);
    setUiHeaderFont(headerFont);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // 배경 텍스처: 레이어 목록과 1:1로 맞춰 둔다 (전체 배경 + 창별).
    // 레이어를 추가/삭제/교체하면 이 목록을 다시 만든다.
    std::vector<std::unique_ptr<BackgroundImage>> bgTex;
    std::vector<std::unique_ptr<BackgroundImage>> winBgTex[kThemeWindowCount];
    UiSkinner skinner; // 버튼·탭·제목 표시줄 이미지 (모든 창 공통)

    // 레이어 목록대로 텍스처를 올린다. 읽지 못한 레이어는 목록에서 뺀다.
    auto syncLayers = [&](std::vector<BgLayer>& layers,
                          std::vector<std::unique_ptr<BackgroundImage>>& tex) {
        tex.clear();
        for (std::size_t i = 0; i < layers.size();) {
            auto img = std::make_unique<BackgroundImage>();
            if (layers[i].image.empty() || !img->load(g_device, layers[i].image)) {
                layers.erase(layers.begin() + (long)i); // 파일이 사라졌으면 레이어도 제거
                continue;
            }
            tex.push_back(std::move(img));
            ++i;
        }
    };
    auto reloadGlobalBg = [&]() {
        syncLayers(m_state.theme.bgLayers, bgTex);
        // 첫 레이어 정보를 표시용으로 (몇 장인지도 함께)
        m_state.bgImageInfo[0] = '\0';
        if (!bgTex.empty()) {
            const auto& b = *bgTex[0];
            std::snprintf(m_state.bgImageInfo, sizeof(m_state.bgImageInfo),
                          b.animated() ? "%dx%d · GIF %d프레임" : "%dx%d", b.width(),
                          b.height(), b.frameCount());
        }
    };
    auto loadWinBg = [&](int i) {
        syncLayers(m_state.windowStyles[i].bgLayers, winBgTex[i]);
    };

    for (int i = 0; i < kThemeWindowCount; ++i) loadWinBg(i);
    reloadGlobalBg();

    // 명령줄로 받은 프로젝트 열기 (.midipro 파일 연결 / 파일에 끌어다 놓기).
    // 실제 로드는 루프의 recentOpenPath 처리부가 맡는다 — 경로 하나만 넘겨 준다.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                const std::filesystem::path p = argv[i];
                std::error_code ec;
                if (std::filesystem::exists(p, ec)) {
                    m_state.recentOpenPath = core::pathToUtf8(p);
                    break; // 첫 번째로 실재하는 경로만 연다
                }
            }
            LocalFree(argv);
        }
    }

    // 받은 테마 파일(.mptheme) 적용: 담겨 있던 이미지를 풀어 이 PC 경로로 바꾼 뒤
    // 바로 적용하고, '내 테마' 목록에도 넣어 다음에 다시 고를 수 있게 한다.
    auto importThemeFile = [&](const std::filesystem::path& path) {
        const auto themesDir = autosaveDir() / L"themes";
        ThemeParams t;
        WindowStyleOverride w[kThemeWindowCount];
        if (!importTheme(path, t, w, themesDir / L"assets")) {
            m_state.statusMessage = "테마 파일을 읽지 못했습니다";
            return;
        }
        m_state.theme = t;
        for (int i = 0; i < kThemeWindowCount; ++i) m_state.windowStyles[i] = w[i];
        applyThemeParams(m_state.theme);
        reloadGlobalBg();
        for (int i = 0; i < kThemeWindowCount; ++i) loadWinBg(i);
        // 목록에 남기기 (이미지 경로는 이미 이 PC 것으로 바뀌어 있다)
        std::error_code ec;
        std::filesystem::create_directories(themesDir, ec);
        const std::string name = path.stem().string();
        saveTheme(m_state.theme, themesDir / (name + ".mptheme"), m_state.windowStyles);
        m_state.themeListDirty = true;
        m_state.themeDirty = true;
        m_state.statusMessage = "테마 적용: " + name;
    };

    // ---- 크래시 복구: 이전 세션이 락을 남기고 죽었으면 자동 저장본을 제안 ----
    {
        namespace fs = std::filesystem;
        const fs::path lock = autosaveDir() / L"session.lock";
        const fs::path autosaveFile = autosaveDir() / L"autosave.midipro";
        std::error_code ec;
        if (fs::exists(lock, ec) && fs::exists(autosaveFile, ec)) {
            const int r = MessageBoxW(
                hwnd,
                L"이전 세션이 비정상 종료된 것 같습니다.\n"
                L"자동 저장된 프로젝트를 복구할까요?",
                L"MidiPro 복구", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) {
                const bool ok = loadProjectFrom(autosaveFile);
                // 자동 저장본은 "지금 작업 중인 파일"이 아니다 — 여기에 덮어쓰면 안 되므로
                // 경로를 비워 둔다(외부 제어의 save도 막힌다).
                m_state.projectPath.clear();
                m_state.statusMessage =
                    ok ? "자동 저장본 복구됨 — '프로젝트 저장'으로 원하는 위치에 저장하세요"
                       : "자동 저장본 복구 실패";
            }
        }
        std::ofstream(lock) << "running"; // 세션 락 생성 (정상 종료 시 삭제)
    }

    // ---- 메인 루프 ----
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // 위젯 스킨(버튼·탭·제목 이미지)은 폰트 아틀라스를 건드리므로 프레임 밖에서
        if (m_state.skinImageOpenRequested) {
            m_state.skinImageOpenRequested = false;
            const int sl = m_state.skinImageSlot;
            const std::string p = fileDialog(
                hwnd, false,
                L"이미지 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
                L"모든 파일\0*.*\0",
                L"png");
            if (!p.empty() && sl >= 0 && sl < kSkinSlotCount) {
                m_state.theme.skins[sl].image = p;
                m_state.themeDirty = true;
            }
        }
        skinner.sync(m_state.theme);
        if (const char* e = skinner.lastError())
            m_state.statusMessage = std::string(e) + " 이미지를 읽지 못했습니다 (형식 확인)";

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ---- 배경 이미지/GIF ----
        // 요청 처리(열기/제거)를 먼저 하고, 모든 창보다 뒤에 깔리는 배경
        // 그리기 목록에 그린다.
        // 배경 이미지 추가/제거 요청 (전체 = 대상 -1, 그 외 = 그 창)
        if (m_state.bgImageOpenRequested) {
            m_state.bgImageOpenRequested = false;
            const std::string p = fileDialog(
                hwnd, false,
                L"이미지 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
                L"모든 파일\0*.*\0",
                L"png");
            const int tw = m_state.bgImageTargetWindow;
            if (!p.empty()) {
                BgLayer L;
                L.image = p;
                L.opacity = 1.0f;
                auto& layers = (tw < 0) ? m_state.theme.bgLayers
                                        : m_state.windowStyles[tw].bgLayers;
                const std::size_t before = layers.size();
                layers.push_back(L);
                if (tw < 0) reloadGlobalBg();
                else loadWinBg(tw);
                if (layers.size() == before) { // 읽기 실패 -> 목록에서 이미 빠졌다
                    m_state.statusMessage = "이미지를 읽지 못했습니다 (형식 확인)";
                } else {
                    // 배경이 아예 안 보이면 당황하므로 패널을 살짝 비춘다
                    if (tw < 0) {
                        if (m_state.theme.panelAlpha > 0.97f) {
                            m_state.theme.panelAlpha = 0.85f;
                            applyThemeParams(m_state.theme);
                        }
                        m_state.statusMessage =
                            "배경 이미지 추가 (" + std::to_string(layers.size()) + "장)";
                    } else {
                        auto& ov = m_state.windowStyles[tw];
                        if (!ov.usePanelAlpha || ov.panelAlpha > 0.97f) {
                            ov.enabled = true;
                            ov.usePanelAlpha = true;
                            ov.panelAlpha = 0.75f;
                        }
                        m_state.statusMessage =
                            std::string("창 배경 추가: ") + themeWindowName(tw);
                    }
                    m_state.themeDirty = true;
                }
            }
        }
        // 레이어 목록이 바뀌었으면(추가·삭제·순서) 텍스처를 다시 맞춘다
        if (m_state.bgLayersDirty >= 0) {
            const int tw = m_state.bgLayersDirty == 0 ? -1 : m_state.bgLayersDirty - 1;
            m_state.bgLayersDirty = -1;
            if (tw < 0) reloadGlobalBg();
            else if (tw < kThemeWindowCount) loadWinBg(tw);
            m_state.themeDirty = true;
        }

        // 전체 배경: 레이어를 아래에서 위로 겹쳐 그린다
        for (std::size_t i = 0; i < bgTex.size() && i < m_state.theme.bgLayers.size(); ++i) {
            const auto& L = m_state.theme.bgLayers[i];
            if (!L.visible) continue;
            BgPlacement pl;
            pl.opacity = L.opacity;
            pl.fit = L.fit;
            pl.scale = L.scale;
            pl.posX = L.posX;
            pl.posY = L.posY;
            drawBackgroundImage(*bgTex[i], pl, ImGui::GetTime());
        }

        // 전체 화면 도킹 공간 (1.92: 첫 인자는 dockspace id).
        // 배경 이미지가 있으면 가운데 빈 영역을 투명하게 뚫어(PassthruCentralNode)
        // 도킹 호스트 창이 배경을 덮지 않게 한다.
        const ImGuiDockNodeFlags dockFlags =
            bgTex.empty() ? 0 : ImGuiDockNodeFlags_PassthruCentralNode;
        const ImGuiID dockId =
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);

        // 첫 실행 시 화면을 꽉 채우는 기본 레이아웃을 구성한다.
        // 트랜스포트(위) · 트랙/장치(왼쪽) · 피아노 롤(가운데) · 도구(오른쪽) · 모니터(아래).
        static bool dockLayoutBuilt = false;
        if (!dockLayoutBuilt) {
            dockLayoutBuilt = true;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);

            ImGuiID center = dockId;
            // 위 16%였는데 DPI 확대 후 내용이 커져 맨 아래 시크 바가 잘렸다 → 20%
            const ImGuiID top = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.20f, nullptr, &center);
            const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
            const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
            const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
            // 가운데를 위(트랙 뷰)/아래(피아노 롤)로 나눈다
            const ImGuiID centerLower =
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.45f, nullptr, &center);

            ImGui::DockBuilderDockWindow("트랜스포트", top);
            ImGui::DockBuilderDockWindow("MIDI 장치", left);
            ImGui::DockBuilderDockWindow("트랙 목록", left);
            ImGui::DockBuilderDockWindow("믹서", left);
            ImGui::DockBuilderDockWindow("채널 (마스터/선택 트랙)", left); // 믹서와 탭
            ImGui::DockBuilderDockWindow("트랙 뷰", center);
            ImGui::DockBuilderDockWindow("피아노 롤", centerLower);
            ImGui::DockBuilderDockWindow("드럼 트랙", centerLower); // 피아노 롤 옆 탭
            ImGui::DockBuilderDockWindow("VST3 플러그인", right);
            ImGui::DockBuilderDockWindow("브라우저", right); // 기타 도우미 대신 (Tool로 켤 수 있음)
            ImGui::DockBuilderDockWindow("신디사이저", right);
            ImGui::DockBuilderDockWindow("입력 모니터", bottom);
            ImGui::DockBuilderDockWindow("상태", bottom);
            ImGui::DockBuilderFinish(dockId);
        }

        // 입력 큐 펌프 (Rule 3: 로깅은 GUI 스레드에서)
        pumpMonitor(m_state);
        // 버튼으로 친 음의 자동 Note Off 처리
        updatePendingNotes(m_state);
        // 루프/메트로놈을 플레이어에 반영 (트랜스포트 창을 닫아도 유지)
        applyTransportState(m_state);

        // ASIO 장치는 첫 프레임에 한 번만 자동 검색하고 첫 장치를 선택한다.
        // (드라이버 로드가 무거워 매 프레임 스캔하면 안 된다)
        if (!m_state.asioScanned && m_state.audioInput) {
            m_state.asioDevices = m_state.audioInput->listAsioDevices();
            m_state.asioScanned = true;
            m_state.asioDeviceIndex = 0;
            m_state.statusMessage = m_state.asioDevices.empty()
                                        ? "ASIO 장치를 찾지 못했습니다"
                                        : "ASIO 장치: " + m_state.asioDevices[0];
        }

        bool openRequested = false;
        bool saveRequested = false;
        // 프레임 시작 시점에 이미 켜져 있던 스크롤 요청만 이번 프레임에 해제한다.
        // 패널을 그리는 도중(예: 채널 창 마커 클릭) 켜진 요청은 이미 그려진
        // 뷰(트랙 뷰)가 못 봤으므로 다음 프레임까지 남겨 모두 반영하게 한다.
        const bool scrollReqAtFrameStart = m_state.scrollToPlayhead;
        drawMenuBar(m_state, openRequested, saveRequested);
        // 창마다 스타일 오버라이드를 적용해 그린다 (없으면 전역 스타일 그대로).
        // ImGui 스타일이 전역이라, 그리기 직전에 바꿔 끼우고 끝나면 되돌린다.
        auto themed = [&](int win, void (*fn)(AppState&)) {
            const bool pushed = pushWindowStyle(m_state.theme, m_state.windowStyles[win]);
            const auto& ov = m_state.windowStyles[win];
            // 그 창의 Begin 직후에 깔릴 배경들을 아래→위 순서로 예약
            for (std::size_t i = 0; i < winBgTex[win].size() && i < ov.bgLayers.size(); ++i) {
                const auto& L = ov.bgLayers[i];
                if (!L.visible) continue;
                BgPlacement pl;
                pl.opacity = L.opacity;
                pl.fit = L.fit;
                pl.scale = L.scale;
                pl.posX = L.posX;
                pl.posY = L.posY;
                addPendingWindowBackground(winBgTex[win][i].get(), pl, ImGui::GetTime());
            }
            fn(m_state);
            clearPendingWindowBackground(); // 창이 안 그려졌으면 다음 창으로 새지 않게
            popWindowStyle(pushed);
        };
        themed(kWinTransport, &drawTransport);
        themed(kWinDevices, &drawDevices);
        themed(kWinTrackList, &drawTrackList);
        themed(kWinTrackView, &drawTrackView);
        themed(kWinMixer, &drawMixer);
        themed(kWinMixerCompact, &drawMixerCompact);
        themed(kWinPerf, &drawPerf);
        themed(kWinPianoRoll, &drawPianoRoll);
        themed(kWinDrums, &drawDrums);
        themed(kWinArrange, &drawArrange);
        themed(kWinGuitarTab, &drawGuitarTab);
        themed(kWinSynth, &drawSynth);
        themed(kWinPreferences, &drawPreferences);
        themed(kWinExport, &drawExportDialog);
        themed(kWinBuiltinFx, &drawBuiltinFx);
        themed(kWinVst, &drawVst);
        themed(kWinGuitarHelper, &drawGuitarHelper);
        themed(kWinMonitor, &drawMonitor);
        drawBrowser(m_state);     // 좌측 브라우저 (악기·이펙트·최근)
        drawStartScreen(m_state); // 모든 패널 위에 겹치는 시작 화면
        if (scrollReqAtFrameStart) m_state.scrollToPlayhead = false; // 모든 뷰가 반영한 뒤 해제

        // 시작 화면 '새 곡' / '프로젝트 열기' 요청 처리 (메뉴의 새 곡과 같은 동작)
        if (m_state.newSongRequested) {
            m_state.newSongRequested = false;
            stopTransport(m_state);
            m_state.snapshot();
            m_state.song = seq::Song{};
            m_state.selectedTrack = 0;
            m_state.statusMessage = "새 곡을 만들었습니다";
        }
        if (m_state.projectOpenRequested) {
            m_state.projectOpenRequested = false;
            m_state.projectLoadRequested = true; // 아래 공용 처리부가 대화상자를 연다
        }

        if (openRequested) {
            const std::string path = fileDialog(hwnd, /*save=*/false);
            if (!path.empty()) {
                if (m_state.player) m_state.player->stop();
                seq::Song loaded;
                if (seq::smf::load(loaded, core::pathFromUtf8(path))) {
                    m_state.snapshot(); // 불러오기도 되돌릴 수 있게 이전 곡 보존
                    m_state.song = std::move(loaded);
                    m_state.selectedTrack = 0;
                    m_state.statusMessage = "불러오기 완료: " + path;
                } else {
                    m_state.statusMessage = "불러오기 실패 (지원하지 않는 형식)";
                }
            }
        }
        // 특정 트랙에 MIDI 불러오기 (우클릭 메뉴). 새 곡으로 열지 않고 노트만 얹는다.
        if (m_state.midiImportRequested) {
            m_state.midiImportRequested = false;
            const int t = m_state.midiImportTrack;
            const std::string path = fileDialog(hwnd, /*save=*/false);
            if (!path.empty() && t >= 0 && t < (int)m_state.song.tracks.size()) {
                m_state.snapshot();
                const std::size_t sl = path.find_last_of("\\/");
                const std::string nm = sl == std::string::npos ? path : path.substr(sl + 1);
                if (importMidiIntoTrack(m_state, t, path)) {
                    m_state.selectedTrack = t;
                    refreshPlaybackIfPlaying(m_state);
                    m_state.statusMessage =
                        "MIDI 불러오기: " + nm + " → 트랙 " + std::to_string(t + 1);
                } else {
                    m_state.statusMessage = "MIDI 불러오기 실패: " + nm;
                }
            }
        }
        if (saveRequested) {
            const std::string path = fileDialog(hwnd, /*save=*/true);
            if (!path.empty()) {
                m_state.statusMessage =
                    seq::smf::save(m_state.song, core::pathFromUtf8(path)) ? "저장 완료: " + path
                                                                    : "저장 실패";
            }
        }

        // 신스 프리셋 저장/불러오기 (.synth 텍스트 파일)
        static const wchar_t* kPresetFilter = L"신스 프리셋 (*.synth)\0*.synth\0모든 파일\0*.*\0";
        if (m_state.presetSaveRequested) {
            m_state.presetSaveRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/true, kPresetFilter, L"synth");
            if (!path.empty())
                m_state.statusMessage = audio::savePreset(m_state.synthParams, core::pathFromUtf8(path))
                                            ? "프리셋 저장 완료: " + path
                                            : "프리셋 저장 실패";
        }
        if (m_state.presetLoadRequested) {
            m_state.presetLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kPresetFilter, L"synth");
            if (!path.empty()) {
                if (audio::loadPreset(m_state.synthParams, core::pathFromUtf8(path))) {
                    if (m_state.synth) m_state.synth->setParams(m_state.synthParams);
                    m_state.statusMessage = "프리셋 불러오기 완료: " + path;
                } else {
                    m_state.statusMessage = "프리셋 불러오기 실패";
                }
            }
        }

        // 컨트롤러 매핑 저장/불러오기 (.map 텍스트 파일)
        static const wchar_t* kMapFilter = L"MIDI 매핑 (*.map)\0*.map\0모든 파일\0*.*\0";
        if (m_state.mapSaveRequested) {
            m_state.mapSaveRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/true, kMapFilter, L"map");
            if (!path.empty())
                m_state.statusMessage = m_state.midiMap.save(core::pathFromUtf8(path)) ? "매핑 저장 완료: " + path
                                                                   : "매핑 저장 실패";
        }
        if (m_state.mapLoadRequested) {
            m_state.mapLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kMapFilter, L"map");
            if (!path.empty())
                m_state.statusMessage = m_state.midiMap.load(core::pathFromUtf8(path)) ? "매핑 불러오기 완료: " + path
                                                                   : "매핑 불러오기 실패";
        }

        // VST3 플러그인 불러오기 (.vst3 번들/파일 선택)
        static const wchar_t* kVstFilter = L"VST3 플러그인 (*.vst3)\0*.vst3\0모든 파일\0*.*\0";
        if (m_state.vstInstrumentLoadRequested) {
            m_state.vstInstrumentLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kVstFilter, L"vst3");
            if (!path.empty() && m_state.vst) {
                std::string err;
                if (m_state.vst->loadInstrument(path, 0, err)) {
                    m_state.vstInstrumentPath = path;
                    m_state.vstInstrumentClass = 0;
                    m_state.statusMessage = "VST 악기 로드: " + m_state.vst->instrumentHost().activeName();
                } else {
                    m_state.statusMessage = "VST 악기 로드 실패: " + err;
                }
            }
        }
        if (m_state.vstEffectLoadRequested) {
            m_state.vstEffectLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kVstFilter, L"vst3");
            if (!path.empty() && m_state.vst) {
                std::string err;
                if (m_state.vst->loadEffect(path, 0, err)) {
                    m_state.vstEffectPath = path;
                    m_state.vstEffectClass = 0;
                    m_state.statusMessage = "VST 이펙트 로드: " + m_state.vst->effectHost().activeName();
                } else {
                    m_state.statusMessage = "VST 이펙트 로드 실패: " + err;
                }
            }
        }

        // 프로젝트 통째 저장/불러오기 (.midipro)
        static const wchar_t* kProjFilter = L"MidiPro 프로젝트 (*.midipro)\0*.midipro\0모든 파일\0*.*\0";
        if (m_state.projectSaveRequested) {
            m_state.projectSaveRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/true, kProjFilter, L"midipro");
            if (!path.empty()) {
                const bool ok = saveProjectTo(core::pathFromUtf8(path));
                if (ok) addRecentProject(path);
                m_state.statusMessage =
                    ok ? "프로젝트 저장 완료: " + path : "프로젝트 저장 실패";
            }
        }
        if (m_state.projectLoadRequested) {
            m_state.projectLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kProjFilter, L"midipro");
            if (!path.empty()) {
                const bool ok = loadProjectFrom(core::pathFromUtf8(path));
                if (ok) addRecentProject(path);
                m_state.statusMessage =
                    ok ? "프로젝트 불러오기 완료: " + path : "프로젝트 불러오기 실패";
            }
        }
        // 최근 프로젝트 메뉴에서 고른 경로 열기
        if (!m_state.recentOpenPath.empty()) {
            const std::string path = m_state.recentOpenPath;
            m_state.recentOpenPath.clear();
            const bool ok = loadProjectFrom(core::pathFromUtf8(path));
            if (ok) addRecentProject(path); // 맨 앞으로 끌어올린다
            m_state.statusMessage = ok ? "프로젝트 불러오기 완료: " + path
                                       : "열 수 없습니다 (파일이 이동/삭제됐을 수 있음)";
        }

        // 외부 제어 명령 (네임드 파이프). 프로젝트 열기/저장 처리 바로 뒤라
        // 곡을 바꾸는 명령도 위 경로들과 같은 시점에 안전하게 실행된다.
        m_control.poll();

        // 주기적 자동 저장 (3분마다, 변경이 있을 때만)
        maybeAutosave();

        // 성능 창 지표 (창이 열려 있을 때만, 1초 간격으로 갱신)
        if (m_state.showPerf) {
            static double lastPerfPoll = 0.0;
            const double nowT = ImGui::GetTime();
            if (nowT - lastPerfPoll >= 1.0) {
                lastPerfPoll = nowT;
                m_state.sysCpuPercent = querySystemCpuPercent();
            }
        }

        // 테마 변경 시 저장 (슬라이더 드래그 중에는 파일 쓰기를 미룬다)
        if (m_state.themeDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            saveTheme(m_state.theme, autosaveDir() / L"theme.ini", m_state.windowStyles);
            m_state.themeDirty = false;
        }

        // ---- 내 테마: 저장/불러오기/삭제 + 목록 갱신 ----
        {
            namespace fs = std::filesystem;
            const fs::path dir = autosaveDir() / L"themes";
            auto themePath = [&](const std::string& name) {
                return dir / (std::filesystem::path(name).filename().string() + ".mptheme");
            };
            if (m_state.themeSaveRequested) {
                m_state.themeSaveRequested = false;
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string name = m_state.themeSaveName;
                if (saveTheme(m_state.theme, themePath(name), m_state.windowStyles)) {
                    m_state.statusMessage = "테마 저장: " + name;
                    m_state.themeListDirty = true;
                } else {
                    m_state.statusMessage = "테마 저장 실패";
                }
            }
            if (!m_state.themeLoadRequested.empty()) {
                const std::string name = m_state.themeLoadRequested;
                m_state.themeLoadRequested.clear();
                // 불러오기 전에 창별 설정을 비운다 (파일에 없는 창은 전체를 따라가도록)
                for (int i = 0; i < kThemeWindowCount; ++i)
                    m_state.windowStyles[i] = WindowStyleOverride{};
                if (loadTheme(m_state.theme, themePath(name), m_state.windowStyles)) {
                    reloadGlobalBg();                                        // 전체 배경
                    for (int i = 0; i < kThemeWindowCount; ++i) loadWinBg(i); // 창별 배경
                    m_state.themeDirty = true; // 현재 테마로도 저장해 둔다
                    m_state.statusMessage = "테마 불러옴: " + name;
                } else {
                    m_state.statusMessage = "테마를 불러오지 못했습니다: " + name;
                }
            }
            if (!m_state.themeDeleteRequested.empty()) {
                const std::string name = m_state.themeDeleteRequested;
                m_state.themeDeleteRequested.clear();
                std::error_code ec;
                fs::remove(themePath(name), ec);
                m_state.statusMessage = "테마 삭제: " + name;
                m_state.themeListDirty = true;
            }
            // 처음 상태로 되돌리기 (색 + 창별 설정 + 배경 이미지 전부)
            if (m_state.themeResetRequested) {
                m_state.themeResetRequested = false;
                m_state.theme = themeDark();
                for (int i = 0; i < kThemeWindowCount; ++i) {
                    m_state.windowStyles[i] = WindowStyleOverride{};
                    winBgTex[i].clear();
                }
                m_state.themeTargetWindow = -1;
                applyThemeParams(m_state.theme);
                reloadGlobalBg(); // bgImage가 비었으므로 텍스처도 정리된다
                m_state.themeDirty = true;
                m_state.statusMessage = "테마를 기본으로 되돌림";
            }
            // 테마를 한 파일로 내보내기 (배경 이미지까지 담아서)
            if (!m_state.themeExportRequested.empty()) {
                const std::string name = m_state.themeExportRequested;
                m_state.themeExportRequested.clear();
                // 저장해 둔 테마를 그대로 읽어 이미지를 담아 내보낸다
                ThemeParams tmpT;
                WindowStyleOverride tmpW[kThemeWindowCount];
                if (loadTheme(tmpT, themePath(name), tmpW)) {
                    // loadTheme은 스타일을 적용해 버리므로 현재 테마로 되돌린다
                    applyThemeParams(m_state.theme);
                    const std::string out = fileDialog(
                        hwnd, true, L"MidiPro 테마 (*.mptheme)\0*.mptheme\0", L"mptheme");
                    if (!out.empty()) {
                        m_state.statusMessage =
                            exportTheme(tmpT, tmpW, std::filesystem::u8path(out))
                                ? ("테마 내보냄: " + name)
                                : "테마 내보내기 실패";
                    }
                } else {
                    m_state.statusMessage = "테마를 읽지 못했습니다: " + name;
                }
            }
            // 받은 테마 파일 가져오기 (이미지를 풀어 경로를 이 PC 것으로 바꾼다)
            if (m_state.themeImportRequested) {
                m_state.themeImportRequested = false;
                const std::string in = fileDialog(
                    hwnd, false, L"MidiPro 테마 (*.mptheme)\0*.mptheme\0모든 파일\0*.*\0",
                    L"mptheme");
                if (!in.empty()) importThemeFile(std::filesystem::u8path(in));
            }
            if (m_state.themeListDirty) {
                m_state.themeListDirty = false;
                m_state.themeFiles.clear();
                std::error_code ec;
                if (fs::exists(dir, ec)) {
                    for (const auto& e : fs::directory_iterator(dir, ec)) {
                        if (!e.is_regular_file(ec)) continue;
                        if (e.path().extension() != L".mptheme") continue;
                        m_state.themeFiles.push_back(e.path().stem().string());
                    }
                    std::sort(m_state.themeFiles.begin(), m_state.themeFiles.end());
                }
            }
        }

        // 내보내기 창 첫 오픈 시 기본 저장 폴더 = 사용자 음악 폴더
        if (m_state.showExportDialog && m_state.exportDir.empty()) {
            const wchar_t* home = _wgetenv(L"USERPROFILE");
            if (home && home[0]) {
                char utf8[MAX_PATH * 4] = "";
                const std::wstring music = std::wstring(home) + L"\\Music";
                WideCharToMultiByte(CP_UTF8, 0, music.c_str(), -1, utf8, sizeof(utf8), nullptr,
                                    nullptr);
                m_state.exportDir = utf8;
            }
        }

        // 내보내기: 저장 폴더 선택
        if (m_state.exportBrowseRequested) {
            m_state.exportBrowseRequested = false;
            const std::string dir = pickFolderDialog(hwnd);
            if (!dir.empty()) m_state.exportDir = dir;
        }

        // 패키지로 내보내기 (프로젝트 + 드럼 샘플 동봉)
        if (m_state.packageExportRequested) {
            m_state.packageExportRequested = false;
            const std::string folder = pickFolderDialog(hwnd);
            if (!folder.empty()) m_state.statusMessage = exportPackage(folder);
        }

        // 내보내기 실행: 재생 없이 오프라인으로 구간을 렌더해 파일로 저장한다
        if (m_state.exportRunRequested) {
            m_state.exportRunRequested = false;
            m_state.statusMessage = runOfflineExport();
        }

        // 클릭 샘플 선택 (일반/카운트인/강조)
        static const wchar_t* kClickFilter =
            L"오디오 (*.wav;*.mp3)\0*.wav;*.mp3\0모든 파일\0*.*\0";
        {
            int clickKind = -1;
            if (m_state.metroSampleLoadRequested) clickKind = 0;
            else if (m_state.countInSampleLoadRequested) clickKind = 1;
            else if (m_state.accentSampleLoadRequested) clickKind = 2;
            else if (m_state.countInAccentSampleLoadRequested) clickKind = 3;
            if (clickKind >= 0) {
                m_state.metroSampleLoadRequested = false;
                m_state.countInSampleLoadRequested = false;
                m_state.accentSampleLoadRequested = false;
                m_state.countInAccentSampleLoadRequested = false;
                const std::string path = fileDialog(hwnd, /*save=*/false, kClickFilter, L"wav");
                if (!path.empty()) {
                    m_state.statusMessage = loadClickSampleFile(clickKind, path)
                                                ? "클릭 샘플 설정 완료"
                                                : "클릭 샘플 로드 실패 (WAV/MP3 확인)";
                }
            }
        }

        // MP3 임포트 (선택 트랙에 오디오 클립으로 붙인다)
        if (m_state.audioImportRequested) {
            m_state.audioImportRequested = false;
            const std::string path = fileDialog(
                hwnd, /*save=*/false,
                L"오디오 (*.mp3;*.wav;*.flac)\0*.mp3;*.wav;*.flac\0모든 파일\0*.*\0", L"mp3");
            if (!path.empty()) {
                const std::vector<uint8_t> bytes = readFileBytes(path);
                const std::size_t slash = path.find_last_of("\\/");
                const std::string name =
                    slash == std::string::npos ? path : path.substr(slash + 1);
                auto clip = audio::decodeAudioAuto(bytes.data(), bytes.size(), name);
                if (clip) {
                    if (m_state.song.tracks.empty()) {
                        m_state.song.tracks.push_back(seq::Track{});
                        m_state.selectedTrack = 0;
                        addTrackEq(m_state, m_state.song.tracks.back()); // 기본 EQ
                    }
                    m_state.snapshot();
                    auto& tr = m_state.song.tracks[m_state.selectedTrack];
                    clip->startTick = m_state.playPosTick; // 현재 위치에 배치
                    tr.clips.push_back(std::move(clip));   // 기존 클립을 유지하고 추가
                    if (tr.clips.size() == 1) tr.name = name;
                    m_state.statusMessage = "오디오 임포트: " + name;
                } else {
                    m_state.statusMessage = "디코드 실패 (MP3/WAV/FLAC 지원)";
                }
            }
        }

        // 탐색기에서 드롭된 오디오 파일 처리: 드롭 지점 아래 트랙에 붙인다
        if (!g_droppedFiles.empty()) {
            POINT scr = g_dropPoint;
            ClientToScreen(hwnd, &scr);
            int target = m_state.selectedTrack;
            for (std::size_t i = 0; i < m_state.laneRects.size(); ++i)
                if ((float)scr.y >= m_state.laneRects[i].y0 &&
                    (float)scr.y < m_state.laneRects[i].y1) {
                    target = (int)i;
                    break;
                }
            for (const std::string& path : g_droppedFiles) {
                // 테마 파일을 떨어뜨리면 바로 적용한다 (남이 보내준 테마 쓰기)
                {
                    std::string lower = path;
                    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
                    if (lower.size() > 8 &&
                        lower.compare(lower.size() - 8, 8, ".mptheme") == 0) {
                        importThemeFile(std::filesystem::u8path(path));
                        continue;
                    }
                }
                // MIDI 파일을 떨어뜨리면 새 창/새 곡이 아니라 "떨어뜨린 트랙"에
                // 노트를 얹는다 (없는 자리에 떨어뜨리면 새 트랙을 만든다).
                if (isMidiPath(path)) {
                    if (target < 0 || target >= (int)m_state.song.tracks.size()) {
                        addTrack(m_state);
                        target = m_state.selectedTrack;
                    }
                    m_state.snapshot();
                    const std::size_t sl = path.find_last_of("\\/");
                    const std::string nm = sl == std::string::npos ? path : path.substr(sl + 1);
                    if (importMidiIntoTrack(m_state, target, path)) {
                        m_state.selectedTrack = target;
                        refreshPlaybackIfPlaying(m_state);
                        m_state.statusMessage = "MIDI 임포트: " + nm + " → 트랙 " +
                                                std::to_string(target + 1);
                    } else {
                        m_state.statusMessage = "MIDI 임포트 실패: " + nm;
                    }
                    continue;
                }
                const std::vector<uint8_t> bytes = readFileBytes(path);
                const std::size_t slash = path.find_last_of("\\/");
                const std::string name =
                    slash == std::string::npos ? path : path.substr(slash + 1);
                auto clip = audio::decodeAudioAuto(bytes.data(), bytes.size(), name);
                if (!clip) {
                    m_state.statusMessage = "오디오 아님/디코드 실패: " + name;
                    continue;
                }
                if (target < 0 || target >= (int)m_state.song.tracks.size()) {
                    m_state.song.tracks.push_back(seq::Track{});
                    target = (int)m_state.song.tracks.size() - 1;
                    addTrackEq(m_state, m_state.song.tracks.back()); // 기본 EQ
                }
                m_state.snapshot();
                clip->startTick = m_state.playPosTick;
                auto& tr = m_state.song.tracks[target];
                tr.clips.push_back(std::move(clip)); // 기존 클립을 유지하고 추가
                if (tr.clips.size() == 1) tr.name = name;
                m_state.selectedTrack = target;
                m_state.statusMessage = "드롭 임포트: " + name;
            }
            g_droppedFiles.clear();
        }

        // 상태 표시줄
        if (m_state.showStatus) {
            if (ImGui::Begin("상태", &m_state.showStatus))
                ImGui::TextUnformatted(m_state.statusMessage.c_str());
            ImGui::End();
        }

        // ---- 렌더 ----
        ImGui::Render();
        // 버튼·탭·제목에 이미지 입히기: 다 그린 뒤 색을 열쇠로 찾아 UV를 갈아 끼운다
        skinner.collectKeys(m_state.theme, m_state.windowStyles);
        skinner.apply(ImGui::GetDrawData());
        const float clear[4] = {0.12f, 0.12f, 0.14f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0); // vsync
    }

    // ---- 정리 ----
    if (m_state.player) m_state.player->stop();
    // 지금 쓰던 MIDI 장치를 이름으로 남긴다 (다음 실행에서 그대로 열어 준다).
    // 이름으로 저장하는 이유는 Settings.h 참고 (번호는 장치를 꽂으면 밀린다).
    {
        AppSettings out;
        out.softThru = m_state.softThru;
        out.startScreenOnLaunch = m_state.startScreenOnLaunch;
        out.controlPipe = m_state.controlPipeOn;
        if (m_state.input) {
            const auto ports = m_state.input->listPorts();
            out.midiInAutoOpen = m_state.input->isOpen();
            if (m_state.selectedInputPort >= 0 &&
                m_state.selectedInputPort < (int)ports.size())
                out.midiInPort = ports[(std::size_t)m_state.selectedInputPort];
        }
        if (m_state.output) {
            const auto ports = m_state.output->listPorts();
            out.midiOutAutoOpen = m_state.output->isOpen();
            if (m_state.selectedOutputPort >= 0 &&
                m_state.selectedOutputPort < (int)ports.size())
                out.midiOutPort = ports[(std::size_t)m_state.selectedOutputPort];
        }
        saveSettings(out, autosaveDir() / L"settings.ini");
    }
    m_control.stop(); // 파이프 스레드 정리 (UI가 멈추기 전에)
    {
        // 정상 종료: 세션 락을 지워 다음 실행에서 복구를 묻지 않게 한다
        std::error_code ec;
        std::filesystem::remove(autosaveDir() / L"session.lock", ec);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace midipro::gui
