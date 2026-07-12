#pragma once
// =============================================================
// MidiPro - sequencer/TabImport.h
// ASCII 기타 타브 텍스트 -> 노트 목록 (순수 로직, 유닛 테스트 대상).
//
// 지원 형식: 널리 쓰이는 6줄 텍스트 타브. 예)
//   e|--0--3--|
//   B|--1-----|
//   G|--0-----|
//   D|--2-----|
//   A|--3-----|
//   E|--------|
// 위 줄부터 1번줄(높은 e)로 본다 (표준 튜닝 EADGBE 고정).
// 리듬 정보가 없는 형식이라, 문자 한 칸 = stepTicks(예: 16분음표)로 매핑한다.
// 여러 블록(단)이 이어지면 시간상 연달아 붙는다.
// =============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace midipro::seq {

struct TabNote {
    uint32_t tick = 0;    // 곡 시작 기준 (첫 칸 = 0)
    uint8_t note = 0;     // MIDI 노트 번호
    uint32_t durTicks = 0;
    int8_t strIdx = -1;   // 악보가 지정한 줄 (0=e ~ 5=E, -1=모름) — 타브 표시 힌트
    uint8_t vel = 100;    // 셈여림 (PDF 가져오기가 채움; 텍스트 타브는 100)
    uint8_t artic = 75;   // 소리 길이 % (텍스트 타브 기본 3/4 — 기존 동작 유지)
};

// text에서 타브 블록들을 찾아 노트로 바꾼다. stepTicks = 문자 한 칸의 길이(틱).
std::vector<TabNote> parseAsciiTab(const std::string& text, uint32_t stepTicks);

} // namespace midipro::seq
