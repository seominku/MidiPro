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

inline constexpr const char* kAppVersion = "1.3.5";

struct ReleaseNote {
    const char* version;
    const char* const* items; // 널 종료 배열
};

// ---- 버전별 항목 (최신이 앞) ----
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

inline const ReleaseNote* releaseNotes(std::size_t& count) {
    static const ReleaseNote kAll[] = {
        {"1.3.5", kNotes135}, {"1.3.4", kNotes134}, {"1.3.3", kNotes133},
        {"1.3.2", kNotes132}, {"1.3.1", kNotes131}, {"1.3.0", kNotes130},
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
