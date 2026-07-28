// =============================================================
// MidiPro - gui/ControlServer.cpp
// =============================================================

#include "gui/ControlServer.h"

#include <windows.h>

#include <chrono>

namespace midipro::gui {
namespace {

constexpr const wchar_t* kPipe = L"\\\\.\\pipe\\MidiPro.Control";
constexpr DWORD kBufSize = 64 * 1024;
// UI가 모달 대화상자(파일 열기 등)에 막혀 있을 수 있다. 무한정 기다리면
// 호출한 쪽이 멈춰 버리니 끊고 이유를 알려준다.
constexpr auto kUiTimeout = std::chrono::seconds(5);

} // namespace

const wchar_t* ControlServer::pipePath() { return kPipe; }

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { // 제어문자는 \u00XX
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                o += buf;
            } else {
                o += (char)c;
            }
        }
    }
    return o;
}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(Handler handler) {
    if (m_run.load()) return true;
    m_handler = std::move(handler);
    m_run.store(true);
    m_thread = std::thread([this] { threadMain(); });
    return true;
}

void ControlServer::stop() {
    if (!m_run.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    // ConnectNamedPipe에서 대기 중인 스레드를 깨우려고 우리 파이프에 한 번 붙는다.
    const HANDLE h = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    {
        std::lock_guard<std::mutex> lk(m_mtx); // 응답을 기다리던 쪽을 깨운다
        m_doneId = m_reqId;
    }
    m_cvDone.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

// UI 스레드: 대기 중인 요청 하나를 실행한다.
void ControlServer::poll() {
    std::string req;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_hasRequest) return;
        req = m_request;
        id = m_reqId;
        m_hasRequest = false;
    }
    std::string resp;
    try {
        resp = m_handler ? m_handler(req) : std::string("{\"ok\":false,\"error\":\"핸들러 없음\"}");
    } catch (const std::exception& e) {
        resp = std::string("{\"ok\":false,\"error\":\"") + jsonEscape(e.what()) + "\"}";
    } catch (...) {
        resp = "{\"ok\":false,\"error\":\"알 수 없는 오류\"}";
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // 이 사이 새 요청이 들어왔다면(= 앞 요청이 시간 초과로 버려졌다면) 결과를 버린다
        if (id != m_reqId) return;
        m_response = std::move(resp);
        m_doneId = id;
    }
    m_cvDone.notify_all();
}

void ControlServer::threadMain() {
    std::string buf;
    while (m_run.load()) {
        // FILE_FLAG_FIRST_PIPE_INSTANCE: 두 번째 인스턴스는 제어 통로를 갖지 않는다
        // (앱은 그대로 뜬다 — 통로만 첫 인스턴스가 갖는다).
        const HANDLE pipe = CreateNamedPipeW(
            kPipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kBufSize, kBufSize, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            m_run.store(false); // 이미 다른 인스턴스가 쓰는 중 = 조용히 포기
            return;
        }
        const BOOL connected =
            ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || !m_run.load()) {
            CloseHandle(pipe);
            continue;
        }

        buf.clear();
        while (m_run.load()) {
            char chunk[4096];
            DWORD got = 0;
            if (!ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
            buf.append(chunk, got);

            for (;;) {
                const std::size_t nl = buf.find('\n');
                if (nl == std::string::npos) break;
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                // UI 스레드에 넘기고 결과를 기다린다
                std::string resp;
                {
                    std::unique_lock<std::mutex> lk(m_mtx);
                    const uint64_t id = ++m_reqId;
                    m_request = line;
                    m_hasRequest = true;
                    if (!m_cvDone.wait_for(lk, kUiTimeout,
                                           [this, id] { return m_doneId == id; })) {
                        m_hasRequest = false; // 아직 안 집어갔으면 취소
                        resp = "{\"ok\":false,\"error\":\"앱이 응답하지 않습니다 "
                               "(대화상자가 열려 있거나 바쁜 상태일 수 있습니다)\"}";
                    } else {
                        resp = m_response;
                    }
                }
                if (!m_run.load()) break;
                resp += '\n';
                DWORD wrote = 0;
                if (!WriteFile(pipe, resp.data(), (DWORD)resp.size(), &wrote, nullptr)) break;
            }
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace midipro::gui
