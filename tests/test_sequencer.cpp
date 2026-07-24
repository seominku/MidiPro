// =============================================================
// MidiPro - tests/test_sequencer.cpp
// 시퀀서 순수 로직 유닛 테스트 (Rule 6):
//   타이밍 변환, 노트 추출, VLQ 인코딩, SMF 저장/불러오기 왕복.
// 하드웨어(장치)나 GUI 없이 검증한다.
// =============================================================

#include "pdf/PdfTab.h"
#include "sequencer/ChordFinder.h"
#include "sequencer/DrumPattern.h"
#include "sequencer/SmfFile.h"
#include "sequencer/Song.h"
#include "sequencer/TabImport.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"
#include "guitar/Fretboard.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>

using namespace midipro;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";        \
        }                                                                                          \
    } while (0)

bool approx(double a, double b) { return std::fabs(a - b) < 1e-6; }

void testTimeBase() {
    // 120 BPM, 480 PPQN -> 초당 960틱
    CHECK(approx(seq::ticksPerSecond(120.0, 480), 960.0));
    // 4분음표(480틱) = 0.5초 @120BPM
    CHECK(approx(seq::ticksToSeconds(480, 120.0, 480), 0.5));
    CHECK(approx(seq::secondsToTicks(0.5, 120.0, 480), 480.0));

    // 마디:박:틱 (4/4, 480 PPQN)
    auto p = seq::toBarBeatTick(0, 480);
    CHECK(p.bar == 1 && p.beat == 1 && p.tick == 0);
    p = seq::toBarBeatTick(480, 480); // 2박째
    CHECK(p.bar == 1 && p.beat == 2 && p.tick == 0);
    p = seq::toBarBeatTick(480 * 4, 480); // 2마디째
    CHECK(p.bar == 2 && p.beat == 1 && p.tick == 0);
}

void testTempoMap() {
    seq::Song s;
    s.ppqn = 480;
    s.bpm = 120.0;

    // 템포 변경 없음: 기존 고정 변환과 동일해야 한다
    CHECK(approx(seq::bpmAtTick(s, 0), 120.0));
    CHECK(approx(seq::songTickToSec(s, 960), seq::ticksToSeconds(960, 120.0, 480)));
    CHECK(approx(seq::songSecToTick(s, 1.0), seq::secondsToTicks(1.0, 120.0, 480)));

    // 틱 960부터 60 BPM으로 반감
    s.tempoChanges.push_back({960, 60.0});
    CHECK(approx(seq::bpmAtTick(s, 959.9), 120.0)); // 지점 직전은 이전 템포
    CHECK(approx(seq::bpmAtTick(s, 960.0), 60.0));  // 지점부터 새 템포
    // 960틱 @120 = 1.0초, 이후 480틱 @60 = 1.0초 -> 1440틱 = 2.0초
    CHECK(approx(seq::songTickToSec(s, 960), 1.0));
    CHECK(approx(seq::songTickToSec(s, 1440), 2.0));
    // 역변환 왕복
    CHECK(approx(seq::songSecToTick(s, 1.0), 960.0));
    CHECK(approx(seq::songSecToTick(s, 2.0), 1440.0));
    CHECK(approx(seq::songSecToTick(s, seq::songTickToSec(s, 2345)), 2345.0));

    // 두 번째 지점(틱 1440부터 240 BPM): 1440틱 이후 960틱 = 0.5초
    s.tempoChanges.push_back({1440, 240.0});
    CHECK(approx(seq::songTickToSec(s, 2400), 2.5));
    CHECK(approx(seq::songSecToTick(s, 2.5), 2400.0));

    // 틱 0 지점은 시작 템포를 덮는다
    seq::Song z;
    z.ppqn = 480;
    z.bpm = 120.0;
    z.tempoChanges.push_back({0, 60.0});
    CHECK(approx(seq::bpmAtTick(z, 0), 60.0));
    CHECK(approx(seq::songTickToSec(z, 480), 1.0)); // 480틱 @60 = 1초
}

void testTempoRamp() {
    seq::Song s;
    s.ppqn = 480;
    s.bpm = 120.0;
    // 틱 0~480 동안 120 -> 240으로 점진 가속 (ramp), 이후 240 유지
    s.tempoChanges.push_back({0, 120.0, true});
    s.tempoChanges.push_back({480, 240.0});
    CHECK(approx(seq::bpmAtTick(s, 240), 180.0)); // 중간 = 선형 보간
    CHECK(approx(seq::bpmAtTick(s, 480), 240.0));
    // 램프 구간 시간 = (60/(ppqn·k))·ln(b1/b0) = 0.5·ln2 ≈ 0.3466초
    const double rampSec = seq::songTickToSec(s, 480);
    CHECK(approx(rampSec, 0.5 * std::log(2.0)));
    CHECK(rampSec < 0.5 && rampSec > 0.25); // 일정 120과 일정 240 사이
    // 역변환 왕복 (램프 안/밖 모두)
    CHECK(approx(seq::songSecToTick(s, seq::songTickToSec(s, 300)), 300.0));
    CHECK(approx(seq::songSecToTick(s, seq::songTickToSec(s, 1000)), 1000.0));
}

