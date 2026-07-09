#pragma once
// =============================================================
// MidiPro - core/UndoHistory.h
// 스냅샷 기반 실행취소/다시실행 히스토리 (순수 로직, 헤더 전용).
//
// 왜 스냅샷 방식인가:
//   이 앱의 편집 대상(Song)은 수 KB 수준이라, 편집마다 명령 객체를
//   설계하는 것보다 상태 전체를 복사해 쌓는 편이 단순하고 버그가
//   적다. record()로 "편집 직전 상태"를 남기고, undo/redo가 현재
//   상태와 스택을 맞바꾼다.
//
// 템플릿이라 헤더에 둔다 (Rule 4의 .h/.cpp 분리 예외). T는 값 복사가
// 가능한 타입이면 되고, 장치/GUI와 무관해 단독 테스트된다 (Rule 6).
// =============================================================

#include <cstddef>
#include <utility>
#include <vector>

namespace midipro::core {

template <typename T>
class UndoHistory {
public:
    explicit UndoHistory(std::size_t maxDepth = 100) : m_maxDepth(maxDepth) {}

    // 편집을 적용하기 "직전"에 현재 상태를 복원 지점으로 남긴다.
    // 새 편집이 생기면 redo 스택은 무효가 되므로 비운다.
    void record(const T& current) {
        m_undo.push_back(current);
        // 상한을 넘으면 가장 오래된 것부터 버린다 (메모리 무한 증가 방지)
        if (m_undo.size() > m_maxDepth) m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    // 현재 상태를 redo로 넘기고, 직전 상태를 current로 복원한다.
    bool undo(T& current) {
        if (m_undo.empty()) return false;
        m_redo.push_back(current);
        current = std::move(m_undo.back());
        m_undo.pop_back();
        return true;
    }

    // undo의 역방향.
    bool redo(T& current) {
        if (m_redo.empty()) return false;
        m_undo.push_back(current);
        current = std::move(m_redo.back());
        m_redo.pop_back();
        return true;
    }

    void clear() {
        m_undo.clear();
        m_redo.clear();
    }

    std::size_t undoCount() const { return m_undo.size(); }
    std::size_t redoCount() const { return m_redo.size(); }

private:
    std::vector<T> m_undo;
    std::vector<T> m_redo;
    std::size_t m_maxDepth;
};

} // namespace midipro::core
