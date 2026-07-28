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

#include "audio/IAudioClips.h"
#include "audio/IAudioInput.h"
#include "audio/IMidi2Input.h"
#include "audio/ISynthControl.h"
#include "audio/IVstHostControl.h"
#include "core/UndoHistory.h"
#include "gui/Theme.h"
#include "mapping/MidiMap.h"
#include "midi/IMidiDevice.h"
#include "midi/MidiOutputRouter.h"
#include "sequencer/Player.h"
#include "sequencer/Song.h"
#include "vst/PluginScanner.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
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
    audio::IAudioClips* audioClips = nullptr; // 임포트 오디오 클립 재생
    audio::IAudioInput* audioInput = nullptr; // 마이크/인터페이스 캡처(녹음/모니터)

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

    // 재생 위치(플레이헤드/시크). 재생 중엔 매 프레임 현재 틱으로 갱신되고,
    // 정지 상태에선 눈금자를 드래그해 이 위치를 지정한다. 재생은 여기서 시작.
    uint32_t playPosTick = 0;
    bool seekWasPlaying = false; // 눈금자 드래그 시작 시 재생 중이었는지

    // 타임라인 표시 마디 수(오디오 포함 내용 + 여유). 줄지 않고 늘기만 한다.
    uint32_t timelineBars = 20;

    // 이번 프레임에 시크가 있었으니 뷰(트랙 뷰/피아노 롤)를 플레이헤드로 스크롤.
    bool scrollToPlayhead = false;

    // 루프 구간 재생 [loopStartTick, loopEndTick). 틱 기준이라 마디 중간도
    // 자유롭게 잡을 수 있다 (트랜스포트의 마디 입력은 마디 스냅 편집기).
    bool loopEnabled = false;
    uint32_t loopStartTick = 0;
    uint32_t loopEndTick = (uint32_t)seq::kDefaultPpqn * seq::kBeatsPerBar * 2; // 기본 2마디

    // 메트로놈 / 카운트인(녹음 전 한 마디 프리롤)
    bool metronome = false;
    bool countIn = false;
    // 클릭 소리 커스텀 4종: 신스 음 높이 또는 WAV/MP3 샘플 (경로 비면 신스 음)
    int metroClickNote = 77;          // 메트로놈 일반
    int countInClickNote = 84;        // 카운트인 일반
    int accentClickNote = 88;         // 메트로놈 강조(마디 첫 박)
    int countInAccentClickNote = 91;  // 카운트인 강조(첫 클릭)
    std::string metroSamplePath;
    std::string countInSamplePath;
    std::string accentSamplePath;
    std::string countInAccentSamplePath;
    bool metroSampleLoadRequested = false;   // 파일 대화상자 요청 (App이 처리)
    bool countInSampleLoadRequested = false;
    bool accentSampleLoadRequested = false;
    bool countInAccentSampleLoadRequested = false;
    int metroSigIndex = 0;  // 박자: 0=4/4, 1=3/4, 2=6/8
    int countInBeats = 4;   // 카운트인 클릭 횟수 (몇 번 세고 들어갈지)

    // 피아노 롤 편집
    bool editMode = false;    // 켜면 클릭으로 노트 추가/삭제/드래그
    int editNoteLenDiv = 2;   // 새 노트 길이 = ppqn/이 값 (2=8분음표)
    int quantGridDiv = 4;     // 퀀타이즈 격자 = ppqn/이 값 (4=16분음표)

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
        int lastAuditionNote = -1; // 이동 중 마지막으로 들려준 음(음정 바뀔 때만 재생)
    } noteDrag;

    // 피아노 롤 다중 선택 (편집 트랙의 (note,startTick) 키 집합)
    std::set<std::pair<uint8_t, uint32_t>> selectedNotes;
    int selNotesTrack = -1; // 선택이 어느 트랙 것인지(트랙 바뀌면 비운다)

    // Shift+드래그 박스(고무줄) 선택. 움직임 없이 놓으면 "직전 노트 복제 생성".
    struct BoxSelect {
        bool active = false;
        uint32_t anchorTick = 0;
        int anchorNote = 0;
        float downX = 0.0f, downY = 0.0f; // 클릭인지 드래그인지 구분용
    } boxSelect;

    // 직전에 만들거나 만진 노트의 길이/세기 (Shift+클릭 복제 생성에 쓴다)
    uint32_t lastNoteDurationTicks = 0; // 0 = 아직 없음(콤보 길이 사용)
    uint8_t lastNoteVelocity = 100;

    // 선택 노트 무리 이동 드래그
    struct SelMove {
        bool active = false;
        uint32_t grabTick = 0;
        int grabNote = 0;
        int dTick = 0; // 현재 이동량(틱)
        int dNote = 0; // 현재 이동량(반음)
    } selMove;

    // 복사/잘라내기 버퍼 (기준 위치 대비 상대 좌표)
    struct ClipNote {
        int32_t dTick = 0;
        uint32_t dur = 0;
        uint8_t note = 0;
        uint8_t velocity = 0;
    };
    std::vector<ClipNote> noteClipboard;

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

    // 오디오(MP3) 임포트: 선택 트랙에 붙인다
    bool audioImportRequested = false;
    // 특정 트랙에 MIDI 파일 불러오기 (트랙 우클릭 메뉴 -> App이 파일 열고 병합)
    bool midiImportRequested = false;
    int midiImportTrack = -1;

    // 트랙 뷰 스냅: 클립/구간을 격자에 맞춘다. 격자 눈금도 이 간격으로 그린다.
    bool trackSnap = true;
    int trackSnapDiv = 2; // 0=1마디 1=1/2마디 2=1박 3=1/2박 4=1/4박

    // 내보내기(믹스다운) 설정 창: 재생 없이 오프라인으로 즉시 렌더한다
    bool showExportDialog = false;
    bool exportUseMp3 = false;
    int exportMode = 0; // 0 = 전체 믹스, 1 = 트랙별 스템, 2 = 선택 트랙만        // false=WAV, true=MP3(192kbps)
    bool exportCustomRange = false;   // 사용자 지정 구간 (마디)
    // 구간은 틱이 기준 (마디 입력과 가장자리 드래그·초 입력이 모두 이 값을 조작)
    uint32_t exportStartTick = 0;
    uint32_t exportEndTick = 0; // 0 = 아직 미초기화 (창 열 때 내용 길이로 설정)
    std::string exportDir;                 // 저장 폴더 (UTF-8)
    std::string exportFileName = "믹스다운"; // 파일 이름 (확장자는 형식에 따라 자동)
    bool exportBrowseRequested = false; // 폴더 선택 대화상자 (App이 처리)
    bool exportRunRequested = false;    // 내보내기 실행 (App이 처리)

    // 오디오 입력(인터페이스) 녹음/모니터 — 트랙별 컨트롤. 모니터는 ASIO 전용.
    int audioRecTrack = -1; // 지금 오디오 녹음 중인 트랙(-1=없음)
    uint32_t audioRecStartTick = 0; // 녹음을 시작한 타임라인 위치(클립을 여기 배치)
    bool audioRecPending = false;    // 카운트인 진행 중 (끝나면 실제 캡처 시작)
    double audioRecPendingUntil = 0.0; // 카운트인이 끝나는 시각 (ImGui 시간)
    // 재생/루프 카운트인 동안 오디오 클립 재생을 멈춰 두고, 카운트인이
    // 끝나는 순간 시작한다 (녹음의 audioRecPending과 같은 판정 방식)
    bool audioStartPending = false;
    uint32_t prevPlayPosTick = 0;    // 루프 되감김 감지용 (직전 프레임 위치)
    int asioTrack = -1;     // 지금 ASIO 듀플렉스 모니터 중인 트랙(-1=없음)
    int asioDeviceIndex = 0; // ASIO 장치 선택(전역)
    // ASIO 장치 목록은 드라이버를 로드하므로 매 프레임이 아니라 버튼으로 1회만 스캔한다.
    std::vector<std::string> asioDevices;
    bool asioScanned = false;

    // 오디오 클립 드래그(이동/좌우 트림/속도) 상태
    struct ClipDrag {
        bool active = false;
        int track = -1;
        int clip = -1; // 트랙 안에서 몇 번째 클립을 끄는 중인가
        // Speed: Shift+오른쪽 끝 = 배속(음정 변함) / Stretch: Ctrl+오른쪽 끝 =
        // 음정 유지 스트레치(놓을 때 WSOLA로 오프라인 처리)
        enum Mode { Move, TrimL, TrimR, Speed, Stretch } mode = Move;
        int grabTickOffset = 0;   // Move: 잡은 지점(마우스틱 - startTick)
        uint32_t origStartTick = 0;
        int64_t origTrimStart = 0;
        int64_t origTrimLen = 0;
        double origSpeed = 1.0;
        double stretchTargetTicks = 0.0; // Stretch: 드래그 중 목표 길이(틱) 미리보기
        float downX = 0.0f, downY = 0.0f; // 클릭(시크)인지 드래그(이동)인지 구분
    } clipDrag;

    // 레인 우클릭이 어느 클립 위에서 일어났는가 (-1 = 빈 곳, 메뉴 내용을 가른다).
    int laneCtxClip = -1;
    uint32_t laneCtxTick = 0; // 우클릭한 지점의 틱 (가위 자르기 위치)

    // 트랙 뷰 각 레인의 화면 y범위 (드래그드롭 대상 트랙 판정용). 매 프레임 갱신.
    struct LaneRect {
        float y0 = 0.0f;
        float y1 = 0.0f;
    };
    std::vector<LaneRect> laneRects;

    // VST3 플러그인
    std::string vstInstrumentPath;
    std::string vstEffectPath;
    int vstInstrumentClass = 0;
    int vstEffectClass = 0;
    // VSTi 출력을 보낼 트랙 (-1 = 마스터 직행). 그 트랙의 볼륨/팬/이펙트가 걸린다.
    int vstInstrumentTrack = -1;
    bool vstInstrumentLoadRequested = false;
    bool vstEffectLoadRequested = false;
    // 설치된 플러그인 자동 스캔 결과
    std::vector<vst::PluginEntry> vstScanned;
    bool vstScanDone = false;
    // 트랙 이펙트/악기로 쓸 수 있는 것만 걸러둔 목록 (+FX/+악기 팝업용). 1회 계산.
    std::vector<vst::PluginEntry> vstEffectsOnly;
    bool vstEffectsFiltered = false;
    std::vector<vst::PluginEntry> vstInstrumentsOnly;
    bool vstInstrumentsFiltered = false;
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

    // 각 창 표시 여부 (Tool/설정 메뉴 체크박스로 토글).
    bool showTransport = true;
    bool showDevices = false;  // MIDI 장치는 '설정 > 개인설정'으로 이동
    bool showTracks = false;   // 옛 트랙 목록(간단 컨트롤). 기본은 트랙 뷰 사용
    bool showTrackView = true; // DAW식 트랙 레인 뷰(헤더 + 타임라인 내용)
    bool showPianoRoll = true;
    bool showDrums = false;   // 드럼 트랙 에디터 (Tool 메뉴)
    bool showArrange = false; // 어레인지 뷰 (구간 블록 재배열, Tool 메뉴)
    bool showTab = false;     // '기타 연습' 창 (타브 + 연습 판정, Tool 메뉴)
    float tabZoom = 0.08f;    // 타브 악보 가로 확대 (px/tick)
    // 연습 창에 함께 띄울 트랙들 (비어 있으면 practiceTrack 하나 — 기타 1·2 동시 보기용)
    std::vector<int> tabTracks;
    // '기타 연습' 창에서 고른 트랙 (song.tracks 인덱스, practice=true인 트랙).
    // MIDI 쪽 selectedTrack과 별개다 — 두 창의 선택이 서로를 흔들지 않는다.
    int practiceTrack = -1;

    // ---- 연습 반주 (MP3/WAV) ----
    // 반주는 연습 트랙 위의 보통 오디오 클립이다 — 레인에서 파형을 보며
    // 드래그해 악보와 맞추고, 재생·볼륨·뮤트는 트랙 기능을 그대로 쓴다.
    // 여기엔 "다시 늘릴 때 쓸 원본"과 템포 정보만 들고 있는다.
    std::shared_ptr<audio::AudioClip> practiceBacking;     // 원본 (스트레치 안 된 것)
    std::shared_ptr<audio::AudioClip> practiceBackingClip; // 트랙에 올라간 재생본
    double practiceBackingBpm = 120.0; // 이 음원의 원래 템포 (사용자가 입력)
    // practiceBackingClip을 만든 배율(원본BPM/곡BPM). 곡 템포든 원본 BPM이든
    // 바뀌면 필요 배율이 달라지므로, 둘 중 뭘 고쳐도 "다시 맞춰야 함"이 잡힌다.
    double practiceBackingMadeRatio = 0.0; // 0 = 아직 없음
    float drumZoom = 0.08f;  // 드럼 에디터 가로 확대 (px/tick)
    int drumSnap = 1;        // 격자: 0=8분, 1=16분, 2=32분
    int drumSwing = 54;      // 스윙 % (적용 버튼으로 사용)

    // 드럼 샘플 배정: 노트 -> WAV 경로 (없으면 내장 드럼 신스). 프로젝트에 저장.
    std::map<int, std::string> drumSamplePaths;
    bool packageExportRequested = false; // 파일 > 패키지로 내보내기 (App이 처리)
    bool exportStemLimiter = false; // 스템 내보내기에 마스터 리미터 세팅 적용
    int drumBrowseNote = -1;   // 샘플 선택 창의 대상 노트 (-1 = 닫힘)
    std::string drumBrowseDir; // 브라우저 현재 폴더 (비면 src/Drum부터)

    // 트랙 뷰 미니맵 <-> 표 안쪽 스크롤 연동 (표가 매 프레임 기록/소비)
    float tvScrollX = 0.0f;    // 현재 가로 스크롤 (타임라인 px)
    float tvVisibleW = 0.0f;   // 보이는 폭 (px)
    float tvScrollReq = -1.0f; // 미니맵이 요청한 스크롤 (음수 = 요청 없음)
    float tvScrollYDelta = 0.0f; // 트랙 뷰 전용 세로 스크롤 (Shift+휠, 표가 소비)
    bool showVst = true;
    bool showGuitar = false; // 기타 도우미 — 기본 화면에서 뺐다 (Tool 메뉴로 켜기)
    bool showMonitor = false;  // 시작 시 꺼둔다 (Tool에서 켜기)
    bool showStatus = false;   // 시작 시 꺼둔다 (Tool에서 켜기)
    bool showSynth = false;
    bool showMixer = true;        // 믹서: 마스터 + 모든 트랙 스트립
    bool showMixerCompact = true; // 채널 창: 마스터 + 선택 트랙만 (왼쪽 컴팩트)

    // ---- 믹서 레벨 미터 표시 상태 ----
    // 엔진이 걷어준 순간 피크를 GUI가 보기 좋게 다듬는다:
    // 상승은 즉시, 하강은 천천히(dB 스케일), 피크는 2초 홀드, 클립은 래치.
    struct MeterView {
        float disp = 0.0f;   // 표시 높이(0~1, dB 스케일)
        float hold = 0.0f;   // 피크 홀드 위치(0~1)
        double holdAt = 0.0; // 피크가 잡힌 시각
        bool clip = false;   // 0dB 초과 래치 (미터 클릭으로 해제)
    };
    MeterView meterMaster[2]; // 마스터 L/R (믹서)
    MeterView meterTrack;     // 선택 트랙 버스 (믹서)
    MeterView meterBus[16];    // 트랙 목록 미니 미터 (버스별)
    MeterView meterBusHdr[16]; // 트랙 뷰 헤더 세로 미터 (스무딩 상태는 창마다 별도)
    MeterView meterMix[16];    // 믹서 트랙 스트립 미터
    MeterView meterMasterC[2]; // 채널(컴팩트) 창 마스터 L/R
    MeterView meterTrackC;     // 채널(컴팩트) 창 선택 트랙
    // 원시 피크 캐시: 엔진 poll은 읽으면 0으로 리셋되는 소비형이라,
    // applyTransportState가 프레임당 한 번만 걷고 모든 미터가 이 값을 읽는다.
    float busPeakCache[16] = {};
    float masterPeakCache[2] = {};
    bool showPreferences = false; // 설정 > 개인설정 (MIDI 장치 + 신디사이저 분류) // 설정 > 개인설정 (MIDI 장치 + 신디사이저 분류)

    bool showHelp = false;  // 도움말 > 단축키 창 (F1)
    bool showAbout = false; // 도움말 > MidiPro 정보

    // 시작 화면 (첫 진입 안내: 새 곡·기타 연습·드럼·최근 파일). App이 시작 시 켠다.
    bool showStartScreen = true;
    bool startScreenOnLaunch = true; // "시작할 때 표시" (settings.ini에 저장)
    bool newSongRequested = false;   // 시작 화면 '새 곡' — App이 처리(정지+초기화)
    bool projectOpenRequested = false; // 시작 화면 '프로젝트 열기' — App이 대화상자

    // 좌측 브라우저 (악기·이펙트·최근 파일 탐색)
    bool showBrowser = true;
    int browserTab = 0; // 0=악기 1=이펙트 2=최근

    // 최근 프로젝트 (파일 메뉴). App이 %LOCALAPPDATA%\MidiPro\recent.txt로 유지.
    std::vector<std::string> recentProjects; // UTF-8 경로, 최신이 앞
    std::string recentOpenPath;              // 메뉴에서 고른 경로 (App이 열고 비움)

    // 지금 작업 중인 프로젝트 파일 (UTF-8). 열거나 저장할 때 갱신된다.
    // 비어 있으면 "아직 파일로 저장한 적 없는 새 곡"이다.
    // 외부 제어(ControlServer)의 save/reload가 이 경로를 쓴다.
    std::string projectPath;

    // 외부 제어 통로(네임드 파이프)를 켤지 (개인설정, settings.ini에 저장).
    // 시작할 때만 반영된다 — 끄고 켜려면 앱을 다시 실행해야 한다.
    bool controlPipeOn = true;

    // 업데이트 내용 창. lastSeenVersion은 "이 버전 알림을 봤다"는 표시로
    // settings.ini에 저장된다 — 새 버전으로 처음 켤 때만 창이 뜬다.
    bool showWhatsNew = false;
    std::string lastSeenVersion;

    // ---- UI 테마 (개인설정 > 테마) ----
    ThemeParams theme;              // 현재 테마 파라미터 (시작 시 파일에서 복원)
    bool showStyleEditor = false;   // 고급 스타일 편집기(ImGui 내장) 창
    bool themeDirty = false;        // 변경됨 -> App이 테마 파일에 저장
    // 배경 이미지: 실제 파일 열기/디코드는 DX11 장치를 가진 App이 처리한다
    bool bgImageOpenRequested = false;  // 파일 대화상자를 열어 달라
    bool bgImageClearRequested = false; // (사용 안 함 — 레이어 삭제로 대체)
    int bgImageTargetWindow = -1;       // 요청 대상 (-1 = 전체 배경, 0.. = 그 창)
    char bgImageInfo[64] = "";          // 첫 레이어 크기 표시용
    // 레이어 목록이 바뀌어 텍스처를 다시 맞춰야 함:
    // -1=없음, 0=전체 배경, 1+n=n번 창
    int bgLayersDirty = -1;
    int bgLayerSel = 0;                 // 목록에서 고른 레이어
    // 위젯 스킨(버튼·탭·제목 표시줄 이미지, 모든 창 공통)
    bool skinImageOpenRequested = false;
    int skinImageSlot = 0;              // kSkinButton / kSkinTab / kSkinTitle
    // 창별 스타일 오버라이드 (켜지 않은 항목은 전체 테마를 상속)
    WindowStyleOverride windowStyles[kThemeWindowCount];
    int themeTargetWindow = -1; // 테마 탭의 '적용 대상' (-1 = 전체)

    // ---- 내 테마 (전체 + 창별 설정을 통째로 이름 붙여 저장) ----
    // 파일 입출력은 저장 폴더를 아는 App이 처리한다 (요청 플래그 방식).
    char themeSaveName[64] = "";        // 저장할 이름
    bool themeSaveRequested = false;
    std::string themeLoadRequested;     // 불러올 테마 이름
    std::string themeDeleteRequested;   // 지울 테마 이름
    std::vector<std::string> themeFiles; // 저장된 테마 목록 (App이 채운다)
    bool themeListDirty = true;          // 목록을 다시 읽어 달라
    // 공유: 한 파일로 내보내기 / 받은 파일 가져오기 (배경 이미지 포함)
    std::string themeExportRequested; // 내보낼 테마 이름
    bool themeImportRequested = false;
    // 테마 탭을 얼마나 펼쳐 보일지 (0=기본 1=자세히 2=고급).
    // 처음 쓰는 사람에게 선택지를 한꺼번에 들이밀지 않으려는 장치.
    int themeUiLevel = 0;
    bool themeResetRequested = false; // 색·창별·배경을 처음 상태로

    // 상태 표시줄 메시지
    std::string statusMessage = "준비됨";

    // ---- 곡 편집 실행취소 히스토리 ----
    // Song 복사는 오디오 클립을 shared_ptr로 "공유"하므로, 클립 안의 배치
    // 필드(위치/트림/속도/페이드/게인)는 Song 복사만으로는 보존되지 않는다.
    // PCM까지 통째로 복사하면 메모리가 폭발하니, 배치 값만 나란히 저장해
    // 복원 시 (공유된) 클립 객체에 다시 써 넣는다.
    struct SongSnapshot {
        seq::Song song;
        struct ClipPlace {
            uint32_t startTick;
            double speed;
            int64_t trimStart;
            int64_t trimLen;
            double fadeInSec;
            double fadeOutSec;
            float gain;
        };
        std::vector<ClipPlace> places; // song 안의 클립과 같은 순서(트랙순->클립순)
    };
    static SongSnapshot makeSongSnapshot(const seq::Song& s) {
        SongSnapshot sn;
        sn.song = s;
        for (const auto& t : s.tracks)
            for (const auto& c : t.clips) {
                if (!c) continue;
                sn.places.push_back({c->startTick, c->speed, c->trimStart, c->trimLen,
                                     c->fadeInSec, c->fadeOutSec, c->gain});
            }
        return sn;
    }
    static void applySongSnapshot(seq::Song& out, const SongSnapshot& sn) {
        out = sn.song;
        std::size_t k = 0;
        for (auto& t : out.tracks)
            for (auto& c : t.clips) {
                if (!c) continue;
                if (k >= sn.places.size()) return;
                const auto& p = sn.places[k++];
                c->startTick = p.startTick;
                c->speed = p.speed;
                c->trimStart = p.trimStart;
                c->trimLen = p.trimLen;
                c->fadeInSec = p.fadeInSec;
                c->fadeOutSec = p.fadeOutSec;
                c->gain = p.gain;
            }
    }
    core::UndoHistory<SongSnapshot> history;

    // 곡을 바꾸기 "직전"에 호출해 복원 지점을 남긴다.
    void snapshot() { history.record(makeSongSnapshot(song)); }

    // ---- 버전 분기 (깃 브랜치식) ----
    // "체크인"으로 현재 곡을 노드로 남기고, 노드 우클릭으로 그 버전에서
    // 가지를 뻗는다(불러온 뒤 이어서 체크인하면 자식으로 붙어 분기가 생긴다).
    // 스냅샷은 PCM을 공유하므로 노드가 많아져도 가볍다. (세션 메모리에만 유지)
    struct VersionNode {
        int id = 0;
        int parent = -1;     // -1 = 루트
        std::string name;    // "V1", "V2" ... (이름 바꾸기 가능)
        std::string note;    // 체크인 메모 (한 줄, 비어도 됨)
        SongSnapshot snap;
    };
    std::vector<VersionNode> versions;
    int versionCurrent = -1; // 마지막 체크인/불러온 노드 id (다음 체크인의 부모)
    int versionNextId = 1;
    int versionCtxNode = -1; // 우클릭 팝업 대상 노드 id

    // 트랙 뷰에서 클릭해 선택한 템포 마커 인덱스 (-1 = 없음). Del 키로 삭제.
    int selectedTempoMarker = -1;
    // 클릭해 선택한 구간 마커 인덱스 (-1 = 없음). Del 키로 삭제.
    int selectedMarker = -1;

    // 피아노 롤 아래 레인의 내용: -1 = 벨로시티, 그 외 = 그 번호의 CC 곡선 편집
    int prLaneCc = -1;

    // 휴머나이즈 강도 (피아노 롤 팝업에서 조절)
    int humanTiming = 15; // 타이밍 흔들림 (±틱)
    int humanVel = 8;     // 세기 흔들림 (±)

    // 트랙 뷰 오토메이션 편집 모드: 0=끔(일반 편집), 1=볼륨 곡선, 2=팬 곡선.
    // 켜져 있는 동안 레인 왼쪽 드래그가 곡선 그리기로 바뀐다.
    int autoLane = 0;

    // 이번 프레임의 방향키 스크롤량(px). 메뉴바가 계산하고
    // 트랙 뷰/피아노 롤이 각자 자기 스크롤에 더한다. (←→ 가로, ↑↓ 세로)
    float keyScrollX = 0.0f;
    float keyScrollY = 0.0f;

    // 피아노 롤 스케일 하이라이트: 조성 구성음 행을 초록빛으로 표시
    int scaleRoot = 0; // 0=C, 1=C#, ... 11=B
    int scaleType = 0; // 0=끄기, 1=메이저, 2=마이너(내추럴)

    // 코드 찾기: 선택 트랙 멜로디를 분석해 조성 + 마디별 코드를 얻어 피아노 롤
    // 마디 위에 표시한다. (분석 시점의 결과를 담아두는 캐시 — 자동 갱신 아님)
    bool showChords = false;             // 마디 위 코드 표시 on/off
    std::string chordKeyName;            // 판별된 조성 (예: "C", "Am")
    int chordTrack = -1;                 // 어느 트랙을 분석했는지
    std::vector<std::pair<int, std::string>> barChords; // (마디 인덱스, 코드 이름)

    // 클릭 음량 (메트로놈/카운트인 각각, 0=무음 ~ 1.5=크게)
    float metroVolume = 1.0f;
    float countInVolume = 1.0f;

    // 뮤지컬 타이핑: 컴퓨터 키보드(Z줄=흰건반, S줄=검은건반)로 연주/녹음
    bool musicalTyping = false;
    int mtOctave = 4; // 기준 옥타브 (Z = C4 = 노트 60)

    bool ghostNotes = false; // 피아노 롤에 다른 트랙 노트를 흐리게 표시

    bool templateDialogRequested = false; // 파일 > 새 곡 (템플릿)...
    int loopCount = 0;                    // 이번 재생에서 루프가 돈 횟수 (연습 카운터)
    bool showPerf = false;                // 성능 창 (Tool 메뉴)
    // 성능 창용 시스템 지표 (App이 1초마다 갱신. 음수 = 아직 측정 전)
    float sysCpuPercent = -1.0f; // 시스템 전체 CPU 사용률 (%)

    // 트랙 순서 드래그: 번호 버튼을 놓으면 표 순회가 끝난 뒤 옮긴다
    // (루프 도중 트랙 벡터를 바꾸면 참조가 꼬이므로 지연 실행)
    int trackReorderFrom = -1;
    int trackReorderTo = -1;

    // 트랙 뷰 클립 선택 (클릭으로 선택, Ctrl+C 복사 대상) + 클립보드
    int selClipTrack = -1;
    int selClipIndex = -1;
    // Shift+클릭 다중 선택: (트랙, 클립) 쌍. 우클릭 메뉴에서 한꺼번에 병합한다.
    // 클립 목록이 바뀌는 작업(삭제/분할/병합 등) 후에는 인덱스가 밀리므로 비운다.
    std::set<std::pair<int, int>> selClips;
    std::shared_ptr<audio::AudioClip> clipClipboard; // 복사 시점의 깊은 복사본

    // 내장 이펙트(EQ/딜레이/리버브) 파라미터 창의 대상 (채널, 체인 인덱스). -1 = 닫힘.
    int builtinFxCh = -1;
    int builtinFxIdx = -1;

    // 선택된 MIDI 클립 (트랙 뷰 어레인지 블록). -1 = 없음.
    int selMidiClipTrack = -1;
    int selMidiClipIndex = -1;

    // 클립 위 Ctrl+드래그 "구간 선택": 그 부분만 복사(Ctrl+C)하거나 삭제(Del)한다.
    struct ClipRangeSel {
        int track = -1, clip = -1;
        uint32_t t0 = 0, t1 = 0; // 틱 구간 (드래그 끝나면 t0 <= t1로 정렬)
        bool active = false;     // 드래그 중
    } clipRange;

    void log(const std::string& line) {
        monitorLog.push_back(line);
        while (monitorLog.size() > maxLogLines) monitorLog.pop_front();
    }
};

} // namespace midipro::gui
