; =============================================================
; MidiPro 설치 스크립트 (Inno Setup 6)
;
; 만드는 법:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\MidiPro.iss
; 결과물:     installer\out\MidiPro-Setup-<버전>.exe
;
; 방침
;  - 모든 사용자용 설치 (C:\Program Files\MidiPro) — 설치·제거에 관리자 권한이 필요하다.
;  - 드럼 샘플 라이브러리(1.35GB)는 넣지 않는다. 나중에 넣고 싶으면
;    %LOCALAPPDATA%\MidiPro\Drum 에 두면 앱이 알아서 찾는다(관리자 권한 불필요).
;  - 사용자 데이터(%LOCALAPPDATA%\MidiPro: 설정·자동저장·테마)는 제거해도 남긴다.
;    지우고 싶은 사람을 위해 제거 마지막에 물어본다.
; =============================================================

#define AppName "MidiPro"
#define AppVersion "1.3.5"
#define AppPublisher "seominku"
#define AppExe "MidiPro.exe"
; 앱이 만드는 뮤텍스 이름. 설치 중 "프로그램이 실행 중입니다"를 정확히 알아내
; 종료를 요청하는 데 쓴다 (main_gui.cpp의 것과 반드시 같아야 한다).
#define AppMutexName "MidiPro.SingleInstance.Mutex"

[Setup]
; AppId는 "같은 프로그램"임을 알려주는 열쇠 — 버전을 올려도 절대 바꾸지 말 것
; (바꾸면 업그레이드가 아니라 별개 프로그램으로 중복 설치된다)
AppId={{7B3C1E42-9A6D-4F58-8B21-2D4E6A9C1F30}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}

DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; 모든 사용자용 -> 설치 시 관리자 권한(UAC)을 요구한다
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

OutputDir=out
OutputBaseFilename=MidiPro-Setup-{#AppVersion}
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; ---- 덮어쓰기(업데이트) ----
; AppId가 같으므로 이미 깔린 게 있으면 설치 위치를 그대로 물려받아 덮어쓴다.
; 낮은 버전으로 되돌리는 것도 막지 않는다(개발 중에는 오히려 필요하다).
; 앱이 켜져 있으면 파일을 바꿀 수 없으므로 종료를 요청한다.
AppMutex={#AppMutexName}
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=no
LicenseFile=
InfoBeforeFile=

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[Tasks]
Name: "desktopicon"; Description: "바탕 화면에 바로 가기 만들기"; GroupDescription: "추가 아이콘:"
Name: "projectassoc"; Description: ".midipro 파일을 MidiPro로 열기"; GroupDescription: "파일 연결:"

[Files]
Source: "..\build\MidiPro.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\사용법.md";     DestDir: "{app}\docs"; Flags: ignoreversion isreadme
Source: "..\docs\CHANGELOG.md";  DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";        Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\사용법";             Filename: "{app}\docs\사용법.md"
Name: "{group}\{#AppName} 제거";   Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; .midipro 파일 연결 (선택한 경우에만)
; .midipro 확장자는 값만 지우고(다른 앱이 가로챘을 수 있으므로) 비면 키까지 정리한다
Root: HKA; Subkey: "Software\Classes\.midipro"; ValueType: string; ValueName: ""; \
    ValueData: "MidiPro.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: projectassoc
Root: HKA; Subkey: "Software\Classes\MidiPro.Project"; ValueType: string; ValueName: ""; \
    ValueData: "MidiPro 프로젝트"; Flags: uninsdeletekey; Tasks: projectassoc
Root: HKA; Subkey: "Software\Classes\MidiPro.Project\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: "{app}\{#AppExe},0"; Tasks: projectassoc
Root: HKA; Subkey: "Software\Classes\MidiPro.Project\shell\open\command"; ValueType: string; \
    ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: projectassoc

[Run]
Filename: "{app}\{#AppExe}"; Description: "지금 {#AppName} 실행"; \
    WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 설치 뒤에 생기는 것들 (있으면 지운다 — 없으면 조용히 넘어간다)
Type: filesandordirs; Name: "{app}\build"
Type: dirifempty;     Name: "{app}\docs"
Type: dirifempty;     Name: "{app}"

[Code]
// 제거할 때 "설정·자동저장도 지울까요?"를 한 번 묻는다. 기본은 남기기.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\MidiPro');
    if DirExists(DataDir) then
    begin
      // SuppressibleMsgBox를 쓰는 이유: 그냥 MsgBox를 쓰면 /SUPPRESSMSGBOXES(무인
      // 제거)에서도 창이 떠서 사람을 기다리다 영영 멈춘다. 이건 기본값(IDNO =
      // 데이터 남기기)을 자동으로 고른다.
      if SuppressibleMsgBox('설정·테마·자동 저장 파일도 함께 지울까요?' + #13#10 + #13#10 +
                DataDir + #13#10 + #13#10 +
                '지우면 테마와 개인설정이 사라집니다.' + #13#10 +
                '(직접 저장한 프로젝트 파일은 이 폴더 밖에 있으므로 영향받지 않습니다)',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