// MIDI 구간 조작: 경계에 걸친 노트도 스팬째 움직여 On/Off 짝이 안 깨진다
void testMidiRangeOps() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);   // 구간 안
    t.addNote(400, 400, 62, 90);  // 시작은 안, 꼬리는 밖 (경계 걸침)
    t.addNote(960, 240, 64, 80);  // 구간 밖
    seq::shiftMidiRange(t, 0, 480, 480); // [0,480)에서 시작하는 노트를 +480
    const auto ns = seq::extractNotes(t);
    CHECK(ns.size() == 3);
    bool ok60 = false, ok62 = false, ok64 = false;
    for (const auto& n : ns) {
        if (n.note == 60 && n.startTick == 480 && n.endTick == 960) ok60 = true;
        if (n.note == 62 && n.startTick == 880 && n.endTick == 1280) ok62 = true; // 길이 유지
        if (n.note == 64 && n.startTick == 960 && n.endTick == 1200) ok64 = true; // 그대로
    }
    CHECK(ok60 && ok62 && ok64);

    seq::Track t2;
    t2.addNote(0, 480, 60, 100);
    t2.addNote(400, 400, 62, 90);
    seq::copyMidiRange(t2, 0, 480, 1920); // 복사
    CHECK(seq::extractNotes(t2).size() == 4);
    seq::eraseMidiRange(t2, 0, 480); // 원본 삭제 -> 복사본만 남는다
    const auto n2 = seq::extractNotes(t2);
    CHECK(n2.size() == 2);
    for (const auto& n : n2) CHECK(n.startTick >= 1920);
}

// MIDI 클립 소유 개념: 멤버 노트만 따라다니고, 겹친 남의 노트는 흡수하지 않는다
void testMidiClipOwnership() {
    seq::Track t;
    t.addNote(0, 240, 60, 100);    // 클립 멤버가 될 노트
    t.addNote(1920, 240, 64, 90);  // 목적지에 있는 "남의" 노트
    seq::MidiClip c;
    c.startTick = 0;
    c.endTick = 480;
    seq::adoptMidiClipMembers(t, c);
    CHECK(c.members.size() == 1);

    // 남의 노트 위(1920)로 이동: 박스를 옮긴 뒤 멤버만 이동
    c.startTick = 1920;
    c.endTick = 2400;
    seq::shiftMidiClip(t, c, 0, 480, 1920);
    {
        const auto ns = seq::extractNotes(t);
        CHECK(ns.size() == 2);
        bool ok60 = false, ok64 = false;
        for (const auto& n : ns) {
            if (n.note == 60 && n.startTick == 1920) ok60 = true;
            if (n.note == 64 && n.startTick == 1920) ok64 = true; // 그대로
        }
        CHECK(ok60 && ok64);
    }
    // 다시 이동: 멤버(60)만 가고 남의 노트(64)는 흡수되지 않는다
    c.startTick = 3840;
    c.endTick = 4320;
    seq::shiftMidiClip(t, c, 1920, 2400, 1920);
    {
        const auto ns = seq::extractNotes(t);
        CHECK(ns.size() == 2);
        bool ok60 = false, ok64 = false;
        for (const auto& n : ns) {
            if (n.note == 60 && n.startTick == 3840) ok60 = true;
            if (n.note == 64 && n.startTick == 1920) ok64 = true; // 여전히 제자리!
        }
        CHECK(ok60 && ok64);
    }
    // 복제/삭제도 멤버만
    const seq::MidiClip dup = seq::copyMidiClip(t, c, 480);
    CHECK(dup.members.size() == 1);
    CHECK(seq::extractNotes(t).size() == 3);
    seq::eraseMidiClip(t, dup);
    CHECK(seq::extractNotes(t).size() == 2);
    // 다른 트랙으로 이동 (채널이 바뀐다)
    seq::Track dst;
    dst.channel = 3;
    seq::MidiClip mv = c;
    seq::moveMidiClipToTrack(t, dst, mv, c.startTick, c.endTick, 0);
    CHECK(seq::extractNotes(t).size() == 1);   // 남은 건 남의 노트뿐
    CHECK(seq::extractNotes(dst).size() == 1); // 멤버가 이사왔다

    // 자동 소속: 클립 범위 안에 추가한 노트는 adopt로 소속되고, 지우면 자동 탈퇴
    seq::Track t3;
    seq::MidiClip c3;
    c3.startTick = 0;
    c3.endTick = 480;
    t3.midiClips.push_back(c3);
    t3.addNote(120, 60, 70, 100);
    seq::adoptNoteIntoClips(t3, 70, 120);
    CHECK(t3.midiClips[0].members.size() == 1);
    bool f = false;
    const auto sp = seq::noteSpanAt(t3, 70, 130, f);
    CHECK(f);
    seq::removeNote(t3, sp); // removeNote가 disown까지 한다
    CHECK(t3.midiClips[0].members.empty());
}

// ASCII 타브 파서: 6줄 블록에서 프렛 -> 노트, 열 -> 시간
void testTabImport() {
    const std::string tab =
        "e|--0--3--|\n"
        "B|--1-----|\n"
        "G|--------|\n"
        "D|--------|\n"
        "A|--------|\n"
        "E|--0-----|\n";
    const auto ns = seq::parseAsciiTab(tab, 120); // 한 칸 = 120틱
    CHECK(ns.size() == 4);
    // 열 2 (틱 240): e줄 0프렛=64, B줄 1프렛=60, E줄 0프렛=40
    int at240 = 0;
    bool has64 = false, has60 = false, has40 = false, has67 = false;
    for (const auto& n : ns) {
        if (n.tick == 240) {
            ++at240;
            if (n.note == 64) has64 = true;
            if (n.note == 60) has60 = true;
            if (n.note == 40) has40 = true;
        }
        if (n.tick == 600 && n.note == 67) has67 = true; // 열 5: e줄 3프렛
    }
    CHECK(at240 == 3 && has64 && has60 && has40 && has67);

    // 두 자리 프렛 + 마디선 열은 시간을 진행시키지 않는다
    const std::string tab2 =
        "e|-12-|-0-|\n"
        "B|----|---|\n"
        "G|----|---|\n"
        "D|----|---|\n"
        "A|----|---|\n"
        "E|----|---|\n";
    const auto n2 = seq::parseAsciiTab(tab2, 100);
    CHECK(n2.size() == 2);
    CHECK(n2[0].note == 64 + 12 && n2[0].tick == 100); // 12프렛 = E5(76)
    // '|' 열과 두 자리 프렛의 뒷자리('2')는 시각을 진행시키지 않는다
    CHECK(n2[1].note == 64 && n2[1].tick == 400);
}

