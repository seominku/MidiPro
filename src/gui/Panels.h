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
void drawTrackList(AppState& state);  // 트랙 추가/뮤트/선택 (간단 목록)
void drawTrackView(AppState& state);  // DAW식 트랙 레인(헤더+타임라인)
void drawMixer(AppState& state);        // 믹서: 마스터 + 모든 트랙 스트립
void drawMixerCompact(AppState& state); // 채널 창: 마스터 + 선택 트랙만 (컴팩트)
void drawPianoRoll(AppState& state);  // 노트 표시 + 재생 헤드
void drawDrums(AppState& state);      // 드럼 스텝 시퀀서 (채널 10)
void drawArrange(AppState& state);    // 어레인지 뷰 (구간 블록 재배열)
void drawGuitarTab(AppState& state);  // 타브 악보 (기타 트랙 6줄 표시)
void drawSynth(AppState& state);      // 내장 신스 음색 파라미터
void drawPreferences(AppState& state); // 개인설정(MIDI 장치 + 신디사이저 탭)
void drawVst(AppState& state);        // VST3 악기/이펙트 호스팅
void drawGuitarHelper(AppState& state); // 튜닝/코드/지판
void drawMonitor(AppState& state);    // 실시간 입력 로그
void drawMenuBar(AppState& state, bool& openRequested, bool& saveRequested);
void drawExportDialog(AppState& state); // 내보내기 설정 창 (경로/형식/구간)
void drawPerf(AppState& state);         // 성능 창 (FPS/오디오 부하/버퍼)
void drawBuiltinFx(AppState& state);    // 내장 이펙트(EQ/딜레이/리버브) 파라미터 창
void drawStartScreen(AppState& state);  // 시작 화면 (새 곡·기타 연습·드럼·최근 파일)
void drawBrowser(AppState& state);      // 좌측 브라우저 (악기·이펙트·최근)

// 실제 내용의 끝(틱): MIDI 끝과 오디오 클립 끝 중 긴 쪽 (내보내기 기본 길이)
uint32_t songEndTicks(const AppState& state);

// 지정 틱 시점의 트랙 볼륨/팬(오토메이션 반영)으로 오디오 믹스를 다시 깐다.
// 내보내기가 블록마다 호출해 오토메이션을 렌더에 반영한다.
void rebuildAudioMixAt(AppState& state, uint32_t tick);

} // namespace midipro::gui
