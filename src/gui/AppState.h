#pragma once
// =============================================================
// MidiPro - gui/AppState.h
// GUI가 다루는 애플리케이션 상태 묶음.
//
// 왜 상태와 렌더링을 분리했는가 (Rule 1):
//   패널 함수(Panels.cpp)는 이 상태를 읽고 쓰기만 한다. 상태는
//   인터페이스(IMidiInput/IMidiOutput)와 시퀀서 타입만 참조하고
//   ImGui/DirectX 같은 렌더링 세부는 모른다.
// =============================================================

#include "audio/IMidi2Input.h"
#include "audio/ISynthControl.h"
#include "audio/IVstHostControl.h"
#include "core/UndoHistory.h"
#include "mapping/MidiMap.h"
#include "midi/IMidiDevice.h"
#include "midi/MidiOutputRouter.h"
#include "sequencer/Player.h"
#include "sequencer/Song.h"
#include "vst/PluginScanner.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace midipro::gui {

// 기타 도우미의 버튼처럼 "쳤다가 잠시 뒤 떼는" 음을 위한 예약 Note Off.
// 왜 필요한가: 버튼은 Note On만 보내는데, 내장 신스는 sustain 구간에서
//   계속 울린다. 지정 시각이 되면 GUI 루프가 Note Off를 보내 멈춘다.
struct PendingNoteOff {
    uint8_t channel;
    uint8_t note;
    double dueTime; // ImGui::GetTime() 기준 초
};

struct AppState {
    // 장치 (조립 지점에서 주입되는 인터페이스)
    midi::IMidiInput* input = nullptr;
    midi::MidiOutputRouter* output = nullptr; // 하드웨어 MIDI / 내장 신스 라우터
    audio::ISynthControl* synth = nullptr;    // 내장 신스 파라미터 제어
    audio::IVstHostControl* vst = nullptr;    // VST3 악기/이펙트 호스트
    audio::IMidi2Input* midi2 = nullptr;      // MIDI 2.0(UMP) 입력

    // 신스 음색 파라미터 (GUI에서 편집 -> synth->setParams로 반영)
    audio::SynthParams synthParams;

    // 곡 데이터 + 재생 엔진
    seq::Song song;
    std::unique_ptr<seq::Player> player;

    // UI 선택 상태
    int selectedInputPort = 0;
    int selectedOutputPort = 0;
    int selectedTrack = 0;
    float pianoRollZoom = 0.25f; // 픽셀/틱
    bool followPlayhead = true;

    // 루프 구간 재생 (마디 단위, 1-based, loopStartBar~loopEndBar 포함)
    bool loopEnabled = false;
    int loopStartBar = 1;
    int loopEndBar = 2;

    // 메트로놈 / 카운트인(녹음 전 한 마디 프리롤)
    bool metronome = false;
    bool countIn = false;

    // 피아노 롤 편집
    bool editMode = false;    // 켜면 클릭으로 노트 추가/삭제/드래그
    int editNoteLenDiv = 2;   // 새 노트 길이 = ppqn/이 값 (2=8분음표)

    // 노트 드래그 상태 (이동/크기 조절). 시작 시 원본을 트랙에서 빼고
    // 드래그 중엔 고스트로 표시, 놓을 때 새 위치로 되넣는다.
    struct NoteDrag {
        bool active = false;
        enum Mode { Move, Resize } mode = Move;
        uint8_t note = 0;
        uint8_t velocity = 100;
        uint32_t durationTicks = 0;
        uint32_t startTick = 0;
        int grabTickOffset = 0; // 잡은 지점의 (마우스틱 - 시작틱)
        // 현재(드래그 중) 표시 위치
        uint8_t curNote = 0;
        uint32_t curStart = 0;
        uint32_t curDuration = 0;
    } noteDrag;

    // 녹음 (MIDI 입력을 선택 트랙에 기록)
    bool recordArmed = false; // 녹음 준비
    bool recording = false;   // 실제 녹음 중
    bool softThru = true;     // 입력을 출력(신스)로 통과시켜 모니터링
    struct OpenRecNote {
        bool active = false;
        uint32_t startTick = 0;
        uint8_t velocity = 0;
    };
    std::array<OpenRecNote, 128> openRecNotes{}; // 녹음 중 눌린 노트별 시작 틱

    // 신스 프리셋
    int selectedPreset = 0;
    bool presetSaveRequested = false;
    bool presetLoadRequested = false;

    // 프로젝트 통째 저장/불러오기 (.midipro)
    bool projectSaveRequested = false;
    bool projectLoadRequested = false;

    // VST3 플러그인
    std::string vstInstrumentPath;
    std::string vstEffectPath;
    int vstInstrumentClass = 0;
    int vstEffectClass = 0;
    bool vstInstrumentLoadRequested = false;
    bool vstEffectLoadRequested = false;
    // 설치된 플러그인 자동 스캔 결과
    std::vector<vst::PluginEntry> vstScanned;
    bool vstScanDone = false;
    int vstPickInstrument = 0;
    int vstPickEffect = 0;

    // MIDI Learn / 컨트롤러 매핑
    mapping::MidiMap midiMap;
    bool learnArmed = false;                                  // 다음 CC를 학습 대기
    mapping::ParamTarget learnTarget = mapping::ParamTarget::FilterCutoff;
    bool mapSaveRequested = false;
    bool mapLoadRequested = false;

    // 기타 도우미 상태
    int guitarRoot = 0;       // 0=C
    int guitarChordIndex = 0; // commonChords() 인덱스
    int guitarProgram = 24;   // GM 나일론 기타

    // 버튼으로 친 음의 자동 Note Off 예약 목록
    std::vector<PendingNoteOff> pendingOffs;
    std::vector<PendingNoteOff> pendingUmpOffs; // MIDI 2.0 데모용 UMP note-off 예약

    // 입력 모니터 로그 (GUI 스레드에서 poll해 채운다)
    std::deque<std::string> monitorLog;
    std::size_t maxLogLines = 200;
    bool monitorEnabled = false;

    // 각 창 표시 여부 (Tool/설정 메뉴 체크박스로 토글). 기본은 신스만 숨김.
    bool showTransport = true;
    bool showDevices = true;
    bool showTracks = true;
    bool showPianoRoll = true;
    bool showVst = true;
    bool showGuitar = true;
    bool showMonitor = true;
    bool showStatus = true;
    bool showSynth = false;

    // 상태 표시줄 메시지
    std::string statusMessage = "준비됨";

    // 곡 편집 실행취소 히스토리
    core::UndoHistory<seq::Song> history;

    // 곡을 바꾸기 "직전"에 호출해 복원 지점을 남긴다.
    void snapshot() { history.record(song); }

    void log(const std::string& line) {
        monitorLog.push_back(line);
        while (monitorLog.size() > maxLogLines) monitorLog.pop_front();
    }
};

} // namespace midipro::gui
