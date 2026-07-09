# MidiPro

MIDI 시퀀서/신디사이저 + 기타 연주 보조 기능 프로그램.
코딩 규칙은 [Rule.md](Rule.md)를 따른다.

## 아키텍처

```
┌─────────────────────────────────────────┐
│              GUI / UI Layer              │  (피아노롤, 트랙뷰, 노브/페이더)
├─────────────────────────────────────────┤
│         프로젝트 / 세션 관리              │  (저장/불러오기, Undo/Redo)
├─────────────────────────────────────────┤
│   시퀀서 엔진   │   신디사이저 엔진        │
├─────────────────────────────────────────┤
│         MIDI 라우팅 / 매핑 레이어         │  (MIDI Learn, 채널 관리)
├─────────────────────────────────────────┤
│  MIDI I/O (RtMidi)  │  Audio I/O (RtAudio)│
└─────────────────────────────────────────┘
```

계층 간 경계는 추상 인터페이스로만 넘는다:
`IMidiInput`/`IMidiOutput` (midi/), `IAudioEngine` (audio/, Phase 3 확장 지점).
구체 타입은 `src/main.cpp`(조립 지점)에서만 생성한다.

## 진행 상황

- [x] **Phase 1** — MIDI 입출력 연결, 메시지 파서/생성기, 콘솔 로그
- [x] **Phase 2** — 시퀀서 코어(타이밍/트랙/이벤트, .mid 읽기·쓰기, 재생 엔진) + GUI
- [x] **Phase 3** — 내장 신디사이저 (RtAudio, 오실레이터+ADSR, 폴리포니, 필터/LFO/딜레이)
- [x] **Phase 4** — GUI 고도화 (MIDI 녹음, 피아노 롤 노트 편집, 신스 프리셋)
- [x] **Phase 5** — 컨트롤러/매핑 (MIDI Learn, CC→파라미터, 매핑 저장/불러오기)
- [x] **Phase 6** — 확장: Undo/Redo, VST3 호스팅(악기+이펙트), MPE,
  **MIDI 2.0(UMP 코덱 + 내장 신스의 고해상도·노트별 처리)** 완료.
  단, MIDI 2.0 **하드웨어 입출력**은 Windows MIDI Services(UMP 네이티브 스택)가
  필요해 별도 백엔드 과제로 남김 — 현재 RtMidi는 MIDI 1.0 전용.

