#pragma once
// =============================================================
// MidiPro - guitar/Fretboard.h
// 기타 지판 계산 (연주 보조 기능의 핵심 로직).
//
// 왜 GUI와 분리했는가 (Rule 1, 6):
//   "이 MIDI 노트가 지판 어디에 있나", "이 코드는 어떤 프렛을
//   잡나" 같은 계산은 순수 함수다. GUI(피아노 롤/지판 뷰)는
//   이 결과를 그리기만 한다. 덕분에 지판 로직을 단독 테스트할
//   수 있다.
// =============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace midipro::guitar {

// 표준 튜닝 6줄의 개방현 MIDI 노트 (6번줄=인덱스0, 1번줄=인덱스5)
inline constexpr int kStringCount = 6;
inline constexpr uint8_t kStandardTuning[kStringCount] = {40, 45, 50, 55, 59, 64};
inline constexpr int kDefaultFretCount = 22;

// 특정 줄/프렛의 MIDI 노트
uint8_t noteAt(int stringIndex, int fret,
               const uint8_t (&tuning)[kStringCount] = kStandardTuning);

// 지판 위의 한 위치
struct FretPosition {
    int stringIndex; // 0=6번줄 ... 5=1번줄
    int fret;        // 0=개방현
};

// 주어진 MIDI 노트를 낼 수 있는 모든 (줄, 프렛) 위치.
// 왜 여러 개인가: 같은 음을 여러 줄에서 잡을 수 있어 피아노 롤의
// 음을 지판에 겹쳐 보여줄 때 후보를 모두 제시한다.
std::vector<FretPosition> positionsForNote(uint8_t note, int maxFret = kDefaultFretCount,
                                           const uint8_t (&tuning)[kStringCount] = kStandardTuning);

// 코드/스케일 표시에 쓰는 음정 집합 (루트로부터의 반음 간격)
struct ChordShape {
    std::string name;
    std::vector<int> intervals; // 예: 메이저 = {0, 4, 7}
};

// 자주 쓰는 코드 공식 목록 (메이저/마이너/7th 등)
const std::vector<ChordShape>& commonChords();

// 루트 노트 이름 12개 (C, C#, ... B)
const char* pitchClassName(int pitchClass);

} // namespace midipro::guitar
