#pragma once
// =============================================================
// MidiPro - core/CrashLog.h
// "오류창도 없이 그냥 꺼지는" 상황을 기록으로 남긴다.
//
// 왜 필요한가:
//   VST 플러그인은 우리 프로세스 안에서 실행된다. 플러그인이 잘못된 메모리를
//   건드리면 우리 코드가 아무 잘못을 안 해도 프로세스가 통째로 죽고, 기본
//   설정에서는 창 하나 없이 사라진다. 그러면 어디서 죽었는지 알 길이 없다.
//   그래서 마지막 순간에 로그와 미니덤프를 남겨 둔다.
//
// 남는 곳: %LOCALAPPDATA%\MidiPro\crash.log  (덤프는 crash.dmp)
// =============================================================

#include <string>

namespace midipro::core {

// 프로그램 시작 직후 한 번 호출한다.
void installCrashHandler();

// "지금 무엇을 하던 중인가"를 남긴다 (죽었을 때 로그에 함께 찍힌다).
// 예: setCrashContext("VST 로드: C:\\...\\Omnisphere.vst3");
void setCrashContext(const std::string& what);
void clearCrashContext();

// 위험한 구간을 표시하는 도우미 (스코프를 벗어나면 자동으로 지운다)
struct CrashContext {
    explicit CrashContext(const std::string& what) { setCrashContext(what); }
    ~CrashContext() { clearCrashContext(); }
    CrashContext(const CrashContext&) = delete;
    CrashContext& operator=(const CrashContext&) = delete;
};

// 크래시가 아니어도 진단이 필요할 때 한 줄 남긴다 (같은 파일에 누적).
void logLine(const std::string& text);

} // namespace midipro::core