// ── 테스트용 타브 PDF를 직접 "조판"한다 (비압축/비암호화) ──
// 페이지 200pt 높이. PDF y = 100,95,...,75 에 가로줄 6개 -> 위(y=100)가 1번줄 e.
// 진짜 악보처럼 가로 위치를 시간에 비례해 놓는다 (파서가 간격도 자로 쓰기 때문).
namespace tabpdf {
constexpr double kTopY = 100.0, kBotY = 75.0; // 보표 위/아래 줄 (PDF 좌표)
constexpr double kMusicX0 = 30.0, kBarX0 = 25.0, kBarX1 = 195.0;
constexpr int kOpenStr[6] = {64, 59, 55, 50, 45, 40}; // e B G D A E

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}
// 프렛 숫자 + 기둥. stem=false면 기둥 없는 음(온음표 등).
std::string note(double x, int str, int fret, bool stem) {
    const double lineY = kTopY - 5.0 * str;
    std::string s = "BT /F1 6 Tf 1 0 0 1 " + num(x) + " " + num(lineY - 2.0) + " Tm (" +
                    std::to_string(fret) + ") Tj ET\n";
    if (stem) {
        const double sx = x + 7.0;
        s += num(sx) + " 78 m " + num(sx) + " 68 l S\n"; // 보표 아래로 뻗는 기둥
    }
    return s;
}
// 빔: 기둥 끝(level 0)에서 위로 쌓인다. 기둥 x = 음 x + 7.
std::string beam(double x0, double x1, int level) {
    const double y = 66.0 + level * 3.0;
    return num(x0 + 7.0) + " " + num(y) + " " + num(x1 - x0) + " 1.5 re f\n";
}
std::string barline(double x) {
    return num(x) + " 100 m " + num(x) + " 75 l S\n";
}
// 도돌이표: 굵은 세로줄(w=2.3) + 점 두 개. 점이 오른쪽이면 시작(‖:), 왼쪽이면 끝(:‖).
std::string repeatBar(double x, bool start) {
    const double dx = start ? x + 5.0 : x - 6.0;
    std::string s = "q 2.3 w " + num(x) + " 100 m " + num(x) + " 75 l S Q\n";
    s += "BT /F1 8 Tf 1 0 0 1 " + num(dx) + " 90 Tm (.) Tj ET\n";
    s += "BT /F1 8 Tf 1 0 0 1 " + num(dx) + " 85 Tm (.) Tj ET\n";
    return s;
}
// 박자표: 보표 앞머리에 크게 (프렛 숫자보다 훨씬 큼)
std::string timeSig(const char* n, const char* d) {
    return std::string("BT /F1 18 Tf 1 0 0 1 25 87 Tm (") + n + ") Tj ET\n" +
           "BT /F1 18 Tf 1 0 0 1 25 74 Tm (" + d + ") Tj ET\n";
}
// 마디 안에서 틱 -> x (시간에 비례). barTicks = 한 마디 틱 수.
double px(uint32_t tick, uint32_t barTicks) {
    return kMusicX0 + (kBarX1 - 5.0 - kMusicX0) * (double)tick / (double)barTicks;
}
} // namespace tabpdf

// 보표 6줄만 그린 PDF (음표는 extra로 직접 조판)
std::string makeStaffPdf(const std::string& extra) {
    std::string content;
    for (int i = 0; i < 6; ++i) {
        const int y = 100 - i * 5;
        content += "20 " + std::to_string(y) + " m 200 " + std::to_string(y) + " l S\n";
    }
    content += extra;

    std::string pdf = "%PDF-1.4\n";
    pdf += "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n";
    pdf += "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n";
    pdf += "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]"
           "/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>endobj\n";
    pdf += "4 0 obj<<>>stream\n" + content + "endstream endobj\n";
    pdf += "5 0 obj<</Type/Font/Subtype/TrueType/BaseFont/Helvetica"
           "/Encoding/WinAnsiEncoding>>endobj\n";
    pdf += "trailer<</Root 1 0 R>>\n%%EOF\n";
    return pdf;
}

std::string makeTabPdf(const std::string& extra) {
    std::string content;
    for (int i = 0; i < 6; ++i) {
        const int y = 100 - i * 5;
        content += "20 " + std::to_string(y) + " m 200 " + std::to_string(y) + " l S\n";
    }
    // 글자 중심 = 베이스라인 - 0.36*크기 이므로 줄 y에 맞춰 베이스라인을 잡는다
    content += "BT /F1 6 Tf 1 0 0 1 50 98 Tm (5) Tj ET\n";   // e줄 5프렛 = 69
    content += "BT /F1 6 Tf 1 0 0 1 100 73 Tm (3) Tj ET\n";  // E줄 3프렛 = 43
    content += "BT /F1 6 Tf 1 0 0 1 150 88 Tm (12) Tj ET\n"; // G줄 12프렛 = 67
    content += extra;

    std::string pdf = "%PDF-1.4\n";
    pdf += "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n";
    pdf += "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n";
    pdf += "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]"
           "/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>endobj\n";
    pdf += "4 0 obj<<>>stream\n" + content + "endstream endobj\n";
    pdf += "5 0 obj<</Type/Font/Subtype/TrueType/BaseFont/Helvetica"
           "/Encoding/WinAnsiEncoding>>endobj\n";
    pdf += "trailer<</Root 1 0 R>>\n%%EOF\n";
    return pdf;
}

midipro::pdf::TabPdfResult runTabPdf(const std::string& pdf, uint32_t ppqn) {
    const char* path = "test_pdftab_tmp.pdf";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(pdf.data(), (std::streamsize)pdf.size());
    }
    const auto r = midipro::pdf::extractTabFromPdf(path, ppqn);
    std::remove(path);
    return r;
}

