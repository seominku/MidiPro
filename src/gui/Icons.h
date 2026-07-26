#pragma once
// =============================================================
// MidiPro - gui/Icons.h
// Segoe MDL2 Assets 아이콘 글리프 (Windows 10+ 내장 segmdl2.ttf).
//
// 사용법: 본문 폰트에 MDL2를 병합해 두면(App.cpp) 문자열 안에 아이콘을
// 그냥 섞어 쓸 수 있다: ICON_PLAY " 재생"
//
// 코드포인트는 Microsoft 공식 "Segoe MDL2 Assets icons" 문서 기준.
// 폰트에 없는 글리프는 물음표로 뜨므로, 새 아이콘을 추가하면 화면으로 확인할 것.
// =============================================================

// UTF-8 인코딩된 U+E7xx/E8xx/EAxx (3바이트)
#define ICON_PLAY       "\xEE\x9D\xA8" // U+E768 Play
#define ICON_PAUSE      "\xEE\x9D\xA9" // U+E769 Pause
#define ICON_STOP       "\xEE\x9C\x9A" // U+E71A Stop
#define ICON_RECORD     "\xEE\xA8\xBB" // U+EA3B RecordDot(원)
#define ICON_PREVIOUS   "\xEE\xA2\x92" // U+E892 Previous(처음으로)
#define ICON_REPEAT     "\xEE\xA3\xAE" // U+E8EE RepeatAll(루프)
#define ICON_FOLDER     "\xEE\xA2\xB7" // U+E8B7 Folder
#define ICON_FILE       "\xEE\xA2\xA5" // U+E8A5 Document
#define ICON_MUSIC      "\xEE\xA3\x96" // U+E8D6 MusicInfo(음표)
#define ICON_SETTINGS   "\xEE\x9C\x93" // U+E713 Settings(톱니)
#define ICON_ADD        "\xEE\x9C\x90" // U+E710 Add(+)
#define ICON_SEARCH     "\xEE\x9C\xA1" // U+E721 Search(돋보기)

// 병합할 글리프 범위 (App.cpp의 폰트 로딩에서 사용)
#define ICONS_RANGE_BEGIN 0xE700
#define ICONS_RANGE_END   0xEA60
