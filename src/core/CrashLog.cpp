// =============================================================
// MidiPro - core/CrashLog.cpp
// =============================================================

#include "core/CrashLog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>

namespace midipro::core {

namespace {

// 크래시 순간에는 무엇도 새로 할당하지 않는 게 안전하다. 그래서 문맥 문자열은
// 고정 버퍼에 담아 둔다 (죽는 시점에 힙이 망가져 있을 수 있다).
constexpr int kCtxMax = 512;
char g_ctx[kCtxMax] = "";
std::mutex g_ctxMutex;

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

std::wstring logDir() {
    wchar_t* base = nullptr;
    std::size_t n = 0;
    if (_wdupenv_s(&base, &n, L"LOCALAPPDATA") != 0 || !base) return L".";
    std::wstring d(base);
    free(base);
    d += L"\\MidiPro";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// 소스가 UTF-8이라 좁은 문자열도 UTF-8이다. fwprintf의 %hs는 이걸 ANSI로
// 보고 변환해 한글이 깨진다 — 명시적으로 넓혀서 %s로 찍는다.
std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "메모리 접근 위반 (ACCESS_VIOLATION)";
    case EXCEPTION_STACK_OVERFLOW: return "스택 넘침 (STACK_OVERFLOW)";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "잘못된 명령 (ILLEGAL_INSTRUCTION)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "0으로 나눔";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "실수 0으로 나눔";
    case EXCEPTION_IN_PAGE_ERROR: return "페이지 오류 (파일/메모리 읽기 실패)";
    case EXCEPTION_PRIV_INSTRUCTION: return "권한 없는 명령";
    case 0xE06D7363: return "처리되지 않은 C++ 예외";
    default: return "알 수 없는 예외";
    }
}

// 주소가 어느 DLL/EXE 안인지 알아낸다 — 범인이 플러그인인지 우리인지 가르는 핵심.
void moduleOfAddress(void* addr, wchar_t* out, DWORD outLen, uintptr_t* offset) {
    out[0] = L'\0';
    if (offset) *offset = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) &&
        mod) {
        GetModuleFileNameW(mod, out, outLen);
        if (offset) *offset = (uintptr_t)addr - (uintptr_t)mod;
    }
}

void writeMiniDump(EXCEPTION_POINTERS* ep) {
    const std::wstring path = logDir() + L"\\crash.dmp";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                      MiniDumpScanMemory),
                      ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

void appendLog(const char* header, EXCEPTION_POINTERS* ep) {
    const std::wstring path = logDir() + L"\\crash.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);

    std::fwprintf(f, L"\n===== %s =====\n", widen(when).c_str());
    std::fwprintf(f, L"무슨 일: %s\n", widen(header).c_str());
    if (g_ctx[0]) std::fwprintf(f, L"하던 작업: %s\n", widen(g_ctx).c_str());

    if (ep && ep->ExceptionRecord) {
        const auto* er = ep->ExceptionRecord;
        std::fwprintf(f, L"예외: 0x%08X %s\n", er->ExceptionCode,
                      widen(exceptionName(er->ExceptionCode)).c_str());
        std::fwprintf(f, L"주소: 0x%p\n", er->ExceptionAddress);
        wchar_t mod[MAX_PATH] = L"";
        uintptr_t off = 0;
        moduleOfAddress(er->ExceptionAddress, mod, MAX_PATH, &off);
        if (mod[0])
            std::fwprintf(f, L"범인 모듈: %s (+0x%llX)\n", mod, (unsigned long long)off);
        else
            std::fwprintf(f, L"범인 모듈: (알 수 없음 — 이미 언로드된 DLL일 수 있음)\n");
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            const wchar_t* how = er->ExceptionInformation[0] == 0   ? L"읽기"
                                 : er->ExceptionInformation[0] == 1 ? L"쓰기"
                                                                    : L"실행";
            std::fwprintf(f, L"접근 종류: %s, 주소 0x%llX\n", how,
                          (unsigned long long)er->ExceptionInformation[1]);
        }
    }
    std::fwprintf(f, L"덤프: crash.dmp (같은 폴더)\n");
    std::fclose(f);
}

LONG WINAPI topLevelFilter(EXCEPTION_POINTERS* ep) {
    // 여기서는 최소한만 한다 — 이미 프로세스 상태가 온전하지 않다.
    appendLog("처리되지 않은 예외로 프로그램이 종료됨", ep);
    writeMiniDump(ep);

    // 사용자가 "그냥 꺼졌다"고 느끼지 않게 알려 준다. 로그 위치도 함께.
    const std::wstring dir = logDir();
    std::wstring msg = L"MidiPro가 예기치 않게 종료되었습니다.\n\n"
                       L"원인을 담은 기록을 남겼습니다:\n" +
                       dir + L"\\crash.log\n" + dir + L"\\crash.dmp\n\n";
    if (g_ctx[0]) {
        msg += L"마지막 작업: ";
        wchar_t wctx[kCtxMax];
        MultiByteToWideChar(CP_UTF8, 0, g_ctx, -1, wctx, kCtxMax);
        msg += wctx;
        msg += L"\n\n";
    }
    msg += L"VST 악기를 불러오는 중이었다면 그 플러그인이 원인일 수 있습니다.";
    // 자동 검증/무인 실행에서는 창을 띄우지 않는다 (기록은 그대로 남는다)
    if (!GetEnvironmentVariableW(L"MIDIPRO_NO_CRASH_DIALOG", nullptr, 0))
        MessageBoxW(nullptr, msg.c_str(), L"MidiPro 오류", MB_OK | MB_ICONERROR);

    if (g_prevFilter) return g_prevFilter(ep);
    return EXCEPTION_EXECUTE_HANDLER; // 조용히 죽는 대신 여기서 끝낸다
}

void onTerminate() {
    appendLog("std::terminate 호출 (처리되지 않은 C++ 예외일 가능성)", nullptr);
    writeMiniDump(nullptr);
    if (!GetEnvironmentVariableW(L"MIDIPRO_NO_CRASH_DIALOG", nullptr, 0))
        MessageBoxW(nullptr,
                    L"MidiPro가 예기치 않게 종료되었습니다.\n"
                    L"%LOCALAPPDATA%\\MidiPro\\crash.log 에 기록을 남겼습니다.",
                    L"MidiPro 오류", MB_OK | MB_ICONERROR);
    _exit(3);
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                        uintptr_t) {
    appendLog("CRT 잘못된 인자 (invalid parameter)", nullptr);
    writeMiniDump(nullptr);
    _exit(3);
}

void onPureCall() {
    appendLog("순수 가상 함수 호출 (pure virtual call)", nullptr);
    writeMiniDump(nullptr);
    _exit(3);
}

} // namespace

void installCrashHandler() {
    g_prevFilter = SetUnhandledExceptionFilter(topLevelFilter);
    std::set_terminate(onTerminate);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    // 일부 CRT는 예외를 자기가 먹고 조용히 죽는다 — 그걸 막는다.
    SetErrorMode(SetErrorMode(0) & ~SEM_NOGPFAULTERRORBOX);
}

void setCrashContext(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    const std::size_t n = what.size() < (kCtxMax - 1) ? what.size() : (kCtxMax - 1);
    std::memcpy(g_ctx, what.data(), n);
    g_ctx[n] = '\0';
}

void clearCrashContext() {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    g_ctx[0] = '\0';
}

void logLine(const std::string& text) {
    const std::wstring path = logDir() + L"\\crash.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::fwprintf(f, L"[%s] %s\n", widen(when).c_str(), widen(text.c_str()).c_str());
    std::fclose(f);
}

} // namespace midipro::core