// 조판 PDF -> 타브: 가로줄 6개 = 보표, 숫자 좌표 -> 현 배정
void testPdfTab() {
    const auto r = runTabPdf(makeTabPdf(""), 480);

    CHECK(r.error.empty());
    CHECK(r.pages == 1);
    CHECK(r.tabStaves == 1);
    CHECK(r.parts.size() == 1);
    CHECK(!r.hasRhythm); // 기둥이 없으니 리듬은 못 읽는다
    if (r.parts.empty()) return;

    // 만들어진 ASCII 타브를 그대로 파서에 먹여 본다 (전 과정 왕복 검증)
    const auto ns = seq::parseAsciiTab(r.parts[0].ascii, 120);
    CHECK(ns.size() == 3);
    if (ns.size() != 3) return;
    CHECK(ns[0].note == 69); // x=50  : e줄 5프렛
    CHECK(ns[1].note == 43); // x=100 : E줄 3프렛
    CHECK(ns[2].note == 67); // x=150 : G줄 12프렛 (두 자리)
    CHECK(ns[0].tick < ns[1].tick && ns[1].tick < ns[2].tick);
}

// 기둥(stem)과 빔(beam)으로 음길이를 읽는다: 빔 0개=4분, 1개=8분, 2개=16분.
// 한 마디(4/4)를 4분 + 8분 + 8분 + 2분 으로 채운다.
void testPdfTabRhythm() {
    using namespace tabpdf;
    const uint32_t PPQN = 480, BAR = PPQN * 4;
    const double xq = px(0, BAR), xe1 = px(PPQN, BAR), xe2 = px(PPQN * 3 / 2, BAR),
                 xh = px(PPQN * 2, BAR);

    std::string e = barline(kBarX0);
    e += note(xq, 0, 5, true);   // 4분 (e줄 5프렛 = 69), 빔 없음
    e += note(xe1, 5, 3, true);  // 8분 (E줄 3프렛 = 43)
    e += note(xe2, 2, 12, true); // 8분 (G줄 12프렛 = 67)
    e += beam(xe1, xe2, 0);      // 두 8분을 잇는 빔 1개
    e += note(xh, 4, 7, true);   // 2분 (A줄 7프렛 = 52), 빔 없음 -> 간격으로 판단
    e += barline(kBarX1);

    const auto r = runTabPdf(makeStaffPdf(e), PPQN);
    CHECK(r.error.empty());
    CHECK(r.tabStaves == 1);
    CHECK(r.hasRhythm);
    if (r.parts.empty() || !r.hasRhythm) return;

    const auto& ns = r.parts[0].notes;
    CHECK(ns.size() == 4);
    if (ns.size() != 4) return;
    CHECK(ns[0].note == 69 && ns[0].tick == 0 && ns[0].durTicks == PPQN);
    CHECK(ns[1].note == 43 && ns[1].tick == PPQN && ns[1].durTicks == PPQN / 2);
    CHECK(ns[2].note == 67 && ns[2].tick == PPQN * 3 / 2 && ns[2].durTicks == PPQN / 2);
    CHECK(ns[3].note == 52 && ns[3].tick == PPQN * 2 && ns[3].durTicks == PPQN * 2);
    // 악보의 줄(현) 정보가 그대로 온다 (타브 창이 원래 운지로 표시하는 근거)
    CHECK(ns[0].strIdx == 0); // e줄
    CHECK(ns[1].strIdx == 5); // E줄
    CHECK(ns[2].strIdx == 2); // G줄
    CHECK(ns[3].strIdx == 4); // A줄
}

// 빔이 없는 음(깃발 달린 홑 8분)은 빔 개수로 구분할 수 없다 -> 가로 간격으로 재야 한다.
// 빔이 하나도 없는 마디: 8분 + 8분 + 4분 + 2분.
void testPdfTabFlagNote() {
    using namespace tabpdf;
    const uint32_t PPQN = 480, BAR = PPQN * 4;

    std::string e = barline(kBarX0);
    e += note(px(0, BAR), 0, 1, true);            // 8분 (65)
    e += note(px(PPQN / 2, BAR), 0, 2, true);     // 8분 (66)
    e += note(px(PPQN, BAR), 0, 3, true);         // 4분 (67)
    e += note(px(PPQN * 2, BAR), 0, 4, true);     // 2분 (68)
    e += barline(kBarX1);

    const auto r = runTabPdf(makeStaffPdf(e), PPQN);
    CHECK(r.hasRhythm);
    if (r.parts.empty()) return;
    const auto& ns = r.parts[0].notes;
    CHECK(ns.size() == 4);
    if (ns.size() != 4) return;
    CHECK(ns[0].note == 65 && ns[0].tick == 0 && ns[0].durTicks == PPQN / 2);
    CHECK(ns[1].note == 66 && ns[1].tick == PPQN / 2 && ns[1].durTicks == PPQN / 2);
    CHECK(ns[2].note == 67 && ns[2].tick == PPQN && ns[2].durTicks == PPQN);
    CHECK(ns[3].note == 68 && ns[3].tick == PPQN * 2 && ns[3].durTicks == PPQN * 2);
}

