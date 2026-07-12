#pragma once
// =============================================================
// MidiPro - gui/PanelsInternal.h
// Panels*.cpp 파일들이 공유하는 "내부" 위젯 헬퍼 선언.
// (App 등 바깥 계층은 Panels.h만 쓴다 — 이 헤더는 gui 내부 전용)
//
// 왜 분리했는가: Panels.cpp가 수천 줄로 커져 패널별 파일로 나누는 중이다.
// 여러 패널이 같이 쓰는 위젯(미터/노브 등)은 여기 선언하고
// PanelsMixer.cpp에 정의한다.
// =============================================================

#include "gui/AppState.h"

#include "imgui.h"

namespace midipro::audio { class BuiltinFx; } // 내장 이펙트 (audio/BuiltinFx.h)

namespace midipro::gui {

// 현재 칸(사용 가능 폭) 안에서 폭 w짜리 위젯이 가운데 오도록 커서를 옮긴다.
void centerNextItem(float w);
// 가운데 정렬 텍스트 (칸 폭 기준)
void centeredText(const char* text);

// 원형 노브: 세로 드래그로 vmin~vmax 조절, 더블클릭 = vdefault 리셋.
bool rotaryKnob(const char* id, const char* label, float* value, float vmin, float vmax,
                float vdefault, float radius);
// 노브/체크박스 편집 시작 시 undo 지점 (이전 값으로 되돌려 스냅샷)
void snapshotKnobEdit(AppState& state, float& live, float before);

// 세로 레벨 미터 (클립 램프 + L/R 또는 모노 바). 믹서/트랙 뷰 헤더 공용.
void levelMeterWidget(const char* id, AppState::MeterView* views, const float* raws, int bars,
                      float w, float h);
// 가로 미니 레벨 미터 (트랙 목록의 좁은 행용)
void miniMeterH(const char* id, AppState::MeterView& m, float raw, float w, float h);

// ---- 트랜스포트/공용 동작 헬퍼 (Panels.cpp에 정의) ----
// 노트 번호 → "C4" 같은 표기
std::string noteName(uint8_t note);
// 곡 틱 → 오디오 프레임 (템포 맵 반영)
int64_t tickToFrame(AppState& state, uint32_t tick);
// 재생/녹음/오디오 녹음 전부 정지 + 소리 끔
void stopTransport(AppState& state);
// 모든 채널 즉시 무음 (All Sound Off + All Notes Off)
void silenceOutput(AppState& state);
// 현재 위치에서 재생 시작 (오디오 클립 믹스 재구축 포함)
void startPlayback(AppState& state);
// 재생 위치 이동 (scrollView=true면 트랙 뷰/피아노 롤 스크롤 따라감)
void seekTo(AppState& state, uint32_t tick, bool scrollView = true);
// 미리듣기 노트 (durationSec 뒤 자동 노트오프)
void triggerNote(AppState& state, uint8_t channel, uint8_t note, uint8_t velocity,
                 double durationSec);
// 선택 노트/클립 인덱스가 범위를 벗어나지 않게 보정
void clampSelection(AppState& state);
// 카운트인 길이(틱)
uint32_t countInTicks(const AppState& state);

// 박자표(state.metroSigIndex: 0=4/4, 1=3/4, 2=6/8)에 따른 한 마디 / 한 박 틱 수.
// 그리드·마디 번호·루프·복제 등 "마디"를 쓰는 곳은 전부 이걸 거쳐야 3/4·6/8이 맞는다.
uint32_t songTicksPerBar(const AppState& state);
uint32_t songTicksPerBeat(const AppState& state);

// ---- 타임라인/클립 그리기 헬퍼 (Panels.cpp에 정의, 트랙 뷰·피아노 롤 공용) ----
// 재생 헤드 틱 (출력 지연 보정 반영)
uint32_t playheadTick(const AppState& state);
// 실제 내용 끝(틱): MIDI + 오디오 클립 (플레이헤드 제외)
uint32_t contentTicksWithAudio(const AppState& state);
// 클립 끝 틱 (배속·트림·템포 맵 반영)
double clipEndTick(const audio::AudioClip& c, const seq::Song& song);
// 클립의 화면 x 범위 [cx0, cx1]
void clipScreenX(const audio::AudioClip& c, float originX, float zoom, const seq::Song& song,
                 float& cx0, float& cx1);
// 마우스 x가 어느 클립 위인가 (몸통/끝 핸들 ±6px, 없으면 -1)
int clipHitTest(const std::vector<std::shared_ptr<audio::AudioClip>>& clips, float mx,
                float originX, float zoom, const seq::Song& song);
// 오디오 클립 블록/파형
void drawClipBlock(ImDrawList* dl, const audio::AudioClip& clip, float originX, float topY,
                   float h, float zoom, const seq::Song& song, bool sel);
void drawWaveform(ImDrawList* dl, const audio::AudioClip& clip, float originX, float topY, float h,
                  float zoom, const seq::Song& song, ImU32 col);
// 드래그 중 좌우 끝에서 자동 스크롤 (표 안에서 호출)
void trackViewEdgeScroll(float mouseX);
// 템포 변경 지점 세로선 + BPM 라벨
void drawTempoMarkers(ImDrawList* dl, const seq::Song& song, float originX, float zoom, float y0,
                      float y1, bool withLabels, int selectedIdx = -1);

// ---- 트랙/클립 조작 (Panels.cpp에 정의) ----
// 트랙 유형 배지: [드럼]=주황, [기타]=초록. 일반 트랙은 아무것도 안 그린다(false 반환).
// sameLine=true면 배지 뒤에 SameLine을 걸어 이름과 한 줄로 잇는다.
bool trackTypeBadge(const seq::Track& t, bool sameLine = true);

// Shift+클릭: 트랙을 타브 창 표시 목록에 넣고 뺀다 (클릭한 순서 = 보표 순서).
// 넣을 때 타브 창도 연다.
void toggleTabTrack(AppState& state, int trackIndex);

void addTrack(AppState& state);
void addDrumTrack(AppState& state); // 채널 10 드럼 트랙 + 에디터 열기
void addGuitarTrack(AppState& state); // 기타 트랙 + 타브 악보 창 열기
// 드럼 노트에 WAV 샘플 배정 (빈 경로 = 내장 신스로 복귀). PanelsDrums.cpp에 정의.
bool assignDrumSample(AppState& state, int note, const std::string& utf8Path);
void deleteTrack(AppState& state, int index);
void moveTrackTo(AppState& state, int from, int to);
void freezeTrack(AppState& state, int trackIndex);
void unfreezeTrack(AppState& state, int trackIndex);
bool startAudioRecording(AppState& state, int trackIndex);
void stopAudioRecording(AppState& state, bool alsoStopTransport = true);
void splitTrackClip(AppState& state, int trackIndex, int clipIndex, uint32_t atTick);
void deleteTrackClip(AppState& state, int trackIndex, int clipIndex);
void mergeTrackClips(AppState& state, int trackIndex, std::vector<int> indices);
void copySelectedClip(AppState& state);
void pasteClipAt(AppState& state, int trackIndex, uint32_t tick);
std::shared_ptr<audio::AudioClip> rangeSelClip(AppState& state); // 구간 선택 대상 클립
void copyClipRange(AppState& state);
void deleteClipRange(AppState& state, bool closeGap = false);
void duplicateSelectedClips(AppState& state);
// 현재 오디오 클립 배치를 엔진에 다시 깐다 (재생 위치 기준)
void rebuildAudioMix(AppState& state);
// 재생 중 곡 편집을 재생 스냅샷에 반영
void refreshPlaybackIfPlaying(AppState& state);

// ---- 내장 이펙트 (PanelsMixer.cpp에 정의) ----
// 체인 인덱스 fxIndex에 해당하는 트랙의 저장 항목(plugins)을 찾는다 (없으면 nullptr).
// 채널을 공유하는 트랙이 있으면 로드 때와 같은 순서(트랙 순서대로)로 센다.
seq::TrackPlugin* trackFxPlugin(AppState& state, int channel, int fxIndex);
// 내장 이펙트의 저장용 경로 문자열 "builtin:eq|p0,p1,p2,p3"
std::string builtinFxPathString(const audio::BuiltinFx& fx);
// 트랙에 기본 EQ 장착 (이미 있으면 무시) — 새 트랙이 생길 때 불린다
void addTrackEq(AppState& state, seq::Track& t);
// 기본 EQ 저/중/고 미니 페이더 (채널 창·믹서 스트립 공용, 없으면 + EQ 버튼)
void drawTrackEqInline(AppState& state, seq::Track& t);
// 마스터 리미터 토글 + GR 표시 + 설정 팝업 (믹서/채널 창 마스터 공용)
void drawMasterLimiterControls(AppState& state);
// 리턴 리버브 (센드/리턴 버스) 레벨 + 설정 (믹서 마스터 스트립)
void drawReturnReverbControls(AppState& state);

// 노트 추출 캐시: 트랙이 안 바뀌면 extractNotes 결과를 재사용 (그리기 전용).
// 반환 참조는 이번 프레임 안에서만 쓰고, 트랙을 수정한 뒤에는 다시 받아야 한다.
const std::vector<seq::NoteSpan>& cachedNotes(const seq::Track& track, int trackIndex);

// ---- 노트 선택/편집 (Panels.cpp에 정의, 피아노 롤·트랙 뷰 공용) ----
std::vector<seq::NoteSpan> gatherSelected(const AppState& state, const seq::Track& track);
void deleteSelectedNotes(AppState& state, seq::Track& track);
void copySelectedNotes(AppState& state, const seq::Track& track);
void pasteNotes(AppState& state, seq::Track& track);
void quantizeNotes(AppState& state, seq::Track& track, uint32_t gridTicks);
// 스윙: 엇박(짝수 격자 칸)을 pct%만큼 뒤로 민다 (100 = 셋잇단). 선택 우선.
void applySwing(AppState& state, seq::Track& track, uint32_t grid, int pct);
// 구간 복제 (어레인지): [start, end)를 바로 뒤에 끼워 넣고 뒷내용을 민다
void duplicateSection(AppState& state, uint32_t startTick, uint32_t endTick);
// (MIDI 구간 조작 shiftMidiRange/copyMidiRange/eraseMidiRange는 순수 로직이라
//  sequencer/Track.h에 있다 — MIDI 클립 이동/복제/삭제가 쓴다)
void moveSelectedNotes(AppState& state, seq::Track& track, int dTick, int dNote);
void duplicateSelectedNotes(AppState& state);

} // namespace midipro::gui
