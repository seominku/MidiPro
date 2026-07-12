// =============================================================
// MidiPro - gui/App.cpp
// Win32 + D3D11 + ImGui 부트스트랩. 구조는 ImGui 공식 예제
// (example_win32_directx11)를 따르되 MidiPro 상태/패널을 연결했다.
// =============================================================

#include "core/PathUtf8.h"
#include "gui/App.h"
#include "gui/PanelsInternal.h" // addTrackEq (새 트랙 기본 EQ)
#include "gui/Panels.h"

#include "audio/AudioClip.h"
#include "audio/BuiltinFx.h"
#include "audio/Mp3Writer.h"
#include "audio/SynthPreset.h"
#include "audio/WavFile.h"
#include "project/Project.h"
#include "sequencer/SmfFile.h"

#include <fstream>

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
    return true;
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
    if (!loadTheme(m_state.theme, autosaveDir() / L"theme.ini"))
        applyThemeParams(m_state.theme);
    loadRecentList(); // 최근 프로젝트 목록 복원

    // 한글 폰트: Windows 기본 맑은 고딕 + 한국어 글리프 범위
    ImFontConfig fontCfg;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f, &fontCfg,
                                 io.Fonts->GetGlyphRangesKorean());

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

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
                m_state.statusMessage =
                    loadProjectFrom(autosaveFile)
                        ? "자동 저장본 복구됨 — '프로젝트 저장'으로 원하는 위치에 저장하세요"
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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 전체 화면 도킹 공간 (1.92: 첫 인자는 dockspace id)
        const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // 첫 실행 시 화면을 꽉 채우는 기본 레이아웃을 구성한다.
        // 트랜스포트(위) · 트랙/장치(왼쪽) · 피아노 롤(가운데) · 도구(오른쪽) · 모니터(아래).
        static bool dockLayoutBuilt = false;
        if (!dockLayoutBuilt) {
            dockLayoutBuilt = true;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);

            ImGuiID center = dockId;
            const ImGuiID top = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.16f, nullptr, &center);
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
            ImGui::DockBuilderDockWindow("기타 도우미", right);
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
        drawTransport(m_state);
        drawDevices(m_state);
        drawTrackList(m_state);
        drawTrackView(m_state);
        drawMixer(m_state);
        drawMixerCompact(m_state);
        drawPerf(m_state);
        drawPianoRoll(m_state);
        drawDrums(m_state);
        drawArrange(m_state);
        drawGuitarTab(m_state);
        drawSynth(m_state);
        drawPreferences(m_state);
        drawExportDialog(m_state);
        drawBuiltinFx(m_state);
        drawVst(m_state);
        drawGuitarHelper(m_state);
        drawMonitor(m_state);
        if (scrollReqAtFrameStart) m_state.scrollToPlayhead = false; // 모든 뷰가 반영한 뒤 해제

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
            saveTheme(m_state.theme, autosaveDir() / L"theme.ini");
            m_state.themeDirty = false;
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
        const float clear[4] = {0.12f, 0.12f, 0.14f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0); // vsync
    }

    // ---- 정리 ----
    if (m_state.player) m_state.player->stop();
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