// 도돌이표(‖: ... :‖)를 펼친다: 구간을 두 번 연주하도록 노트를 복제한다.
void testPdfTabRepeat() {
    using namespace tabpdf;
    const uint32_t PPQN = 480, BAR = PPQN * 4;

    // 2마디: [25(‖:) .. 110 .. 195(:‖)]. 두 마디 전체가 반복 구간이다.
    std::string e = repeatBar(25, true); // 반복 시작
    e += note(33, 0, 5, true);           // 1마디: e줄 5프렛 = 69
    e += note(75, 0, 7, true);           //        e줄 7프렛 = 71
    e += barline(110);
    e += note(118, 5, 3, true); // 2마디: E줄 3프렛 = 43
    e += note(160, 5, 5, true); //        E줄 5프렛 = 45
    e += repeatBar(195, false); // 반복 끝

    const auto r = runTabPdf(makeStaffPdf(e), PPQN);
    CHECK(r.error.empty());
    CHECK(r.hasRhythm);
    CHECK(r.repeats == 1);
    if (r.parts.empty()) return;
    const auto& ns = r.parts[0].notes;

    // 4개 음이 두 번씩 = 8개. 두 번째 바퀴는 정확히 2마디(=BAR*2) 뒤에 온다.
    CHECK(ns.size() == 8);
    auto has = [&](uint32_t tick, uint8_t note) {
        for (const auto& n : ns)
            if (n.tick == tick && n.note == note) return true;
        return false;
    };
    CHECK(has(0, 69));            // 1바퀴 1마디
    CHECK(has(BAR, 43));          // 1바퀴 2마디 (마디선에서 시각 재동기화)
    CHECK(has(BAR * 2, 69));      // 2바퀴 1마디
    CHECK(has(BAR * 3, 43));      // 2바퀴 2마디
}

// 박자표(3/4, 6/8)를 읽어 마디 길이를 바꾼다.
// 마디선을 지나면 그 마디의 정확한 시각으로 되돌리므로, 마디 길이가 곧바로 드러난다.
void testPdfTabTimeSig() {
    using namespace tabpdf;
    const uint32_t PPQN = 480;

    // 1마디를 x 25~130 에 두고, 2마디 첫 음을 x=140 에 둔다.
    // 그 음의 시각이 곧 "한 마디의 길이"다: 4/4=1920, 3/4=1440, 6/8=1440.
    auto build = [&](const std::string& sig) {
        std::string e = sig;
        e += barline(25);
        e += note(40, 0, 5, true); // 1마디
        e += note(70, 0, 7, true);
        e += note(100, 0, 9, true);
        e += barline(130);          // 1마디 끝
        e += note(140, 5, 3, true); // 2마디 첫 음 — 이 음의 시각이 곧 마디 길이
        e += note(170, 5, 5, true);
        e += barline(195); // 2마디 끝
        return makeStaffPdf(e);
    };

    // 2마디 첫 음 = E줄 3프렛 = 43. 그 시각이 곧 한 마디의 길이다.
    auto bar2Tick = [](const midipro::pdf::TabPdfResult& r) -> uint32_t {
        if (r.parts.empty()) return 0;
        for (const auto& n : r.parts[0].notes)
            if (n.note == 43) return n.tick;
        return 0;
    };
    {
        const auto r = runTabPdf(build(""), PPQN); // 박자표 없음 = 4/4
        CHECK(r.hasRhythm);
        CHECK(bar2Tick(r) == PPQN * 4);
    }
    {
        const auto r = runTabPdf(build(timeSig("3", "4")), PPQN);
        CHECK(r.timeSigNum == 3 && r.timeSigDen == 4);
        CHECK(r.hasRhythm);
        // 박자표 숫자가 프렛으로 새어들지 않았다 (음 5개 그대로)
        if (!r.parts.empty()) CHECK(r.parts[0].notes.size() == 5);
        CHECK(bar2Tick(r) == PPQN * 3); // 3/4: 한 마디 = 3박
    }
    {
        const auto r = runTabPdf(build(timeSig("6", "8")), PPQN);
        CHECK(r.timeSigNum == 6 && r.timeSigDen == 8);
        if (!r.parts.empty()) CHECK(r.parts[0].notes.size() == 5);
        CHECK(bar2Tick(r) == PPQN * 3); // 6/8: 8분 6개 = 3박
    }
}

void testNoteExtraction() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    t.addNote(480, 240, 64, 90);
    t.sortEvents();

    const auto notes = seq::extractNotes(t);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60 && notes[0].startTick == 0 && notes[0].endTick == 480);
    CHECK(notes[1].note == 64 && notes[1].startTick == 480 && notes[1].endTick == 720);
    CHECK(t.lengthTicks() == 720);
}

void testNoteEditing() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    t.addNote(480, 480, 64, 100);
    t.addNote(480, 240, 67, 90); // 64와 같은 시작, 다른 음
    t.sortEvents();
    CHECK(seq::extractNotes(t).size() == 3);

    // (60, tick 100)을 포함하는 노트를 찾아 삭제
    bool found = false;
    const seq::NoteSpan hit = seq::noteSpanAt(t, 60, 100, found);
    CHECK(found);
    CHECK(hit.startTick == 0 && hit.endTick == 480);
    CHECK(seq::removeNote(t, hit));
    CHECK(seq::extractNotes(t).size() == 2);

    // 삭제된 자리에는 더 이상 노트가 없다
    bool found2 = false;
    seq::noteSpanAt(t, 60, 100, found2);
    CHECK(!found2);

    // 같은 시작 틱의 다른 음(67)은 그대로 남아 있다
    bool found3 = false;
    seq::noteSpanAt(t, 67, 500, found3);
    CHECK(found3);
}

void testSetNoteVelocity() {
    seq::Track t;
    t.addNote(0, 480, 60, 100);
    t.addNote(480, 480, 60, 90); // 같은 음, 다른 시작
    t.sortEvents();

    bool found = false;
    const seq::NoteSpan first = seq::noteSpanAt(t, 60, 0, found);
    CHECK(found);
    CHECK(seq::setNoteVelocity(t, first, 40));

    const auto notes = seq::extractNotes(t);
    CHECK(notes.size() == 2);
    // 첫 노트만 바뀌고, 같은 음의 두 번째 노트는 그대로다
    CHECK(notes[0].velocity == 40);
    CHECK(notes[1].velocity == 90);

    // 범위 제한: 0은 Note Off 의미라 1로, 127 초과는 127로
    CHECK(seq::setNoteVelocity(t, first, 0));
    CHECK(seq::extractNotes(t)[0].velocity == 1);

    // 없는 노트는 false
    seq::NoteSpan none;
    none.note = 72;
    none.startTick = 999;
    CHECK(!seq::setNoteVelocity(t, none, 80));
}

