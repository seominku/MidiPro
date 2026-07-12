#pragma once
// =============================================================
// MidiPro - pdf/PdfTab.h
// 조판(디지털) PDF 악보에서 6줄 타브 보표를 찾아 노트로 복원.
//
// 동작 원리:
//  - PDF 벡터 선 중 "등간격 가로줄 6개" 그룹 = 타브 보표 (5개 = 오선보)
//  - 프렛 숫자는 글자 좌표로 추출해 가장 가까운 줄(현)에 배정
//  - 리듬: 타브 아래에 그려진 기둥(stem)·빔(beam)·부점을 읽는다.
//      빔 0개 = 4분, 1개 = 8분, 2개 = 16분, 3개 = 32분, 부점이 붙으면 1.5배.
//      기둥만 있고 프렛 숫자가 없는 자리 = 슬래시(/) 스트로크 -> 직전 코드를 다시 친다.
//      마디선마다 시각을 마디 경계로 다시 맞춰서 오차가 누적되지 않게 한다.
//  - 기둥이 없는 악보(리듬 표기가 없는 타브)면 hasRhythm=false가 되고,
//    이때는 ascii만 쓰고 리듬은 사용자가 고른 칸 길이로 정한다.
//
// 지원: FlateDecode, RC4 암호화(빈 비밀번호, V1/V2·R2/R3), Type0(ToUnicode)·단순 폰트, ObjStm.
// 미지원: 스캔(이미지) PDF, AES 암호화 — error에 사유를 담아 반환.
// =============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace midipro::pdf {

struct TabPdfNote {
    uint32_t tick = 0;     // 파트 시작 기준
    uint8_t note = 0;      // MIDI 노트 번호
    uint32_t durTicks = 0; // 리듬상 길이 (실제 소리 길이 = durTicks * artic / 100)
    int8_t strIdx = -1;    // 악보의 줄 (0=e ~ 5=E) — 타브 창이 원래 운지로 표시하게
    uint8_t vel = 100;     // 셈여림: 박 위치 강약 + 뮤트/데드/레가토 반영
    uint8_t artic = 94;    // 아티큘레이션 %: 뮤트=50, 데드=30, 레가토=100(다음 음까지)
};

struct TabPdfPart {
    std::string ascii;               // 미리보기용 6줄 타브 텍스트
    std::vector<TabPdfNote> notes;   // 리듬까지 읽어낸 노트 (hasRhythm일 때만 채워진다)
};

struct TabPdfResult {
    // 한 단에 타브 보표가 2개씩 있으면(기타 1·2) parts도 2개가 된다.
    std::vector<TabPdfPart> parts;
    int tabStaves = 0;      // 발견한 6줄 보표 수
    int pages = 0;
    bool hasRhythm = false; // 기둥/빔을 읽어 실제 리듬을 복원했는가
    int timeSigNum = 0;     // 곡 첫머리의 박자표 (0 = 못 읽음 -> 4/4로 처리)
    int timeSigDen = 0;
    int repeats = 0;        // 펼친 도돌이표(반복 구간) 수
    int measureRepeats = 0; // 채워 넣은 마디 반복 기호(𝄎) 수
    int tempoBpm = 0;       // 악보의 템포 표기 (♩ = 164). 0 = 못 읽음
    std::string error;      // 비어 있으면 성공
};

// ppqn: 4분음표 한 개의 틱 수 (곡 설정값). 리듬 복원에 쓴다.
TabPdfResult extractTabFromPdf(const std::string& utf8Path, uint32_t ppqn);

} // namespace midipro::pdf
