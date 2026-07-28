// =============================================================
// MidiPro - tests/test_releasenotes.cpp
// 버전 비교 + "업데이트 창을 띄울까" 판정 (gui/ReleaseNotes.h)
// =============================================================

#include "gui/ReleaseNotes.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace midipro::gui;

static int g_fail = 0;
static void expect(bool cond, const char* what) {
    if (!cond) { std::printf("[FAIL] %s\n", what); ++g_fail; }
}

int main() {
    // ---- 버전 비교 ----
    expect(versionCompare("1.3.0", "1.3.1") < 0, "1.3.0 < 1.3.1");
    expect(versionCompare("1.3.1", "1.3.0") > 0, "1.3.1 > 1.3.0");
    expect(versionCompare("1.3.2", "1.3.2") == 0, "같은 버전");
    expect(versionCompare("1.2.9", "1.3.0") < 0, "마이너가 올라가면 큼");
    expect(versionCompare("0.9.9", "1.0.0") < 0, "메이저가 올라가면 큼");
    // 문자열 비교였다면 "1.3.10" < "1.3.9" 로 뒤집힌다 — 숫자 비교인지 확인
    expect(versionCompare("1.3.10", "1.3.9") > 0, "1.3.10 > 1.3.9 (숫자 비교)");
    expect(versionCompare("1.10.0", "1.9.0") > 0, "1.10.0 > 1.9.0");
    // 자리 수가 모자라도 0으로 본다
    expect(versionCompare("1.3", "1.3.0") == 0, "1.3 == 1.3.0");
    expect(versionCompare("2", "1.9.9") > 0, "2 > 1.9.9");
    expect(versionCompare("", "1.0.0") < 0, "빈 문자열은 가장 낮다");
    expect(versionCompare(nullptr, "1.0.0") < 0, "널도 가장 낮다");
    expect(versionCompare("", "") == 0, "빈 것끼리 같다");

    // ---- 항목 목록 ----
    std::size_t n = 0;
    const ReleaseNote* all = releaseNotes(n);
    expect(n > 0, "항목이 있다");
    expect(std::strcmp(all[0].version, kAppVersion) == 0,
           "맨 앞 항목이 현재 버전과 같다 (버전 올릴 때 같이 갱신)");

    // 내림차순 + 중복 없음
    std::set<std::string> seen;
    for (std::size_t i = 0; i < n; ++i) {
        expect(seen.insert(all[i].version).second, "버전이 중복되지 않는다");
        if (i + 1 < n)
            expect(versionCompare(all[i].version, all[i + 1].version) > 0,
                   "최신이 앞으로 정렬돼 있다");
    }
    // 모든 항목에 내용이 하나 이상 있고 널로 끝난다
    for (std::size_t i = 0; i < n; ++i) {
        int cnt = 0;
        for (const char* const* p = all[i].items; *p; ++p) {
            expect(**p != '\0', "빈 문장이 없다");
            ++cnt;
            if (cnt > 64) break; // 널 종료를 빠뜨렸으면 여기서 걸린다
        }
        expect(cnt > 0 && cnt <= 64, "항목마다 내용이 1~64개");
    }

    // ---- 띄울지 판정 ----
    expect(hasNewNotes(""), "처음 실행이면 띄운다");
    expect(hasNewNotes(nullptr), "값이 없어도 띄운다");
    expect(hasNewNotes("1.3.0"), "옛 버전을 봤으면 띄운다");
    expect(!hasNewNotes(kAppVersion), "이미 이 버전을 봤으면 안 띄운다");
    expect(!hasNewNotes("9.9.9"), "더 최신을 봤으면 안 띄운다 (되돌린 경우)");

    // ---- 몇 개 보여줄까 ----
    expect(newNoteCount("") == 1, "처음 실행이면 최신 하나만");
    expect(newNoteCount(kAppVersion) == 0, "같은 버전이면 없음");
    expect(newNoteCount("1.3.0") == n - 1, "1.3.0을 봤으면 그보다 새 것 전부");
    expect(newNoteCount("0.0.1") == n, "아주 옛 버전이면 전부");

    if (g_fail) {
        std::printf("[FAIL] release notes tests failed (%d)\n", g_fail);
        return 1;
    }
    std::printf("[OK] release notes tests passed\n");
    return 0;
}
