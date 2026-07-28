# MidiPro MCP 서버

AI 어시스턴트(Claude 등)가 MidiPro 프로젝트를 직접 만들고 편집할 수 있게 해주는
MCP(Model Context Protocol) 서버입니다. "C - Am - F - G7 진행에 드럼 깔아서 4마디
만들어줘" 같은 말로 `.midipro` 파일을 만들고 바로 MidiPro에서 열 수 있습니다.

## 어떻게 동작하나

`.midipro`는 사람이 읽을 수 있는 텍스트 포맷이라, 이 서버는 **앱을 전혀 건드리지 않고**
프로젝트 파일을 직접 읽고 씁니다. MidiPro 실행 파일은 프로젝트를 열 때만 씁니다.

- 외부 라이브러리 없음 — Node.js만 있으면 됩니다 (v18 이상).
- 서버가 이해하지 못하는 줄(VST 설정, 오디오 클립, 버전 트리, 신스 음색, 컨트롤러 매핑)은
  **원문 그대로 보존**합니다. 모르는 기능을 지우지 않습니다.
- 기존 파일을 편집할 때는 항상 `<파일이름>.bak` 백업을 남깁니다.

## 설치

Claude Code에 등록하려면:

```powershell
claude mcp add midipro --scope user -- node "C:\Users\<사용자>\...\MidiPro\mcp\midipro-mcp.mjs"
```

등록 후 Claude Code를 다시 시작하면 도구가 잡힙니다. `/mcp` 로 상태를 볼 수 있습니다.

MidiPro 실행 파일은 이 순서로 찾습니다:

1. 환경변수 `MIDIPRO_EXE`
2. `<저장소>/build/MidiPro.exe` (개발 트리)
3. `C:\Program Files\MidiPro\MidiPro.exe` (설치본)

## 도구 목록

| 도구 | 하는 일 |
|---|---|
| `midipro_create_project` | 새 `.midipro` 만들기 (템포·트랙 목록 지정 가능) |
| `midipro_read_project` | 곡 구조를 JSON으로 읽기 (트랙·노트 수·길이·VST·마커) |
| `midipro_add_track` | 트랙 추가 (일반 / 드럼 / 기타 연습) |
| `midipro_add_notes` | 노트 찍기 (음이름 또는 0~127, 박 단위 위치·길이) |
| `midipro_add_chords` | 코드 심볼로 화음 찍기 (`["C","Am","F","G7"]`) |
| `midipro_add_drums` | 스텝 문자열로 드럼 패턴 찍기 |
| `midipro_set_tempo` | 기본 템포(BPM) 바꾸기 |
| `midipro_set_drumkit` | 드럼을 내장 신디 대신 실제 WAV 샘플로 (자동 선택) |
| `midipro_list_drumkits` | 드럼 라이브러리의 드럼머신·계열 목록 |
| `midipro_set_instrument` | 트랙에 VST3 악기(VSTi) 얹기 |
| `midipro_add_effect` | FX 체인에 이펙트 추가 (VST3 또는 내장) |
| `midipro_import_midi` | `.mid` 파일 가져오기 (새 트랙들로 또는 한 트랙에 합치기) |
| `midipro_export_midi` | 프로젝트를 표준 `.mid`(포맷 1)로 내보내기 |
| `midipro_transport` | **실행 중인 앱 조종** (재생/정지/시크/템포/상태/저장/다시 불러오기) |
| `midipro_preset` | 플러그인 음색 보관·적용 (`.mppreset` / `.vstpreset`) |
| `midipro_open` | 만든 프로젝트를 MidiPro로 열기 |
| `midipro_status` | 설치·실행 상태, 스캔된 VST3 목록, 최근 프로젝트 |

### 시간 단위

위치와 길이는 전부 **박(beat = 4분음표)** 이고 **0박이 곡의 시작**입니다.
4/4에서 한 마디는 4박이므로 2마디 첫 박은 `start: 4`입니다.

### 음높이

`"C4"`, `"F#3"`, `"Bb5"` 같은 이름이나 `0~127` 숫자를 씁니다.
MidiPro 피아노 롤과 같은 기준입니다 (**C4 = 60**, A0 = 21, C8 = 108).

### 코드 심볼

`maj` `m` `5` `dim` `aug` `sus2` `sus4` `6` `m6` `7` `maj7` `m7` `mmaj7` `dim7`
`m7b5` `7sus4` `9` `maj9` `m9` `add9` `11` `13` … 그리고 `G7/B` 같은 분수코드.