void testQuantize() {
    seq::Track t;
    t.addNote(10, 100, 60, 100);   // 0에 가까움 -> 0
    t.addNote(130, 100, 62, 90);   // 120에 가까움 -> 120
    t.addNote(179, 100, 64, 80);   // 반올림 경계: 179+60=239/120=1 -> 120
    t.addNote(240, 100, 65, 70);   // 이미 격자 위 -> 그대로
    t.sortEvents();

    const int changed = seq::quantizeTrack(t, 120); // 16분음표(ppqn 480 기준) 격자
    CHECK(changed == 3);

    const auto notes = seq::extractNotes(t);
    CHECK(notes.size() == 4);
    CHECK(notes[0].startTick == 0 && notes[0].note == 60);
    CHECK(notes[1].startTick == 120 && notes[1].note == 62);
    CHECK(notes[2].startTick == 120 && notes[2].note == 64);
    CHECK(notes[3].startTick == 240 && notes[3].note == 65);
    // 길이와 세기는 유지된다
    CHECK(notes[0].endTick - notes[0].startTick == 100);
    CHECK(notes[1].velocity == 90);

    // 다시 돌리면 바뀔 게 없다 (멱등)
    CHECK(seq::quantizeTrack(t, 120) == 0);
    // 격자 0은 무시
    CHECK(seq::quantizeTrack(t, 0) == 0);
}

void testVlq() {
    struct Case {
        uint32_t value;
        std::size_t bytes;
    };
    const Case cases[] = {{0, 1}, {127, 1}, {128, 2}, {8192, 2}, {0x0FFFFFFF, 4}};
    for (const auto& c : cases) {
        std::vector<uint8_t> buf;
        seq::smf::writeVlq(buf, c.value);
        CHECK(buf.size() == c.bytes);
        uint32_t decoded = 0;
        const std::size_t consumed = seq::smf::readVlq(buf.data(), buf.size(), 0, decoded);
        CHECK(consumed == c.bytes);
        CHECK(decoded == c.value);
    }
}

void testSmfRoundTrip() {
    seq::Song song;
    song.bpm = 100.0;
    song.ppqn = 480;

    seq::Track t1;
    t1.name = "Lead";
    t1.channel = 0;
    t1.addProgramChange(0, guitar::kStringCount); // 임의 프로그램
    t1.addNote(0, 480, 60, 100);
    t1.addNote(480, 480, 67, 90);
    t1.sortEvents();
    song.tracks.push_back(t1);

    seq::Track t2;
    t2.name = "Bass";
    t2.channel = 1;
    t2.addNote(0, 960, 40, 110);
    t2.sortEvents();
    song.tracks.push_back(t2);

    const auto path =
        std::filesystem::temp_directory_path() / "midipro_roundtrip_test.mid";
    CHECK(seq::smf::save(song, path));

    seq::Song loaded;
    CHECK(seq::smf::load(loaded, path));
    CHECK(loaded.ppqn == 480);
    CHECK(std::fabs(loaded.bpm - 100.0) < 0.5); // 템포는 마이크로초 반올림 오차 허용
    CHECK(loaded.tracks.size() == 2);

    // 첫 트랙의 노트가 보존됐는지 (Program Change 포함)
    const auto notes = seq::extractNotes(loaded.tracks[0]);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60);
    CHECK(notes[1].note == 67);
    CHECK(loaded.tracks[0].name == "Lead");
    CHECK(loaded.tracks[1].name == "Bass");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// .mid 파일에 템포 맵(곡 중간 템포 변경)이 보존되는지
