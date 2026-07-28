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

## 주의

- **MidiPro에서 그 프로젝트를 열어 둔 채로 편집하지 마세요.** 앱이 자동 저장하면서
  MCP가 쓴 내용을 덮어씁니다. 앱을 닫고 편집한 뒤 다시 여는 게 안전합니다.
- 이 서버는 노트·코드·드럼·템포만 다룹니다. VST 로드, 믹스, 오디오 녹음 같은
  기능은 앱에서 하세요 (그런 설정이 담긴 파일을 편집해도 설정은 보존됩니다).

## 테스트

```powershell
node mcp/test-midipro-mcp.mjs
```

서버를 진짜 프로세스로 띄워 stdio JSON-RPC로 대화하며 74가지를 검사합니다
(프로토콜, 각 도구, 오류 처리, 모르는 내용 보존, stdout 청결).