### 드럼 패턴

`x` `o` = 치기, `-` `.` 공백 = 쉼, `1`~`9` = 세기(9가 가장 셈), `|`는 무시(마디 구분용).
기본은 한 칸이 16분음표(`stepsPerBeat: 4`)입니다.

악기 이름: `kick` `snare` `rim` `clap` `hat` `openhat` `pedalhat` `crash` `ride`
`ridebell` `splash` `china` `tom1`~`tom4` `floortom` `cowbell` `tambourine` `shaker` `clave`

```json
{ "kick": "x---x---", "snare": "----x---", "hat": "x-x-x-x-" }
```

## 예시

4마디짜리 곡을 만드는 흐름:

```
1. midipro_create_project  path=D:\곡\demo.midipro  bpm=96
     tracks=[{name:"피아노"}, {name:"멜로디",channel:1}, {name:"드럼",type:"drum"}]
2. midipro_add_chords      track="피아노"  chords=["C","Am","F","G7"]  octave=3
3. midipro_add_notes       track="멜로디"  notes=[{pitch:"E5",start:0,duration:1}, ...]
4. midipro_add_drums       track="드럼"    pattern={kick:"x---x---",snare:"----x---"}  repeat=4
5. midipro_open            path=D:\곡\demo.midipro
```

## 실행 중인 앱 조종 (앱 1.3.2 이상)

앱이 `\\.\pipe\MidiPro.Control`을 열어 둡니다. `midipro_transport`가 여기에 붙습니다.

```
midipro_transport  action="status"           -> 재생 여부·위치·템포·트랙 목록
midipro_transport  action="play" | "stop" | "toggle" | "rewind"
midipro_transport  action="seek"   beat=16   -> 16박(4/4에서 5마디)으로 이동
midipro_transport  action="tempo"  bpm=150
midipro_transport  action="save"             -> 열려 있는 파일에 저장
midipro_transport  action="reload"           -> 디스크에서 다시 불러오기
midipro_transport  action="open"   path=...
```

**핵심 사용법** — 앱을 닫지 않고 고치기:

```
1. midipro_add_notes ... force=true      (앱이 열고 있어도 강행)
2. midipro_transport action="reload"     (앱이 그 자리에서 새 내용을 읽는다)
```

`force` 없이 부르면 안전장치가 막습니다(아래 "주의" 참고). 고친 뒤 곧바로
`reload`를 부르면 앱의 자동 저장이 끼어들 틈이 거의 없습니다.

명령은 앱의 **UI 스레드**가 프레임마다 실행합니다 — 버튼을 누른 것과 같은
경로라 오디오가 꼬이지 않습니다. 앱이 대화상자에 막혀 있으면 5초 뒤 그 사실을
알려줍니다. 통로를 끄려면 `settings.ini`의 `control_pipe 0`(앱 재시작 필요).

## 플러그인 음색 (앱 1.3.3 이상)

```
midipro_preset  action="save"  track=0  name="메탈 리드"   -> 지금 그 트랙 악기의 음색을 보관
midipro_preset  action="load"  track=1  name="메탈 리드"   -> 다른 트랙에 그대로 적용
midipro_preset  action="load"  track=0  file="...\Foo.vstpreset"
midipro_preset  action="list"                              -> 보관함 + 표준 .vstpreset 목록
```

`slot`으로 자리를 고릅니다: `-1`(기본) = 트랙 악기, `0` 이상 = 그 번호의 트랙 이펙트.
보관함은 `%LOCALAPPDATA%\MidiPro\presets`입니다.

**한계 — 솔직히**: MCP가 음색을 *만들어* 내지는 못합니다. 음색은 플러그인 내부
데이터라 한 번은 앱에서 손으로 잡아야 합니다. 대신 한 번 잡아 두면 그 뒤로는
"이 트랙에 그 톤 얹어줘"가 자동으로 됩니다. Surge XT 같은 플러그인의 자체 패치
(`.fxp`)는 형식이 달라 아직 못 읽습니다.

적용한 음색을 프로젝트에 남기려면 `midipro_transport action="save"`로 저장하세요
(앱이 플러그인 상태를 프로젝트 옆 사이드카에 함께 저장합니다).

## 드럼 샘플 자동 배정

드럼은 기본이 내장 신디 소리입니다. 실제 WAV로 바꾸려면:

