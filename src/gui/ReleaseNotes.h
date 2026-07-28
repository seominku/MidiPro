#pragma once
// =============================================================
// MidiPro - gui/ReleaseNotes.h
// 앱 버전 + 버전별 "무엇이 바뀌었나" (사용자용).
//
// 왜 코드에 넣나 (Rule 1):
//   docs/CHANGELOG.md는 개발용이라 내용이 기술적이고, 설치본에는 따라가지도
//   않는다. 사용자에게 보여줄 문장은 짧게 따로 적어 실행 파일에 넣는다.
//
// 버전 올릴 때: 여기 kAppVersion과 맨 앞 항목을 같이 갱신한다.
//   (설치 스크립트 installer/MidiPro.iss, 리소스 src/app.rc도 함께)
//
// 순수 문자열/비교 로직이라 GUI 없이 단위 테스트한다 (Rule 6).
// =============================================================

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace midipro::gui {

inline constexpr const char* kAppVersion = "1.4.1";

struct ReleaseNote {
    const char* version;
    const char* const* items; // 널 종료 배열
};

// ---- 버전별 항목 (최신이 앞) ----
inline const char* const kNotes141[] = {
    "줄 편집기에서 노트 길이를 조절할 수 있습니다 — 노트 오른쪽 끝을 잡아 끌거나 Shift+←→.",
    "프렛 바꾸기가 제대로 먹습니다. 휠이 화면 가로 스크롤에 먹히던 문제를 고쳤고, "
    "↑↓ 화살표와 숫자 키(프렛 직접 입력)도 됩니다.",
    "고른 노트가 주황색으로 표시되고, ←→로 위치를 옮기거나 Del로 지울 수 있습니다.",
    nullptr,
};
inline const char* const kNotes140[] = {
    "줄 편집기가 생겼습니다 (Tool > 줄 편집기). 기타는 6줄, 베이스는 4줄 격자에서 "
    "노트를 직접 찍고 옮깁니다.",
    "줄 위에서만 찍히니 악기가 못 내는 음이 섞이지 않습니다 — 샘플 기타/베이스는 "
    "실제 음역 밖의 음에서 아무 소리도 내지 않습니다.",
    "클릭=찍기, 드래그=이동(위아래로 줄 바꾸기), 휠=프렛 조절, 우클릭=삭제.",
    "'음역 밖 노트 맞추기'로 피아노 롤에서 찍은 음을 옥타브 단위로 끌어올 수 있습니다.",
    "트랙마다 튜닝을 정할 수 있고 프로젝트에 함께 저장됩니다.",
    nullptr,
};
inline const char* const kNotes136[] = {
    "지난 버전들의 변경 내용도 모두 담았습니다 — 도움말 > 업데이트 내용 에서 처음(1.0)까지 볼 수 있습니다.",
    nullptr,
};
inline const char* const kNotes135[] = {
    "업데이트하면 이 창이 떠서 무엇이 바뀌었는지 알려줍니다.",
    "도움말 > 업데이트 내용 에서 언제든 다시 볼 수 있습니다.",
    nullptr,
};
inline const char* const kNotes134[] = {
    "재생 중에 뮤트를 눌러도 바로 반영됩니다 (전에는 다음 재생 전까지 소리가 그대로였습니다).",
    "밖에서 믹서를 조작할 수 있습니다 — 뮤트·볼륨·팬·게인·리버브 센드, 마스터 포함.",
    nullptr,
};
inline const char* const kNotes133[] = {
    "플러그인 음색을 이름 붙여 저장해 두고, 다른 트랙이나 다른 곡에 그대로 적용할 수 있습니다.",
    "다른 프로그램에서 만든 표준 프리셋(.vstpreset)도 읽습니다.",
    nullptr,
};
inline const char* const kNotes132[] = {
    "앱을 닫지 않고도 밖에서 조종할 수 있는 통로가 생겼습니다 "
    "(재생·정지·위치 이동·프로젝트 다시 불러오기).",
    "파일을 고친 뒤 '다시 불러오기'만 하면 껐다 켤 필요가 없습니다.",
    "쓰지 않으시면 개인설정 파일의 control_pipe 를 0으로 두면 꺼집니다.",
    nullptr,
};
inline const char* const kNotes131[] = {
    "드럼 샘플 브라우저가 이름을 더 정확히 알아봅니다. "
    "'탐'에 엉뚱한 베이스 샘플이 섞이던 문제를 고쳤습니다.",
    "Rd·Kik·Sn 처럼 줄여 쓴 이름도 찾습니다 — 전에는 목록에 아예 안 뜨던 샘플이 있었습니다.",
    nullptr,
};
inline const char* const kNotes130[] = {
    "4K·고배율 화면에서 글자와 화면이 또렷해졌습니다.",
    "시작 화면과 왼쪽 브라우저(악기·이펙트·최근 프로젝트)가 생겼습니다.",
    "슬라이더마다 -/+ 버튼이 붙어 값을 하나씩 정확히 맞출 수 있습니다.",
    "믹서·채널 창이 커지고, FX 체인을 드래그로 순서 바꿀 수 있습니다.",
    "피아노 롤이 88건반이 되고, 트랙 뷰에 스냅과 세밀한 눈금이 생겼습니다.",
    "트랙 뷰 빈 곳을 우클릭해 트랙을 만들 수 있습니다.",
    nullptr,
};

inline const char* const kNotes121[] = {
    "무거운 가상악기(옴니스피어·키스케이프·트릴리안)에서 오류창도 없이 프로그램이 꺼지던 문제를 고쳤습니다.",
    "가상악기를 올린 상태에서 내장 신디사이저를 켜면 꺼지던 문제도 고쳤습니다.",
    "트랙을 지웠다 다시 만들면 옛 악기·이펙트·EQ가 남아 있던 문제를 고쳤습니다.",
    "피아노 롤 맨 위 음에 노트가 안 찍히던 문제를 고치고, 음역을 88건반(A0~C8)으로 넓혔습니다.",
    "아무것도 고르지 않고 Delete를 눌러 트랙이 통째로 지워지던 사고를 막았습니다.",
    "MIDI 장치를 켤 때마다 다시 고르지 않아도 됩니다 — 마지막에 쓰던 장치를 기억합니다.",
    "트랙 [+]를 누를 때 한참 기다리던 문제: 플러그인 조사 결과를 기억해 두 번째 실행부터는 바로 열립니다.",
    "샘플레이트·버퍼 크기를 개인설정에서 고를 수 있습니다.",
    "MIDI 파일을 트랙 위로 끌어다 놓으면 그 트랙에 바로 들어갑니다.",
    "트랙 뷰에 스냅과 세밀한 격자가 생겼습니다.",
    "프로그램이 갑자기 꺼져도 기록을 남깁니다 (%LOCALAPPDATA%\\MidiPro\\crash.log).",
    nullptr,
};
inline const char* const kNotes11[] = {
    "드럼 트랙 에디터, 내장 드럼 신스, 드럼 샘플 라이브러리와 킷 저장이 생겼습니다.",
    "MIDI 클립 — 노트를 블록으로 묶어 트랙 뷰에서 옮기고 복제합니다.",
    "어레인지 뷰에서 구간 블록을 끌어 곡 구조를 바꿉니다.",
    "기타 연습 트랙과 타브 악보 창, 텍스트·PDF 타브 가져오기.",
    "내장 이펙트 5종(EQ·딜레이·리버브·컴프레서·리미터), 사이드체인 덕킹, 센드/리턴 리버브, 마스터 리미터.",
    "카운트인, 피치 벤드 레인, CC 오토메이션, 벨로시티 도구(크레센도·랜덤).",
    "템포 램프(점진 가속·감속), 곡 미니맵, 스윙.",
    nullptr,
};
inline const char* const kNotes10[] = {
    "최초 릴리스 — MIDI 시퀀서, 피아노 롤, 오디오 녹음(ASIO)과 클립 편집, "
    "VST3 호스팅, 믹서, 버전 분기 트리, 내보내기(WAV/MP3/스템), 테마.",
    nullptr,
};

inline const ReleaseNote* releaseNotes(std::size_t& count) {
    static const ReleaseNote kAll[] = {
        {"1.4.1", kNotes141}, {"1.4.0", kNotes140}, {"1.3.6", kNotes136}, {"1.3.5", kNotes135}, {"1.3.4", kNotes134},
        {"1.3.3", kNotes133}, {"1.3.2", kNotes132}, {"1.3.1", kNotes131},
        {"1.3.0", kNotes130}, {"1.2.1", kNotes121}, {"1.1", kNotes11},
        {"1.0", kNotes10},
    };
    count = sizeof(kAll) / sizeof(kAll[0]);
    return kAll;
}

// "1.3.10" > "1.3.9" 처럼 숫자로 비교한다 (문자열 비교면 뒤집힌다).
// a<b면 음수, 같으면 0, a>b면 양수.
inline int versionCompare(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    for (int part = 0; part < 3; ++part) {
        long va = 0, vb = 0;
        if (*a) { va = std::strtol(a, const_cast<char**>(&a), 10); if (*a == '.') ++a; }
        if (*b) { vb = std::strtol(b, const_cast<char**>(&b), 10); if (*b == '.') ++b; }
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

// lastSeen 이후에 나온 항목이 있는가 (= 업데이트 창을 띄울까).
// lastSeen이 비어 있으면 "처음 실행"이라 보고 최신 것 하나만 보여준다.
inline bool hasNewNotes(const char* lastSeen) {
    if (!lastSeen || !*lastSeen) return true;
    return versionCompare(lastSeen, kAppVersion) < 0;
}

// 보여줄 항목 개수: lastSeen보다 새로운 버전들 (비어 있으면 1개).
inline std::size_t newNoteCount(const char* lastSeen) {
    std::size_t n = 0;
    const ReleaseNote* all = releaseNotes(n);
    if (!lastSeen || !*lastSeen) return n ? 1 : 0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (versionCompare(lastSeen, all[i].version) < 0) ++k;
    return k;
}

} // namespace midipro::gui