GUI는 [Dear ImGui](https://github.com/ocornut/imgui) `docking` 브랜치 + Win32/DirectX11,
오디오는 [RtAudio](https://github.com/thestk/rtaudio)(WASAPI),
VST3 호스팅은 [Steinberg VST3 SDK 3.8.0](https://github.com/steinbergmedia/vst3sdk)의
호스팅 부분만 벤더링(에디터 렌더링용 vstgui 제외)해 만들었으며, 모두 외부 DLL 없이
MSVC 단독으로 빌드된다. (VST3 플러그인 자체는 런타임에 로드하는 DLL이라 별도 설치 필요.)

## 빌드 & 테스트

Visual Studio 2022 Community(C++ 툴셋)가 필요합니다.

```
build.bat    →  유닛 테스트를 실행하고 통과 시 실행 파일 생성
```

생성물:
- `build\MidiPro.exe` — **GUI 앱 (Phase 2 메인)**
- `build\MidiProConsole.exe` — Phase 1 콘솔 도구 (MIDI I/O 확인용, 유지)
- `build\MidiProTests.exe`, `build\MidiProSeqTests.exe` — 유닛 테스트

## 실행 (Phase 3 GUI 테스트 방법)

`build\MidiPro.exe` 실행. 창은 도킹 가능한 여러 패널로 구성된다.

1. **MIDI 장치** 패널 → **출력 대상** 선택
   - `내장 신디사이저`(권장): Phase 3에서 만든 소프트 신스로 소리를 낸다 → **열기**
   - `하드웨어 MIDI`: "Microsoft GS Wavetable Synth" 등 OS 신스 사용
2. **트랙** 패널 → `+ 트랙 추가` → `데모 채우기`(C 메이저 스케일 한 마디 삽입)
3. **트랜스포트** → `▶ 재생` 누르면 소리가 나고, **피아노 롤**에 빨간 재생 헤드가 지나감
4. **신디사이저** 패널(내장 신스 선택 시) →
   - 파형(Sine/Saw/Square/Triangle), ADSR, 필터 Cutoff/Resonance,
     LFO, 딜레이, 마스터 볼륨을 실시간 조절 → 소리가 즉시 바뀜
   - `활성 보이스`로 동시 발음 수(폴리포니) 확인
5. **기타 도우미** →
   - 표준 튜닝 버튼(6 E2 ~ 1 E4)으로 기준음 재생
   - 루트/코드 선택 시 지판에 구성음 위치 표시(주황=루트), `코드 재생`으로 소리 확인
6. **피아노 롤 노트 편집** → `노트 편집` 체크 후, 빈 칸 좌클릭=추가(16분음표
   스냅), 노트를 드래그=이동, 오른쪽 끝 드래그=길이 조절, 우클릭=삭제.
   **트랜스포트의 `루프`**로 구간(마디) 반복, `메트로놈`/`카운트인`으로 박 클릭·
   녹음 프리롤. 파일 메뉴의 `프로젝트 저장(.midipro)`은 곡·음색·VST·매핑을 통째로 보존
7. **녹음** → MIDI 입력 포트를 연 뒤 트랜스포트의 `● 녹음`을 누르고 연주하면
   선택 트랙에 기록된다. `소프트 스루`가 켜져 있으면 연주가 내장 신스로 들린다
8. **신스 프리셋** → 신디사이저 패널에서 내장 프리셋(패드/리드/베이스/기타풍)을
   고르거나, 현재 음색을 `.synth` 파일로 저장/불러오기
9. **MIDI Learn** → 신디사이저 패널에서 파라미터 슬라이더 옆 `Learn`을 누르고
   컨트롤러 노브/페이더를 움직이면 그 CC가 연결된다. 이후 노브를 돌리면 값이
   실시간으로 바뀐다. 버튼은 연결되면 `CC번호`를 표시(우클릭으로 해제).
   `매핑 저장`/`매핑 열기`로 `.map` 파일에 보관
10. **실행취소/다시실행** → 편집 메뉴 또는 `Ctrl+Z`/`Ctrl+Y`(Ctrl+Shift+Z).
    트랙 추가·데모 채우기·노트 추가/삭제·녹음·새 곡·불러오기가 되돌려진다
11. **VST3 플러그인** → `VST3 플러그인` 패널에서
    - **설치된 플러그인 자동 스캔**: 패널을 열면 표준 폴더(`Common Files\VST3`,
      `%LOCALAPPDATA%\Programs\Common\VST3`)를 훑어 목록을 만든다. 콤보에서
      고르고 `불러오기`만 누르면 된다(파일 탐색 불필요). `새로고침`으로 재스캔.
    - **악기(VSTi)**: 고른 악기가 내장 신스 대신 발음한다(출력 대상을
      `내장 신디사이저`로 열어야 소리남). `에디터 열기`로 플러그인 GUI를 띄운다.
    - **이펙트**: 리버브/컴프 등으로 내장 신스·악기 출력을 후처리한다.
    - `플러그인 선택...`으로 표준 폴더 밖의 .vst3를 직접 지정할 수도 있다.
12. **MPE (노트별 표현)** → 신디사이저 패널의 `MPE 모드`를 켜면 내장 신스가
    노트마다 독립적인 피치벤드(±48반음)·압력(애프터터치)·음색(CC74)을 처리한다.
    MPE 컨트롤러(ROLI, LinnStrument 등)를 입력으로 열고 `소프트 스루`를 켜면
    각 손가락의 벤딩·프레셔가 개별 음에 적용된다. 일반 모드는 벤드 ±2반음.
    **VST 악기**에도 전달된다: 피치벤드/애프터터치/CC74를 플러그인의 `IMidiMapping`
    으로 조회한 파라미터에 넣어주므로, Surge XT 등 MPE 지원 플러그인에서 노트별
    표현이 동작한다(Surge에서 C4→+2반음 벤드로 293Hz 확인).
13. **MIDI 2.0** → 신디사이저 패널의 `MIDI 2.0 데모 (노트별 벤딩)`을 누르면
    **한 채널에서 3음을 각각 다르게 벤딩**한다(MIDI 1.0/MPE는 채널을 나눠야
    가능한 것을 MIDI 2.0은 한 채널에서 처리). 내부적으로 UMP 패킷(16비트
    벨로시티 + 32비트 노트별 피치벤드)을 만들어 신스로 보낸다. 출력 대상을
    `내장 신디사이저`로 열어야 들린다.
14. **파일 메뉴** → `저장 (.mid)`으로 내보내고, `열기 (.mid)`로 다시 불러오기

## 폴더 구조 (Rule 4)

```
MidiPro/
├── .clang-format               # 코드 스타일 (Rule 2)
├── build.bat                   # MSVC 빌드 + 테스트 스크립트
├── external/
│   ├── rtmidi/                 # RtMidi 6.0.0 (벤더링)
│   ├── rtaudio/                # RtAudio 6.0.1 (벤더링)
│   ├── imgui/                  # Dear ImGui docking + Win32/DX11 백엔드 (벤더링)
│   └── vst3sdk/                # VST3 SDK 3.8.0 호스팅 부분 (벤더링, vstgui 제외)
├── src/
│   ├── main.cpp                # 콘솔 진입점 (Phase 1)
│   ├── main_gui.cpp            # GUI 진입점 = 조립 지점 (Phase 2)
│   ├── core/
│   │   ├── SpscQueue.h         #   락프리 SPSC 큐 (Rule 3)
│   │   └── UndoHistory.h       #   스냅샷 실행취소/다시실행 (순수, 템플릿)
│   ├── midi/                   # MIDI I/O, 파싱
│   │   ├── MidiConstants.h     #   프로토콜 상수 (매직 넘버 금지, Rule 5)
│   │   ├── MidiMessage.h/.cpp  #   메시지 파서/생성기 (순수 로직)
│   │   ├── IMidiDevice.h       #   장치 추상 인터페이스 (Rule 1)
│   │   └── RtMidiDevice.h/.cpp #   RtMidi 구현체
│   ├── audio/                  # 신디사이저 (Phase 3)
│   │   ├── IAudioEngine.h      #   오디오 엔진 확장 지점 (Rule 8)
│   │   ├── ISynthControl.h     #   신스 파라미터 제어 인터페이스 (Rule 1)
│   │   ├── Synth.h/.cpp        #   폴리포닉 신스 DSP (순수, 오디오 스레드 전용)
│   │   ├── SynthPreset.h/.cpp  #   프리셋 직렬화 + 내장 음색 (순수 로직)
│   │   ├── RtAudioEngine.h/.cpp#   RtAudio 스트림 = IMidiOutput+ISynthControl
│   │   └── dsp/                #   Oscillator/Adsr/Filter/Delay (헤더 전용 DSP)
│   ├── sequencer/              # 시퀀서 코어 (Phase 2)
│   │   ├── TimeBase.h/.cpp     #   BPM/틱/초 변환 (순수 로직)
│   │   ├── MidiEvent.h         #   이벤트 값 타입
│   │   ├── Track.h/.cpp        #   트랙 + 노트 구간 추출
│   │   ├── Song.h/.cpp         #   곡 (트랙 목록 + 타이밍)
│   │   ├── SmfFile.h/.cpp      #   .mid(SMF) 읽기/쓰기
│   │   └── Player.h/.cpp       #   재생 엔진 (스냅샷 + 아토믹, Rule 3)
│   ├── guitar/                 # 기타 연주 보조
│   │   └── Fretboard.h/.cpp    #   지판/코드 계산 (순수 로직)
│   ├── mapping/                # 컨트롤러 매핑 (Phase 5)
│   │   └── MidiMap.h/.cpp      #   CC→파라미터 매핑·값 스케일·직렬화 (순수)
│   ├── midi2/                  # MIDI 2.0 (Phase 6)
│   │   └── Ump.h/.cpp          #   UMP 코덱·MIDI2 채널보이스·스케일 변환 (순수)
│   ├── vst/                    # VST3 호스팅 (Phase 6)
│   │   ├── Vst3Host.h/.cpp     #   .vst3 로드·오디오 처리·에디터 (SDK는 여기만)
│   │   └── PluginScanner.h/.cpp#   표준 폴더에서 설치된 .vst3 자동 탐색
│   ├── midi/MidiOutputRouter.h #   하드웨어 MIDI / 내장 신스 전환 (Rule 1/8)
│   └── gui/                    # GUI (ImGui/DX11만 여기서 참조, Rule 1)
│       ├── AppState.h          #   상태 묶음 (렌더링 백엔드 무관)
│       ├── App.h/.cpp          #   Win32+DX11+ImGui 셸 + 메인 루프
│       └── Panels.h/.cpp       #   패널별 렌더링 (SRP)
└── tests/
    ├── test_midi_message.cpp   # MIDI 파서 유닛 테스트
    ├── test_sequencer.cpp      # 타이밍/노트추출/노트편집/VLQ/SMF왕복/지판 테스트
    ├── test_synth.cpp          # 신스 DSP(발음/폴리포니/릴리스/보이스스틸)/프리셋 테스트
    ├── test_mapping.cpp        # MIDI Learn 매핑(스케일링/1:1 바인딩/직렬화) 테스트
    ├── test_undo.cpp           # 실행취소/다시실행(스택/redo무효화/깊이/Song) 테스트
    └── test_ump.cpp            # MIDI 2.0 UMP 코덱(빌드/파싱 왕복/스케일) 테스트
```

## 스레딩 규칙 요약 (Rule 3)

- MIDI 입력 콜백(실시간성 스레드)에서는 **파싱 + 락프리 큐 push만** 한다.
  로깅, 메모리 할당, 뮤텍스, 파일 I/O 금지.
- `MidiMessage`는 동적 할당 없는 고정 크기 값 타입 — 큐 복사가 안전하다.
- 로깅/화면 출력은 항상 메인 스레드에서 `poll()`로 꺼내서 한다.
- 큐가 가득 차면 블로킹 대신 드롭하고 `droppedCount()`로 집계한다.
- **오디오 콜백**(RtAudio 실시간 스레드)은 락프리 큐에서 노트/파라미터
  이벤트를 꺼내 신스에 적용하고 렌더만 한다. 할당·락·로깅 없음.
  보이스 풀과 딜레이 버퍼는 스트림 시작 전에 미리 할당한다.
- 여러 제어 스레드(GUI·재생)가 신스로 보내는 이벤트는 프로듀서 뮤텍스로
  직렬화하되, 소비자(오디오 스레드)는 락을 잡지 않는다.
