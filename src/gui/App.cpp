// =============================================================
// MidiPro - gui/App.cpp
// Win32 + D3D11 + ImGui 부트스트랩. 구조는 ImGui 공식 예제
// (example_win32_directx11)를 따르되 MidiPro 상태/패널을 연결했다.
// =============================================================

#include "gui/App.h"
#include "gui/Panels.h"

#include "audio/SynthPreset.h"
#include "project/Project.h"
#include "sequencer/SmfFile.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <windows.h>
#include <commdlg.h>

#include <tchar.h>
#include <utility>

// ImGui의 Win32 메시지 핸들러 전방 선언
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace midipro::gui {

namespace {

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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Win32 파일 대화상자. 성공 시 경로를 반환한다.
std::string fileDialog(HWND owner, bool save, const wchar_t* filter = L"MIDI 파일 (*.mid)\0*.mid\0모든 파일\0*.*\0",
                       const wchar_t* defExt = L"mid") {
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
    if (!ok) return {};

    // UTF-16 경로 -> UTF-8
    char utf8[MAX_PATH * 4] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return std::string(utf8);
}

} // namespace

App::App(midi::IMidiInput& input, midi::MidiOutputRouter& output, audio::ISynthControl& synth,
         audio::IVstHostControl& vst, audio::IMidi2Input& midi2) {
    m_state.input = &input;
    m_state.output = &output;
    m_state.synth = &synth;
    m_state.vst = &vst;
    m_state.midi2 = &midi2;
    // Player는 라우터(IMidiOutput)에만 의존한다 — 실제 출력 대상은 라우터가 결정.
    m_state.player = std::make_unique<seq::Player>(output);
    // 초기 음색을 신스에 한 번 전달 (스트림이 열리면 콜백이 적용).
    m_state.synth->setParams(m_state.synthParams);
}

App::~App() = default;

int App::run() {
    // ---- 창 등록/생성 ----
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandle(nullptr),
                      nullptr,    nullptr,    nullptr, nullptr, L"MidiProWnd", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MidiPro - Phase 5", WS_OVERLAPPEDWINDOW, 100, 100,
                              1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // ---- ImGui 초기화 ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // 한글 폰트: Windows 기본 맑은 고딕 + 한국어 글리프 범위
    ImFontConfig fontCfg;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f, &fontCfg,
                                 io.Fonts->GetGlyphRangesKorean());

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 전체 화면 도킹 공간 (1.92: 첫 인자는 dockspace id)
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // 입력 큐 펌프 (Rule 3: 로깅은 GUI 스레드에서)
        pumpMonitor(m_state);
        // 버튼으로 친 음의 자동 Note Off 처리
        updatePendingNotes(m_state);
        // 루프/메트로놈을 플레이어에 반영 (트랜스포트 창을 닫아도 유지)
        applyTransportState(m_state);

        bool openRequested = false;
        bool saveRequested = false;
        drawMenuBar(m_state, openRequested, saveRequested);
        drawTransport(m_state);
        drawDevices(m_state);
        drawTrackList(m_state);
        drawPianoRoll(m_state);
        drawSynth(m_state);
        drawVst(m_state);
        drawGuitarHelper(m_state);
        drawMonitor(m_state);

        if (openRequested) {
            const std::string path = fileDialog(hwnd, /*save=*/false);
            if (!path.empty()) {
                if (m_state.player) m_state.player->stop();
                seq::Song loaded;
                if (seq::smf::load(loaded, path)) {
                    m_state.snapshot(); // 불러오기도 되돌릴 수 있게 이전 곡 보존
                    m_state.song = std::move(loaded);
                    m_state.selectedTrack = 0;
                    m_state.statusMessage = "불러오기 완료: " + path;
                } else {
                    m_state.statusMessage = "불러오기 실패 (지원하지 않는 형식)";
                }
            }
        }
        if (saveRequested) {
            const std::string path = fileDialog(hwnd, /*save=*/true);
            if (!path.empty()) {
                m_state.statusMessage =
                    seq::smf::save(m_state.song, path) ? "저장 완료: " + path : "저장 실패";
            }
        }

        // 신스 프리셋 저장/불러오기 (.synth 텍스트 파일)
        static const wchar_t* kPresetFilter = L"신스 프리셋 (*.synth)\0*.synth\0모든 파일\0*.*\0";
        if (m_state.presetSaveRequested) {
            m_state.presetSaveRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/true, kPresetFilter, L"synth");
            if (!path.empty())
                m_state.statusMessage = audio::savePreset(m_state.synthParams, path)
                                            ? "프리셋 저장 완료: " + path
                                            : "프리셋 저장 실패";
        }
        if (m_state.presetLoadRequested) {
            m_state.presetLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kPresetFilter, L"synth");
            if (!path.empty()) {
                if (audio::loadPreset(m_state.synthParams, path)) {
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
                m_state.statusMessage = m_state.midiMap.save(path) ? "매핑 저장 완료: " + path
                                                                   : "매핑 저장 실패";
        }
        if (m_state.mapLoadRequested) {
            m_state.mapLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kMapFilter, L"map");
            if (!path.empty())
                m_state.statusMessage = m_state.midiMap.load(path) ? "매핑 불러오기 완료: " + path
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
                project::ProjectData pd;
                pd.song = m_state.song;
                pd.synth = m_state.synthParams;
                pd.midiMap = m_state.midiMap;
                pd.mpe = m_state.synth && m_state.synth->mpeMode();
                pd.vstInstrumentPath = m_state.vstInstrumentPath;
                pd.vstInstrumentClass = m_state.vstInstrumentClass;
                pd.vstEffectPath = m_state.vstEffectPath;
                pd.vstEffectClass = m_state.vstEffectClass;
                m_state.statusMessage =
                    project::save(pd, path) ? "프로젝트 저장 완료: " + path : "프로젝트 저장 실패";
            }
        }
        if (m_state.projectLoadRequested) {
            m_state.projectLoadRequested = false;
            const std::string path = fileDialog(hwnd, /*save=*/false, kProjFilter, L"midipro");
            if (!path.empty()) {
                project::ProjectData pd;
                if (project::load(pd, path)) {
                    if (m_state.player) m_state.player->stop();
                    m_state.snapshot();
                    m_state.song = std::move(pd.song);
                    m_state.selectedTrack = 0;
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
                            m_state.vst->loadInstrument(pd.vstInstrumentPath, pd.vstInstrumentClass,
                                                        err)) {
                            m_state.vstInstrumentPath = pd.vstInstrumentPath;
                            m_state.vstInstrumentClass = pd.vstInstrumentClass;
                        }
                        if (!pd.vstEffectPath.empty() &&
                            m_state.vst->loadEffect(pd.vstEffectPath, pd.vstEffectClass, err)) {
                            m_state.vstEffectPath = pd.vstEffectPath;
                            m_state.vstEffectClass = pd.vstEffectClass;
                        }
                    }
                    m_state.statusMessage = "프로젝트 불러오기 완료: " + path;
                } else {
                    m_state.statusMessage = "프로젝트 불러오기 실패";
                }
            }
        }

        // 상태 표시줄
        if (m_state.showStatus) {
            if (ImGui::Begin("상태", &m_state.showStatus))
                ImGui::TextUnformatted(m_state.statusMessage.c_str());
            ImGui::End();
        }

        // ---- 렌더 ----
        ImGui::Render();
        const float clear[4] = {0.12f, 0.12f, 0.14f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0); // vsync
    }

    // ---- 정리 ----
    if (m_state.player) m_state.player->stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace midipro::gui