void testSmfTempoMap() {
    seq::Song song;
    song.ppqn = 480;
    song.bpm = 120.0;
    song.tempoChanges.push_back({1920, 60.0});  // 2마디부터 60
    song.tempoChanges.push_back({3840, 150.0}); // 3마디부터 150
    seq::Track t;
    t.name = "T";
    t.addNote(0, 480, 60, 100);
    t.sortEvents();
    song.tracks.push_back(t);

    const auto path = std::filesystem::temp_directory_path() / "midipro_tempomap_test.mid";
    CHECK(seq::smf::save(song, path));
    seq::Song loaded;
    CHECK(seq::smf::load(loaded, path));
    CHECK(std::fabs(loaded.bpm - 120.0) < 0.5);
    CHECK(loaded.tempoChanges.size() == 2);
    if (loaded.tempoChanges.size() == 2) {
        CHECK(loaded.tempoChanges[0].tick == 1920);
        CHECK(std::fabs(loaded.tempoChanges[0].bpm - 60.0) < 0.5);
        CHECK(loaded.tempoChanges[1].tick == 3840);
        CHECK(std::fabs(loaded.tempoChanges[1].bpm - 150.0) < 0.5);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// 오토메이션 값 보간: 빈 목록=페이더 값, 점 이전/이후=끝값 유지, 중간=선형
void testAutomation() {
    std::vector<seq::Track::AutoPoint> pts;
    CHECK(approx(seq::autoValueAt(pts, 100, 0.7f), 0.7));
    pts.push_back({100, 1.0f});
    pts.push_back({200, 0.0f});
    CHECK(approx(seq::autoValueAt(pts, 50, 0.7f), 1.0));  // 첫 점 이전 = 첫 값
    CHECK(approx(seq::autoValueAt(pts, 150, 0.7f), 0.5)); // 중간 = 선형 보간
    CHECK(approx(seq::autoValueAt(pts, 175, 0.7f), 0.25));
    CHECK(approx(seq::autoValueAt(pts, 300, 0.7f), 0.0)); // 마지막 이후 = 끝 값
}

// 휴머나이즈: 흔들림 범위 준수 + 결정성(같은 seed = 같은 결과) + 0이면 무변화
void testHumanize() {
    seq::Track t;
    for (int i = 0; i < 8; ++i) t.addNote((uint32_t)i * 480, 240, 60, 100);
    t.sortEvents();

    seq::Track a = t, b = t;
    CHECK(seq::humanizeTrack(a, 0, 0, 7) == 0); // 강도 0 = 아무것도 안 바뀜

    const int ca = seq::humanizeTrack(a, 20, 10, 12345);
    CHECK(ca > 0);
    const auto notes = seq::extractNotes(a);
    CHECK(notes.size() == 8); // 노트 수 보존
    for (const auto& n : notes) {
        // 원본 격자(480 배수)에서 ±20틱 이내
        const uint32_t nearest = ((n.startTick + 240) / 480) * 480;
        const int64_t d = (int64_t)n.startTick - (int64_t)nearest;
        CHECK(d >= -20 && d <= 20);
        CHECK(n.velocity >= 90 && n.velocity <= 110); // 100 ± 10
        CHECK(n.endTick - n.startTick == 240);        // 길이 유지
    }
    // 같은 seed면 같은 결과 (결정적)
    seq::humanizeTrack(b, 20, 10, 12345);
    const auto na = seq::extractNotes(a), nb = seq::extractNotes(b);
    CHECK(na.size() == nb.size());
    for (std::size_t k = 0; k < na.size() && k < nb.size(); ++k) {
        CHECK(na[k].startTick == nb[k].startTick);
        CHECK(na[k].velocity == nb[k].velocity);
    }
}

void testFretboard() {
    // 6번줄(인덱스0) 개방 = E2(40), 5프렛 = A2(45)
    CHECK(guitar::noteAt(0, 0) == 40);
    CHECK(guitar::noteAt(0, 5) == 45);
    // 1번줄(인덱스5) 개방 = E4(64)
    CHECK(guitar::noteAt(5, 0) == 64);

    // A2(45)는 6번줄 5프렛과 5번줄 0프렛에서 난다
    const auto pos = guitar::positionsForNote(45);
    bool found6th5 = false, found5th0 = false;
    for (const auto& p : pos) {
        if (p.stringIndex == 0 && p.fret == 5) found6th5 = true;
        if (p.stringIndex == 1 && p.fret == 0) found5th0 = true;
    }
    CHECK(found6th5 && found5th0);

    // 코드 공식이 비어있지 않다
    CHECK(!guitar::commonChords().empty());
    CHECK(std::string(guitar::pitchClassName(0)) == "C");
    CHECK(std::string(guitar::pitchClassName(9)) == "A");
}

// ── 코드 찾기: 조성 판별 · 다이어토닉 코드 · 마디별 추천 ──
void testChordFinder() {
    using namespace seq;
    auto mk = [](uint8_t note, uint32_t start, uint32_t dur) {
        return MelNote{note, start, dur};
    };
    const uint32_t Q = 480; // 4분음표 = 480틱

    // [1] 조성 판별: C장조 음계(C D E F G A B, 다 같은 길이) → C장조
    {
        std::vector<MelNote> mel;
        const int cmaj[] = {60, 62, 64, 65, 67, 69, 71};
        for (int i = 0; i < 7; ++i) mel.push_back(mk((uint8_t)cmaj[i], (uint32_t)i * Q, Q));
        const MusicKey k = detectKey(mel);
        CHECK(k.root == 0 && !k.minor); // C장조
        CHECK(keyName(k) == "C");
    }
    // 조성 판별: A단조 성격(근음 A·3도 C·5도 E를 길게) → A단조
    {
        std::vector<MelNote> mel = {mk(69, 0, Q * 2), mk(72, Q * 2, Q * 2),
                                    mk(76, Q * 4, Q * 2), mk(69, Q * 6, Q * 2),
                                    mk(71, Q * 8, Q / 2),  mk(74, Q * 8 + Q / 2, Q / 2)};
        const MusicKey k = detectKey(mel);
        CHECK(k.root == 9 && k.minor); // A단조
        CHECK(keyName(k) == "Am");
    }

    // [2] 다이어토닉 코드: C장조 → C Dm Em F G Am Bdim
    {
        const auto ch = diatonicChords({0, false});
        CHECK(chordName(ch[0]) == "C");
        CHECK(chordName(ch[1]) == "Dm");
        CHECK(chordName(ch[2]) == "Em");
        CHECK(chordName(ch[3]) == "F");
        CHECK(chordName(ch[4]) == "G");
        CHECK(chordName(ch[5]) == "Am");
        CHECK(chordName(ch[6]) == "Bdim");
        // C 구성음 = {0,4,7}
        CHECK(ch[0].pcs[0] == 0 && ch[0].pcs[1] == 4 && ch[0].pcs[2] == 7);
    }
    // 다이어토닉: A단조 → Am Bdim C Dm Em F G
    {
        const auto ch = diatonicChords({9, true});
        CHECK(chordName(ch[0]) == "Am");
        CHECK(chordName(ch[1]) == "Bdim");
        CHECK(chordName(ch[2]) == "C");
        CHECK(chordName(ch[3]) == "Dm");
        CHECK(chordName(ch[4]) == "Em");
    }

    // [3] 마디별 추천: C장조, 4/4(한 마디 4Q=1920틱).
    // 1마디 = C코드 음(C·E·G), 2마디 = G코드 음(G·B·D) → C, G
    {
        const uint32_t BAR = Q * 4;
        std::vector<MelNote> mel = {
            mk(60, 0, Q), mk(64, Q, Q), mk(67, Q * 2, Q), mk(72, Q * 3, Q),     // C
            mk(67, BAR, Q), mk(71, BAR + Q, Q), mk(74, BAR + Q * 2, Q),          // G
            mk(67, BAR + Q * 3, Q)};
        ChordRecoOptions opt;
        opt.ticksPerBar = BAR;
        opt.smooth = false; // 스무딩 없이 원 판정 확인
        const auto rec = recommendChords(mel, {0, false}, opt);
        CHECK(rec.size() == 2);
        if (rec.size() == 2) {
            CHECK(chordName(rec[0].chord) == "C");
            CHECK(chordName(rec[1].chord) == "G");
        }
    }
    // 짧은 경과음 제외 옵션: 코드음(길게) + 코드밖 음(아주 짧게) → 코드음 코드
    {
        const uint32_t BAR = Q * 4;
        std::vector<MelNote> mel = {mk(65, 0, Q * 3),        // F를 길게
                                    mk(60, Q * 3, Q / 8)};   // C를 아주 짧게(경과음)
        ChordRecoOptions opt;
        opt.ticksPerBar = BAR;
        opt.minNoteTicks = Q / 4; // 16분음표 미만 제외
        const auto rec = recommendChords(mel, {0, false}, opt);
        CHECK(rec.size() == 1);
        if (!rec.empty()) CHECK(chordName(rec[0].chord) == "F");
    }

    // 빈 입력은 안전하게 C장조 + 코드 없음
    {
        const MusicKey k = detectKey({});
        CHECK(k.root == 0 && !k.minor);
        ChordRecoOptions opt;
        CHECK(recommendChords({}, k, opt).empty());
    }
}

// ── 드럼 패턴 자동 생성 ──
void testDrumPattern() {
    using namespace seq;
    const uint32_t PPQN = 480;

    auto count = [](const std::vector<DrumHit>& hits, uint8_t note) {
        int c = 0;
        for (const auto& h : hits) if (h.note == note) ++c;
        return c;
    };
    auto hasAt = [](const std::vector<DrumHit>& hits, uint32_t tick, uint8_t note) {
        for (const auto& h : hits)
            if (h.tick == tick && h.note == note) return true;
        return false;
    };

    // 4/4 기본: 1마디 → 킥 2(1·3박), 스네어 2(2·4박), 햇 8개, 크래시 1
    {
        const auto p = generateDrumPattern(kSig44, kStyleBasic, PPQN, 1);
        CHECK(count(p, kDrumKick) == 2);
        CHECK(count(p, kDrumSnare) == 2);
        CHECK(count(p, kDrumHatClosed) == 8);
        CHECK(count(p, kDrumCrash) == 1);
        // 킥은 1박(0), 3박(2*PPQN); 스네어는 2박(PPQN), 4박(3*PPQN)
        CHECK(hasAt(p, 0, kDrumKick));
        CHECK(hasAt(p, 2 * PPQN, kDrumKick));
        CHECK(hasAt(p, PPQN, kDrumSnare));
        CHECK(hasAt(p, 3 * PPQN, kDrumSnare));
    }
    // 4/4 2마디: 마디마다 반복되고 startTick 오프셋이 붙는다
    {
        const uint32_t bar = 4 * PPQN;
        const auto p = generateDrumPattern(kSig44, kStyleBasic, PPQN, 2, bar);
        CHECK(count(p, kDrumSnare) == 4); // 2마디 × 2
        CHECK(hasAt(p, bar + PPQN, kDrumSnare));       // 2마디째 2박
        CHECK(hasAt(p, bar + 3 * PPQN, kDrumSnare));   // 2마디째 4박
        // 크래시는 채우기 첫 마디에만 (2마디여도 1개)
        CHECK(count(p, kDrumCrash) == 1);
    }
    // 3/4: 한 마디 6개 8분. 킥 1박, 스네어 2·3박
    {
        const auto p = generateDrumPattern(kSig34, kStyleBasic, PPQN, 1);
        CHECK(count(p, kDrumKick) == 1);
        CHECK(count(p, kDrumSnare) == 2);
        CHECK(hasAt(p, 0, kDrumKick));
        CHECK(hasAt(p, PPQN, kDrumSnare));     // 2박
        CHECK(hasAt(p, 2 * PPQN, kDrumSnare)); // 3박
    }
    // 6/8: 겹박자, 8분 6개. 킥 첫 8분·네 번째 8분, 스네어 네 번째
    {
        const auto p = generateDrumPattern(kSig68, kStyleBasic, PPQN, 1);
        CHECK(count(p, kDrumHatClosed) == 6);
        CHECK(hasAt(p, 0, kDrumKick));
        CHECK(hasAt(p, 3 * (PPQN / 2), kDrumKick));  // 4번째 8분
        CHECK(hasAt(p, 3 * (PPQN / 2), kDrumSnare));
    }
    // 발라드는 크래시 없이 성글게 (햇은 있으나 킥 1개)
    {
        const auto p = generateDrumPattern(kSig44, kStyleBallad, PPQN, 1);
        CHECK(count(p, kDrumCrash) == 0);
        CHECK(count(p, kDrumKick) == 1);
    }
    // 빈/무효 입력은 빈 결과
    CHECK(generateDrumPattern(kSig44, kStyleBasic, 0, 1).empty());
    CHECK(generateDrumPattern(kSig44, kStyleBasic, PPQN, 0).empty());
}

} // namespace

int main() {
    testTimeBase();
    testTempoMap();
    testTempoRamp();
    testMidiRangeOps();
    testMidiClipOwnership();
    testTabImport();
    testPdfTab();
    testPdfTabRhythm();
    testPdfTabFlagNote();
    testPdfTabRepeat();
    testPdfTabTimeSig();
    testSmfTempoMap();
    testAutomation();
    testHumanize();
    testNoteExtraction();
    testNoteEditing();
    testSetNoteVelocity();
    testQuantize();
    testVlq();
    testSmfRoundTrip();
    testFretboard();
    testChordFinder();
    testDrumPattern();

    if (g_failures == 0) {
        std::cout << "[OK] sequencer tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
