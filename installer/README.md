# MidiPro 인스톨러 만들기

## 준비 (한 번만)

[Inno Setup 6](https://jrsoftware.org/isdl.php)을 설치합니다. winget이 있으면:

```
winget install --id JRSoftware.InnoSetup
```

받는 사람은 Inno Setup이 **필요 없습니다** — 만드는 PC에만 있으면 됩니다.

## 만들기

먼저 앱을 빌드해 `build\MidiPro.exe`가 있어야 합니다.

```
build.bat
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\MidiPro.iss
```

결과: `installer\out\MidiPro-Setup-1.1.0.exe` (약 3MB)

## 설치되는 내용

| 항목 | 위치 |
|---|---|
| 프로그램 | `C:\Program Files\MidiPro\MidiPro.exe` |
| 문서 | `C:\Program Files\MidiPro\docs\` |
| 시작 메뉴 | `MidiPro`, `사용법`, `MidiPro 제거` |
| 바탕 화면 아이콘 | 설치 중 선택 |
| `.midipro` 파일 연결 | 설치 중 선택 |

모든 사용자용 설치라 **설치·제거에 관리자 권한(UAC)** 이 필요합니다.

## 제거

`설정 > 앱 > 설치된 앱`에서 **MidiPro**를 제거하거나, 시작 메뉴의 `MidiPro 제거`를 씁니다.

설치한 파일만 지우고, **개인 설정·테마·자동 저장(`%LOCALAPPDATA%\MidiPro`)은 남깁니다.**
제거 마지막에 "설정도 같이 지울까요?"를 한 번 묻고, 기본은 **남기기**입니다.
직접 저장한 `.midipro` 프로젝트 파일은 이 폴더 밖에 있으므로 어떤 경우에도 영향받지 않습니다.

## 드럼 샘플 라이브러리

`src\Drum`(4717개 · 1.35GB)은 설치 파일 크기 때문에 **포함하지 않습니다.**
쓰시려면 다음 중 아무 곳에나 폴더를 두면 앱이 알아서 찾습니다.

1. `C:\Program Files\MidiPro\src\Drum` — 관리자 권한 필요
2. **`%LOCALAPPDATA%\MidiPro\Drum`** — 권한 불필요, 권장

## 버전 올릴 때

`MidiPro.iss` 맨 위 `#define AppVersion` 만 고치면 파일 이름·표시 버전이 함께 바뀝니다.

`AppId`(GUID)는 **절대 바꾸지 마세요.** 이 값이 "같은 프로그램"임을 알려 주므로,
바꾸면 업그레이드가 아니라 별개 프로그램으로 중복 설치됩니다.