```
midipro_set_drumkit  path=곡.midipro
    -> 드럼 트랙이 쓰는 노트를 찾아 라이브러리에서 킥/스네어/햇/탐/심벌을 자동으로 고른다
midipro_set_drumkit  path=곡.midipro  kit="Akai Producer Kits"  family="Acoustic"
    -> 특정 드럼머신의 특정 계열로 (어쿠스틱 드럼을 원하면 family="Acoustic")
midipro_set_drumkit  path=곡.midipro  dryRun=true
    -> 파일을 고치지 않고 어떤 샘플이 걸릴지만 미리 본다
```

라이브러리는 앱과 같은 자리에서 찾습니다: `<저장소>\src\Drum` →
`%LOCALAPPDATA%\MidiPro\Drum` (환경변수 `MIDIPRO_DRUMLIB`로 지정 가능).

**고르는 방식** — 앱의 자동 분류(`gui/PanelsDrums.cpp`)를 옮기되 두 가지를 고쳤습니다:

- **낱말 단위로 봅니다.** 단순 부분 문자열이면 "Bo**ttom**s Up"이 탐으로,
  "3**rd**"가 라이드로 잡힙니다. 사람이 목록에서 고를 때는 눈으로 걸러지지만
  자동 배정은 그대로 집어가므로 오탐이 치명적입니다.
- **점수를 매깁니다.** "snare"라고 적힌 파일이 "rim"보다, "crash"가 막연한
  "cymbal"보다 먼저 뽑힙니다. `Kik`·`Sn`·`Rd`·`Crsh` 같은 약어도 알아봅니다.

그리고 킷 안에 `Acoustic-Kick-...`, `Urban-Tom-...` 처럼 **계열**이 있으면
필요한 악기를 가장 많이 갖춘 계열로 통일합니다 — 킥만 어쿠스틱, 탐만 힙합인
잡탕을 피하려는 것입니다. 탐 3개처럼 같은 분류에 여러 노트가 걸리면 후보를
고르게 훑어 서로 다른 샘플이 걸리게 하고, 한 파일이 두 드럼에 겹치지 않게 합니다.

## MIDI 파일 주고받기

```
midipro_import_midi  path=곡.midipro  midiPath=받은파일.mid
    -> MIDI의 트랙/채널마다 새 트랙을 만든다 (포맷 0이면 채널별로 나눈다)
midipro_import_midi  path=곡.midipro  midiPath=드럼.mid  track="드럼"  startBeat=8
    -> 기존 트랙 하나에 8박 위치부터 합친다

midipro_export_midi  path=곡.midipro  midiPath=내보내기.mid
    -> 포맷 1 .mid (트랙 0 = 템포 트랙). 다른 DAW로 옮길 때
```

지원 범위는 앱의 `sequencer/SmfFile.cpp`와 같습니다: 포맷 0/1, PPQN 분해능
(SMPTE는 거부), 러닝 스테이터스, 템포·트랙 이름 메타, 채널 보이스 메시지.
노트뿐 아니라 CC·피치벤드·프로그램 체인지도 함께 오갑니다.
오디오 클립과 VST 설정은 MIDI 포맷에 담기지 않습니다.

## 주의

- **MidiPro에서 그 프로젝트를 열어 둔 채로는 편집이 막힙니다.** 앱이 자동 저장하면서
  MCP가 쓴 내용을 덮어쓰기 때문입니다. 앱이 `session.lock`을 만들어 두었고
  `recent.txt` 맨 윗줄이 그 파일이면 열려 있다고 보고 막습니다. 앱을 닫고 편집한 뒤
  다시 여세요. 확실한 판정은 아니므로 `force: true`로 넘길 수 있고,
  `MIDIPRO_OPEN_GUARD=off`로 아예 끌 수도 있습니다.
  (앱이 비정상 종료해 `session.lock`이 남으면 계속 막힙니다 — 앱을 한 번 켰다
  정상 종료하면 풀립니다.)
- 이 서버는 노트·코드·드럼·템포만 다룹니다. VST 로드, 믹스, 오디오 녹음 같은
  기능은 앱에서 하세요 (그런 설정이 담긴 파일을 편집해도 설정은 보존됩니다).

## 테스트

```powershell
node mcp/test-midipro-mcp.mjs
```

서버를 진짜 프로세스로 띄워 stdio JSON-RPC로 대화하며 74가지를 검사합니다
(프로토콜, 각 도구, 오류 처리, 모르는 내용 보존, stdout 청결).
