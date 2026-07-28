#pragma once
// =============================================================
// MidiPro - gui/ControlServer.h
// 실행 중인 앱을 밖에서 조종하는 로컬 명령 통로 (네임드 파이프).
//
// 무엇에 쓰나:
//   MCP 서버 같은 외부 도구가 앱을 닫지 않고 재생/정지/시크를 시키거나,
//   파일로 고친 프로젝트를 그 자리에서 다시 불러오게 한다.
//
// 왜 파이프인가 (Rule 1):
//   같은 PC 안에서만 쓰는 통로다. TCP는 방화벽 경고와 외부 노출이 따라오고
//   포트도 골라야 한다. 네임드 파이프는 로컬 전용이고 이름이 고정이라 간단하다.
//
// 스레드 규칙 (중요):
//   파이프는 백그라운드 스레드가 받지만, 명령은 UI 스레드가 poll()에서 실행한다.
//   버튼을 누른 것과 완전히 같은 코드 경로를 타므로 새로운 경쟁 조건이 생기지
//   않는다 — 오디오 스레드는 손대지 않는다. 파이프 스레드는 응답이 채워질 때까지
//   기다리되, UI가 모달 대화상자에 막혀 있을 수 있으니 시간 제한을 둔다.
//
// 프로토콜: 줄바꿈으로 구분. 요청은 "명령 인자..." 한 줄, 응답은 JSON 한 줄.
// =============================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace midipro::gui {

class ControlServer {
public:
    // 명령 한 줄을 받아 응답 한 줄(JSON)을 돌려준다. UI 스레드에서 불린다.
    using Handler = std::function<std::string(const std::string&)>;

    ~ControlServer();

    // 파이프 스레드를 띄운다. 실패해도(이미 다른 인스턴스가 쓰는 중 등)
    // 앱은 그대로 동작한다 — 제어 통로만 없는 것이다.
    bool start(Handler handler);
    void stop();

    // UI 스레드에서 프레임마다 호출: 대기 중인 명령이 있으면 실행한다.
    void poll();

    bool running() const { return m_run.load(); }
    static const wchar_t* pipePath(); // \\.\pipe\MidiPro.Control

private:
    void threadMain();

    Handler m_handler;
    std::thread m_thread;
    std::atomic<bool> m_run{false};

    std::mutex m_mtx;
    std::condition_variable m_cvDone; // 응답이 찼을 때 파이프 스레드를 깨운다
    std::string m_request;
    std::string m_response;
    // 요청마다 번호를 매겨 응답을 짝지운다. 시간 초과로 한 번 포기한 뒤
    // 뒤늦게 끝난 결과가 다음 명령의 응답으로 새어 나가면 안 되기 때문이다.
    uint64_t m_reqId = 0;  // 마지막으로 접수된 요청
    uint64_t m_doneId = 0; // 응답이 채워진 요청
    bool m_hasRequest = false;
};

// JSON 문자열 값으로 안전하게 넣기 (경로의 역슬래시·따옴표 처리)
std::string jsonEscape(const std::string& s);

} // namespace midipro::gui
