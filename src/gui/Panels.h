#pragma once
// =============================================================
// MidiPro - gui/Panels.h
// 각 UI 패널 렌더링 함수. AppState를 받아 ImGui 창을 그린다.
//
// 함수마다 하나의 패널만 책임진다 (Rule 1의 SRP).
// =============================================================

#include "gui/AppState.h"

namespace midipro::gui {

void pumpMonitor(AppState& state);         // 입력 큐를 poll해 로그에 쌓는다 (프레임마다)
void updatePendingNotes(AppState& state);  // 예약된 Note Off를 시간이 되면 보낸다 (프레임마다)
void applyTransportState(AppState& state); // 루프/메트로놈을 플레이어에 반영 (창 표시와 무관)

void drawTransport(AppState& state);  // 재생/정지/BPM/위치
void drawDevices(AppState& state);    // MIDI 입출력 포트 선택
void drawTrackList(AppState& state);  // 트랙 추가/뮤트/선택
void drawPianoRoll(AppState& state);  // 노트 표시 + 재생 헤드
void drawSynth(AppState& state);      // 내장 신스 음색 파라미터
void drawVst(AppState& state);        // VST3 악기/이펙트 호스팅
void drawGuitarHelper(AppState& state); // 튜닝/코드/지판
void drawMonitor(AppState& state);    // 실시간 입력 로그
void drawMenuBar(AppState& state, bool& openRequested, bool& saveRequested);

} // namespace midipro::gui
