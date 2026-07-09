#pragma once
// =============================================================
// MidiPro - core/SpscQueue.h
// 단일 생산자/단일 소비자(SPSC) 락프리 큐.
//
// 왜 락프리인가 (Rule 3):
//   MIDI 입력 콜백은 드라이버의 실시간성 스레드에서 호출된다.
//   여기서 뮤텍스를 잡거나 메모리를 할당하면 콜백이 블로킹되어
//   메시지 유실/지연이 생길 수 있으므로, 미리 할당된 고정 크기
//   버퍼 + 아토믹 인덱스만으로 데이터를 넘긴다.
//
// 제약:
//   - push()는 생산자 스레드 하나에서만, pop()은 소비자 스레드
//     하나에서만 호출해야 한다. (SPSC 전제)
//   - Capacity는 2의 거듭제곱이어야 한다. (마스킹으로 모듈로 대체)
//   - T는 값 복사가 저렴하고 동적 할당이 없는 타입이어야 한다.
//
// 템플릿이므로 헤더에 구현을 둔다. (Rule 4의 .h/.cpp 분리 예외)
// =============================================================

#include <array>
#include <atomic>
#include <cstddef>

namespace midipro::core {

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity는 2의 거듭제곱이어야 합니다");

public:
    // 생산자 전용. 큐가 가득 차면 false를 반환하고 버린다.
    // 왜 버리는가: 실시간 스레드에서 대기(spin/block)는 금지이므로
    // 넘치면 드롭하고, 드롭 수는 호출 측에서 집계한다.
    bool push(const T& value) {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & (Capacity - 1);
        // acquire: 소비자가 pop에서 올린 head 갱신 이후의 상태를 봐야 함
        if (next == m_head.load(std::memory_order_acquire)) {
            return false; // 가득 참
        }
        m_buffer[tail] = value;
        // release: buffer 쓰기가 tail 갱신보다 먼저 보이도록 보장
        m_tail.store(next, std::memory_order_release);
        return true;
    }

    // 소비자 전용. 비어 있으면 false.
    bool pop(T& out) {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        // acquire: 생산자의 buffer 쓰기가 tail 갱신 전에 완료됐음을 보장
        if (head == m_tail.load(std::memory_order_acquire)) {
            return false; // 비어 있음
        }
        out = m_buffer[head];
        // release: buffer 읽기가 head 갱신보다 먼저 완료되도록 보장
        m_head.store((head + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> m_buffer{}; // 사전 할당(pre-allocation), 재사용만 한다
    std::atomic<std::size_t> m_head{0}; // 소비자가 읽는 위치
    std::atomic<std::size_t> m_tail{0}; // 생산자가 쓰는 위치
};

} // namespace midipro::core
