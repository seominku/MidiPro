// =============================================================
// MidiPro - tests/test_project.cpp
// 프로젝트 통째 저장 왕복 테스트 (Rule 6):
//   곡 + 신스 음색 + 매핑 + VST 참조 + MPE가 보존되는지.
// =============================================================

#include "audio/AudioClip.h"
#include "audio/Mp3Writer.h"
#include "audio/WavFile.h"
#include "project/Project.h"

#include <cmath>
#include <fstream>
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

void testRoundTrip() {
    project::ProjectData p;
    p.song.bpm = 132.0;
    p.song.ppqn = 480;
    seq::Track t1;
    t1.name = "Lead Guitar";
    t1.channel = 2;
    t1.muted = true;
    t1.addNote(0, 480, 60, 100);
    t1.addNote(480, 240, 67, 90);
    t1.sortEvents();
    p.song.tracks.push_back(t1);

    p.synth.waveform = audio::Waveform::Square;
    p.synth.filterCutoff = 3456.0f;
    p.synth.masterVolume = 0.22f;
    p.mpe = true;
    p.midiMap.bind(74, 0, mapping::ParamTarget::FilterCutoff);
    p.vstInstrumentPath = "C:\\Plugins\\Surge XT.vst3";
    p.vstInstrumentClass = 0;
    p.vstInstrumentTrack = 3;

    const std::string text = project::serialize(p);
    project::ProjectData q;
    CHECK(project::deserialize(q, text));

    // 곡
    CHECK(std::fabs(q.song.bpm - 132.0) < 1e-6);
    CHECK(q.song.ppqn == 480);
    CHECK(q.song.tracks.size() == 1);
    CHECK(q.song.tracks[0].name == "Lead Guitar"); // 공백 포함 이름 보존
    CHECK(q.song.tracks[0].channel == 2);
    CHECK(q.song.tracks[0].muted == true);
    const auto notes = seq::extractNotes(q.song.tracks[0]);
    CHECK(notes.size() == 2);
    CHECK(notes[0].note == 60 && notes[1].note == 67);

    // 신스
    CHECK(q.synth.waveform == audio::Waveform::Square);
    CHECK(std::fabs(q.synth.filterCutoff - 3456.0f) < 0.1f);
    CHECK(std::fabs(q.synth.masterVolume - 0.22f) < 1e-4);

    // 매핑 / MPE / VST
    CHECK(q.mpe == true);
    CHECK(q.midiMap.ccForTarget(mapping::ParamTarget::FilterCutoff) == 74);
    CHECK(q.vstInstrumentPath == "C:\\Plugins\\Surge XT.vst3");
    CHECK(q.vstInstrumentTrack == 3); // VSTi 출력 트랙 라우팅 보존
    CHECK(q.vstEffectPath.empty());

    // 우리 포맷이 아니면 거부
    project::ProjectData bad;
    CHECK(!project::deserialize(bad, "hello not a project"));
}

// WAV 인코딩/디코딩 왕복: 샘플 값과 포맷이 보존되어야 한다.
void testWavRoundTrip() {
    audio::AudioClip clip;
    clip.name = "rec";
    clip.channels = 2;
    clip.sampleRate = 48000;
    clip.pcm = {0.0f, -1.0f, 0.5f, 0.25f, -0.75f, 1.0f}; // 3프레임 x 2채널
    clip.trimStart = 0;
    clip.trimLen = 3;

    const auto bytes = audio::encodeWav(clip);
    CHECK(bytes.size() > 44);

    auto back = audio::decodeWav(bytes.data(), bytes.size(), "rec2");
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(back->channels == 2);
    CHECK(back->sampleRate == 48000);
    CHECK(back->pcm.size() == clip.pcm.size());
    for (std::size_t i = 0; i < clip.pcm.size() && i < back->pcm.size(); ++i)
        CHECK(std::fabs(back->pcm[i] - clip.pcm[i]) < 1e-6f);
    CHECK(back->frames() == 3);

    // 자동 판별(decodeAudioAuto): RIFF 매직으로 WAV 인식 + 파형 피크 준비
    auto autoClip = audio::decodeAudioAuto(bytes.data(), bytes.size(), "auto");
    CHECK(autoClip != nullptr);
    if (autoClip) {
        CHECK(autoClip->sampleRate == 48000);
        CHECK(!autoClip->peakMax.empty());
    }

    // 손상된 데이터는 거부
    CHECK(audio::decodeWav(nullptr, 0, "x") == nullptr);
    const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(audio::decodeWav(junk, sizeof(junk), "x") == nullptr);
}

// 트랙의 볼륨/팬/플러그인/오디오 참조가 프로젝트 텍스트에 보존되는지.
void testTrackExtrasRoundTrip() {
    project::ProjectData p;
    p.song.masterVolume = 0.8f;
    p.song.masterPan = 0.25f;
    p.song.masterGain = 1.4f;
    seq::Track t;
    t.name = "Gtr";
    t.volume = 0.42f;
    t.pan = -0.3f;
    t.gain = 1.7f;
    t.inputChannelMode = 2;
    t.plugins.push_back({"Surge XT", "C:\\VST3\\Surge XT.vst3", 1, true, false});
    t.plugins.push_back({"Pro-Q 3", "C:\\Program Files\\FabFilter\\Pro-Q 3.vst3", 0, false, true});
    // 트랙당 여러 클립: 두 개를 넣어 순서와 배치가 보존되는지 본다.
    auto clip1 = std::make_shared<audio::AudioClip>();
    clip1->name = "take 1";
    clip1->channels = 1;
    clip1->sampleRate = 44100;
    clip1->pcm = {0.1f, 0.2f};
    clip1->startTick = 960;
    clip1->speed = 1.5;
    clip1->trimStart = 0;
    clip1->trimLen = 2;
    clip1->fadeInSec = 0.25;
    clip1->fadeOutSec = 1.0;
    t.clips.push_back(std::move(clip1));
    auto clip2 = std::make_shared<audio::AudioClip>();
    clip2->name = "take 2";
    clip2->channels = 1;
    clip2->sampleRate = 44100;
    clip2->pcm = {0.3f, 0.4f};
    clip2->startTick = 3840;
    clip2->trimLen = 2;
    t.clips.push_back(std::move(clip2));
    p.song.tracks.push_back(std::move(t));

    project::ProjectData q;
    CHECK(project::deserialize(q, project::serialize(p)));
    CHECK(q.song.tracks.size() == 1);
    if (q.song.tracks.empty()) return;
    const auto& r = q.song.tracks[0];
    CHECK(std::fabs(r.volume - 0.42f) < 1e-4f);
    CHECK(std::fabs(r.pan + 0.3f) < 1e-4f);
    CHECK(std::fabs(r.gain - 1.7f) < 1e-4f);
    CHECK(std::fabs(q.song.masterVolume - 0.8f) < 1e-4f);
    CHECK(std::fabs(q.song.masterPan - 0.25f) < 1e-4f);
    CHECK(std::fabs(q.song.masterGain - 1.4f) < 1e-4f);
    CHECK(r.inputChannelMode == 2);
    CHECK(r.plugins.size() == 2);
    if (r.plugins.size() == 2) {
        CHECK(r.plugins[0].name == "Surge XT" && r.plugins[0].isInstrument && !r.plugins[0].enabled);
        CHECK(r.plugins[0].path == "C:\\VST3\\Surge XT.vst3"); // 복원용 경로
        CHECK(r.plugins[0].classIndex == 1);
        CHECK(r.plugins[1].name == "Pro-Q 3" && !r.plugins[1].isInstrument && r.plugins[1].enabled);
        // 경로에 공백이 있어도 보존된다
        CHECK(r.plugins[1].path == "C:\\Program Files\\FabFilter\\Pro-Q 3.vst3");
        CHECK(r.plugins[1].classIndex == 0);
    }
    // PCM은 사이드카 WAV라 텍스트엔 참조만 남는다 (클립마다 한 줄, 순서 보존)
    CHECK(q.audioRefs.size() == 2);
    if (q.audioRefs.size() == 2) {
        CHECK(q.audioRefs[0].trackIndex == 0);
        CHECK(q.audioRefs[0].file == "track0_0.wav");
        CHECK(q.audioRefs[0].startTick == 960);
        CHECK(std::fabs(q.audioRefs[0].speed - 1.5) < 1e-9);
        CHECK(q.audioRefs[0].trimLen == 2);
        CHECK(q.audioRefs[0].clipName == "take 1");
        CHECK(std::fabs(q.audioRefs[0].fadeInSec - 0.25) < 1e-9);  // 페이드 보존
        CHECK(std::fabs(q.audioRefs[0].fadeOutSec - 1.0) < 1e-9);
        CHECK(q.audioRefs[1].fadeInSec == 0.0); // 페이드 없는 클립은 0 유지
        CHECK(q.audioRefs[1].file == "track0_1.wav");
        CHECK(q.audioRefs[1].startTick == 3840);
        CHECK(q.audioRefs[1].clipName == "take 2");
    }
}

// 타임스트레치 수식: 배속 <-> 재생 길이가 서로 역함수여야 한다.
void testClipSpeedDuration() {
    audio::AudioClip c;
    c.channels = 1;
    c.sampleRate = 48000;
    c.pcm.assign(96000, 0.0f); // 2초 분량
    c.trimLen = 96000;

    c.speed = 1.0;
    CHECK(std::fabs(c.durationSeconds() - 2.0) < 1e-9);
    c.speed = 2.0; // 2배 빠름 -> 절반 길이
    CHECK(std::fabs(c.durationSeconds() - 1.0) < 1e-9);
    c.speed = 0.5; // 절반 속도 -> 두 배 길이
    CHECK(std::fabs(c.durationSeconds() - 4.0) < 1e-9);

    // speedForDuration은 durationSeconds의 역함수
    c.speed = 1.0;
    for (double target : {0.5, 1.0, 2.0, 4.0}) {
        const double sp = c.speedForDuration(target);
        audio::AudioClip d = c;
        d.speed = sp;
        CHECK(std::fabs(d.durationSeconds() - target) < 1e-9);
    }
    // 오른쪽으로 늘림(길게) = 느려짐(배속 감소)
    CHECK(c.speedForDuration(4.0) < c.speedForDuration(2.0));

    // 잘못된 입력은 현재 배속 유지
    c.speed = 1.25;
    CHECK(std::fabs(c.speedForDuration(0.0) - 1.25) < 1e-9);
    c.sampleRate = 0;
    CHECK(std::fabs(c.speedForDuration(1.0) - 1.25) < 1e-9);
    CHECK(c.durationSeconds() == 0.0);
}

// 클립 분할(가위): 왼쪽/오른쪽 조각의 트림·PCM·타임라인 연속성이 맞아야 한다.
void testClipSplit() {
    audio::AudioClip c;
    c.channels = 1;
    c.sampleRate = 1000; // 계산이 쉬운 값
    c.pcm.resize(4000);
    for (std::size_t i = 0; i < c.pcm.size(); ++i) c.pcm[i] = (float)i; // 위치 = 값
    c.trimStart = 500;
    c.trimLen = 3000; // 0.5초~3.5초 구간, 재생 3초
    c.speed = 1.0;
    c.fadeOutSec = 1.5;

    // 1초 지점에서 자르면: 왼쪽 1초(소스 500~1500), 오른쪽 2초(소스 1500~)
    auto right = audio::splitClipAt(c, 1.0);
    CHECK(right != nullptr);
    if (!right) return;
    CHECK(c.trimLen == 1000);           // 왼쪽 = 1초
    CHECK(c.fadeOutSec == 0.0);         // 잘린 경계엔 페이드아웃 없음
    CHECK(right->trimStart == 0);
    CHECK(right->trimLen == 2000);      // 오른쪽 = 2초
    CHECK(std::fabs(right->fadeOutSec - 1.5) < 1e-9); // 끝 페이드는 오른쪽으로
    // 오른쪽 pcm 첫 샘플 = 원본 소스 프레임 1500 (trimStart 500 + 1초)
    CHECK(right->pcm.size() == 2500);   // 1500~3999
    CHECK(std::fabs(right->pcm[0] - 1500.0f) < 1e-3f);
    // 시간 합이 보존된다
    CHECK(std::fabs(c.durationSeconds() + right->durationSeconds() - 3.0) < 1e-9);

    // 끝에 너무 가까우면 거부
    CHECK(audio::splitClipAt(c, 0.001) == nullptr);
    CHECK(audio::splitClipAt(c, 999.0) == nullptr);
}

// 클립 게인: 정규화 계산(트림 반영) + 분할 시 유지 + 프로젝트 텍스트 왕복.
void testClipGain() {
    audio::AudioClip c;
    c.channels = 1;
    c.sampleRate = 1000;
    c.pcm.resize(3000, 0.0f);
    c.pcm[100] = 0.9f;  // 트림 "밖"의 큰 피크 (무시돼야 함)
    c.pcm[1500] = 0.25f; // 재생 구간 안의 피크
    c.trimStart = 1000;
    c.trimLen = 2000;

    // 재생 구간 피크 0.25 -> 목표 0.891이면 게인 3.564
    const float g = audio::normalizeGainFor(c);
    CHECK(std::fabs(g - 0.891f / 0.25f) < 1e-3f);

    // 무음이면 1.0 (0으로 나누지 않는다)
    audio::AudioClip silent;
    silent.channels = 1;
    silent.sampleRate = 1000;
    silent.pcm.resize(100, 0.0f);
    silent.trimLen = 100;
    CHECK(audio::normalizeGainFor(silent) == 1.0f);

    // 분할해도 양쪽 조각이 게인을 유지한다
    c.gain = 2.5f;
    c.speed = 1.0;
    auto right = audio::splitClipAt(c, 1.0);
    CHECK(right != nullptr);
    if (right) CHECK(std::fabs(right->gain - 2.5f) < 1e-6f);
    CHECK(std::fabs(c.gain - 2.5f) < 1e-6f);

    // 프로젝트 텍스트 왕복 (tgain 라인)
    project::ProjectData p;
    seq::Track t;
    t.name = "A";
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sampleRate = 1000;
    clip->pcm.resize(10, 0.1f);
    clip->trimLen = 10;
    clip->gain = 1.75f;
    clip->name = "loud";
    t.clips.push_back(clip);
    p.song.tracks.push_back(t);
    const std::string text = project::serialize(p);
    CHECK(text.find("tgain 1.75") != std::string::npos);

    // 트랙 프리즈 상태 왕복 (tfrz + tbounce 라인)
    p.song.tracks[0].frozen = true;
    clip->freezeBounce = true;
    const std::string text2 = project::serialize(p);
    CHECK(text2.find("tfrz 1") != std::string::npos);
    CHECK(text2.find("tbounce") != std::string::npos);
    project::ProjectData q2;
    CHECK(project::deserialize(q2, text2));
    CHECK(q2.song.tracks.size() == 1 && q2.song.tracks[0].frozen);
    CHECK(q2.audioRefs.size() == 1 && q2.audioRefs[0].freezeBounce);

    // 타브 가져오기 그룹 꼬리표 왕복 (timp 라인 — 키에 공백이 있어도 그대로)
    p.song.tracks[0].importKey = "Butterfly (Tab) musx";
    p.song.tracks[0].importPart = 1;
    {
        const std::string ti = project::serialize(p);
        CHECK(ti.find("timp 1 Butterfly (Tab) musx") != std::string::npos);
        project::ProjectData qi;
        CHECK(project::deserialize(qi, ti));
        CHECK(qi.song.tracks.size() == 1);
        CHECK(qi.song.tracks[0].importKey == "Butterfly (Tab) musx");
        CHECK(qi.song.tracks[0].importPart == 1);
    }
    p.song.tracks[0].importKey.clear();
    p.song.tracks[0].importPart = -1;

    // 타브 운지 힌트 왕복 (thint 라인)
    p.song.tracks[0].tabHints.push_back({960, 68, 1}); // 2번줄(B) 9프렛
    {
        const std::string th = project::serialize(p);
        CHECK(th.find("thint 960 68 1") != std::string::npos);
        project::ProjectData qh;
        CHECK(project::deserialize(qh, th));
        CHECK(qh.song.tracks.size() == 1 && qh.song.tracks[0].tabHints.size() == 1);
        if (!qh.song.tracks[0].tabHints.empty()) {
            CHECK(qh.song.tracks[0].tabHints[0].tick == 960);
            CHECK(qh.song.tracks[0].tabHints[0].note == 68);
            CHECK(qh.song.tracks[0].tabHints[0].strIdx == 1);
        }
    }
    p.song.tracks[0].tabHints.clear();

    // 연습 트랙 표시 왕복 (tprc 라인) — 이 플래그가 살아 있어야 프로젝트를 다시
    // 열었을 때도 연습 트랙이 MIDI 트랙 목록에 섞이지 않는다.
    p.song.tracks[0].practice = true;
    {
        const std::string tp = project::serialize(p);
        CHECK(tp.find("tprc 1") != std::string::npos);
        project::ProjectData qp;
        CHECK(project::deserialize(qp, tp));
        CHECK(qp.song.tracks.size() == 1 && qp.song.tracks[0].practice);
    }
    p.song.tracks[0].practice = false;
    { // 기본값(연습 아님)은 줄을 쓰지 않는다 — 옛 프로젝트도 그대로 읽힌다
        const std::string tp = project::serialize(p);
        CHECK(tp.find("tprc") == std::string::npos);
        project::ProjectData qp;
        CHECK(project::deserialize(qp, tp));
        CHECK(qp.song.tracks.size() == 1 && !qp.song.tracks[0].practice);
    }

    // 볼륨/팬 오토메이션 왕복 (tautov/tautop 라인)
    p.song.tracks[0].volAuto.push_back({480, 0.5f});
    p.song.tracks[0].panAuto.push_back({960, -0.25f});
    const std::string text3 = project::serialize(p);
    CHECK(text3.find("tautov 480 0.5") != std::string::npos);
    project::ProjectData q3;
    CHECK(project::deserialize(q3, text3));
    CHECK(q3.song.tracks[0].volAuto.size() == 1 && q3.song.tracks[0].volAuto[0].tick == 480);
    CHECK(q3.song.tracks[0].panAuto.size() == 1 &&
          std::fabs(q3.song.tracks[0].panAuto[0].value + 0.25f) < 1e-4f);
}

// 리버스: 재생 구간이 뒤집히고 페이드 인/아웃이 자리를 바꾼다.
void testReverseClip() {
    audio::AudioClip c;
    c.channels = 1;
    c.sampleRate = 1000;
    c.pcm = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    c.trimStart = 1;
    c.trimLen = 3; // [2,3,4] 구간만 재생
    c.fadeInSec = 0.1;
    c.fadeOutSec = 0.2;
    auto r = audio::reverseClip(c);
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->frames() == 3);
        CHECK(std::fabs(r->pcm[0] - 4.0f) < 1e-6f); // 뒤집힘: 4,3,2
        CHECK(std::fabs(r->pcm[2] - 2.0f) < 1e-6f);
        CHECK(std::fabs(r->fadeInSec - 0.2) < 1e-9);  // 페이드 자리 교환
        CHECK(std::fabs(r->fadeOutSec - 0.1) < 1e-9);
    }
    audio::AudioClip tiny;
    tiny.channels = 1;
    tiny.sampleRate = 1000;
    tiny.pcm = {1.0f};
    tiny.trimLen = 1;
    CHECK(audio::reverseClip(tiny) == nullptr); // 너무 짧으면 거부
}

// 클립 병합: 배치 보존 + 사이 공백 무음 + 게인 반영.
void testMergeClips() {
    audio::AudioClip a;
    a.channels = 1;
    a.sampleRate = 1000;
    a.pcm.assign(1000, 0.5f); // 1초, 일정 값 0.5
    a.trimLen = 1000;
    audio::AudioClip b = a;
    b.gain = 2.0f;

    std::vector<audio::MergeItem> items = {{&a, 0.0}, {&b, 2.0}}; // 0~1초 / 2~3초
    auto m = audio::mergeClips(items, 1000);
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->channels == 2);
        CHECK(std::llabs((long long)m->frames() - 3000) <= 2);
        auto at = [&](double sec) { return m->pcm[(std::size_t)(sec * 1000.0) * 2]; };
        CHECK(std::fabs(at(0.5) - 0.5f) < 0.05f); // a 구간
        CHECK(std::fabs(at(1.5)) < 1e-4f);        // 사이 공백 = 무음
        CHECK(std::fabs(at(2.5) - 1.0f) < 0.1f);  // b 구간 (게인 2 반영)
    }
    CHECK(audio::mergeClips({}, 1000) == nullptr); // 빈 목록 거부
}

// 음정 유지 스트레치(WSOLA): 길이가 ratio대로 변하고 신호가 보존되는지.
void testPitchStretch() {
    audio::AudioClip c;
    c.channels = 1;
    c.sampleRate = 44100;
    const int n = 44100; // 1초짜리 440Hz 사인
    c.pcm.resize((std::size_t)n);
    for (int i = 0; i < n; ++i)
        c.pcm[(std::size_t)i] =
            (float)(std::sin(2.0 * 3.14159265358979 * 440.0 * i / 44100.0) * 0.5);
    c.trimLen = n;

    auto s = audio::stretchClipPitchPreserve(c, 1.5); // 1.5배 길게
    CHECK(s != nullptr);
    if (s) {
        CHECK(std::llabs((long long)s->frames() - (long long)(n * 1.5)) < 4410); // ±0.1초
        float peak = 0.0f;
        for (float v : s->pcm) peak = std::max(peak, std::fabs(v));
        CHECK(peak > 0.3f && peak < 0.75f); // 무음도 과증폭도 아니어야 한다
        CHECK(s->speed == 1.0);             // 배속이 아니라 파형 재배열
    }
    auto h = audio::stretchClipPitchPreserve(c, 0.5); // 절반으로
    CHECK(h != nullptr);
    if (h) CHECK(std::llabs((long long)h->frames() - (long long)(n * 0.5)) < 4410);

    audio::AudioClip tiny; // 너무 짧으면 거부
    tiny.channels = 1;
    tiny.sampleRate = 44100;
    tiny.pcm.resize(500);
    tiny.trimLen = 500;
    CHECK(audio::stretchClipPitchPreserve(tiny, 1.5) == nullptr);
}

// 버전 분기 트리: [versions] 섹션 왕복 (id/부모/이름/곡/배치/현재 노드).
void testVersionsRoundTrip() {
    project::ProjectData p;
    seq::Track t;
    t.name = "Main";
    t.addNote(0, 480, 60, 100);
    p.song.tracks.push_back(t);

    // 버전 1: 노트 하나 + 클립 하나 (배치는 places가 원본)
    project::VersionSnap v1;
    v1.id = 1;
    v1.parent = -1;
    v1.name = "V1";
    seq::Track vt;
    vt.name = "Main";
    vt.addNote(0, 240, 64, 90);
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sampleRate = 1000;
    clip->pcm.resize(50, 0.2f);
    clip->trimLen = 50;
    clip->name = "take";
    vt.clips.push_back(clip);
    v1.song.tracks.push_back(vt);
    v1.places.push_back({960, 1.0, 5, 40, 0.1, 0.2, 1.5f});
    p.versions.push_back(v1);

    // 버전 2: V1의 자식 (이름에 공백 포함)
    project::VersionSnap v2;
    v2.id = 2;
    v2.parent = 1;
    v2.name = "V2 final mix";
    v2.note = "기타 두 번째 테이크가 더 좋음"; // 체크인 메모 (공백 포함)
    v2.song.tracks.push_back(t);
    p.versions.push_back(v2);
    p.versionCurrent = 2;
    p.versionNextId = 3;

    project::ProjectData q;
    CHECK(project::deserialize(q, project::serialize(p)));
    CHECK(q.versions.size() == 2);
    CHECK(q.versionCurrent == 2);
    CHECK(q.versionNextId == 3);
    CHECK(q.versions[0].id == 1 && q.versions[0].parent == -1);
    CHECK(q.versions[0].name == "V1");
    CHECK(q.versions[1].id == 2 && q.versions[1].parent == 1);
    CHECK(q.versions[1].name == "V2 final mix"); // 공백 포함 이름 보존
    CHECK(q.versions[1].note == "기타 두 번째 테이크가 더 좋음"); // 메모 보존
    CHECK(q.versions[0].note.empty()); // 메모 없는 노드는 빈 채로
    // 버전 곡 내용
    const auto notes = seq::extractNotes(q.versions[0].song.tracks[0]);
    CHECK(notes.size() == 1 && notes[0].note == 64);
    // 배치(places) 왕복 — 클립 객체가 아닌 places가 진짜 배치다
    CHECK(q.versions[0].places.size() == 1);
    const auto& pl = q.versions[0].places[0];
    CHECK(pl.startTick == 960);
    CHECK(pl.trimStart == 5 && pl.trimLen == 40);
    CHECK(std::fabs(pl.fadeInSec - 0.1) < 1e-9 && std::fabs(pl.fadeOutSec - 0.2) < 1e-9);
    CHECK(std::fabs(pl.gain - 1.5f) < 1e-6f);
    // 클립 참조(파일명)는 refs로 넘어온다 (load()가 사용)
    CHECK(q.versions[0].refs.size() == 1);
    CHECK(q.versions[0].refs[0].file == "vclip0.wav");
    // 버전 없는 프로젝트 출력엔 [versions] 섹션이 없다 (옛 파일과 동일)
    project::ProjectData bare;
    CHECK(project::serialize(bare).find("[versions]") == std::string::npos);
}

// 실제 디스크 왕복: .midipro + 사이드카 <이름>.audio/track0.wav 로 PCM이 보존되는지.
void testProjectFileRoundTrip() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "midipro_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path proj = dir / "song.midipro";

    project::ProjectData p;
    seq::Track t;
    t.name = "Rec";
    t.volume = 0.6f;
    auto take = std::make_shared<audio::AudioClip>();
    take->name = "take";
    take->channels = 1;
    take->sampleRate = 48000;
    take->pcm = {0.25f, -0.5f, 0.75f, -1.0f};
    take->startTick = 480;
    take->speed = 1.0;
    take->trimStart = 1;
    take->trimLen = 3;
    t.clips.push_back(take);
    auto take2 = std::make_shared<audio::AudioClip>();
    take2->name = "take2";
    take2->channels = 1;
    take2->sampleRate = 48000;
    take2->pcm = {0.5f, 0.5f};
    take2->startTick = 1920;
    take2->trimLen = 2;
    t.clips.push_back(take2);
    p.song.tracks.push_back(std::move(t));

    CHECK(project::save(p, proj));
    CHECK(fs::exists(proj));
    CHECK(fs::exists(dir / "song.audio" / "track0_0.wav")); // 클립별 사이드카 생성
    CHECK(fs::exists(dir / "song.audio" / "track0_1.wav"));

    project::ProjectData q;
    CHECK(project::load(q, proj));
    CHECK(q.song.tracks.size() == 1);
    if (q.song.tracks.empty() || q.song.tracks[0].clips.size() != 2 ||
        !q.song.tracks[0].clips[0] || !q.song.tracks[0].clips[1]) {
        CHECK(false); // 오디오가 복원되지 않음
        fs::remove_all(dir, ec);
        return;
    }
    const auto& c = *q.song.tracks[0].clips[0];
    CHECK(c.channels == 1);
    CHECK(c.sampleRate == 48000);
    CHECK(c.pcm.size() == 4);
    for (std::size_t i = 0; i < 4 && i < c.pcm.size(); ++i)
        CHECK(std::fabs(c.pcm[i] - take->pcm[i]) < 1e-6f);
    CHECK(c.startTick == 480);      // 배치 복원
    CHECK(c.trimStart == 1);        // 트림 복원
    CHECK(c.trimLen == 3);
    const auto& c2 = *q.song.tracks[0].clips[1];
    CHECK(c2.startTick == 1920);    // 두 번째 클립도 자기 위치에
    CHECK(c2.pcm.size() == 2);
    CHECK(std::fabs(q.song.tracks[0].volume - 0.6f) < 1e-4f);

    // 오디오가 없는 프로젝트는 사이드카 폴더를 만들지 않는다
    const fs::path proj2 = dir / "noaudio.midipro";
    project::ProjectData plain;
    plain.song.tracks.push_back(seq::Track{});
    CHECK(project::save(plain, proj2));
    CHECK(!fs::exists(dir / "noaudio.audio"));

    fs::remove_all(dir, ec);
}

// MP3 인코딩(Media Foundation) -> dr_mp3 디코딩 왕복: 길이와 내용이 대체로 보존.
void testMp3RoundTrip() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "midipro_test_mp3";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path mp3 = dir / "tone.mp3";

    // 1초짜리 440Hz 스테레오 사인파
    audio::AudioClip tone;
    tone.channels = 2;
    tone.sampleRate = 44100;
    tone.pcm.resize(44100 * 2);
    for (int i = 0; i < 44100; ++i) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * (float)i / 44100.0f);
        tone.pcm[(std::size_t)i * 2] = s;
        tone.pcm[(std::size_t)i * 2 + 1] = s;
    }
    tone.trimLen = 44100;

    CHECK(audio::writeMp3File(tone, mp3, 192));
    CHECK(fs::exists(mp3) && fs::file_size(mp3, ec) > 4000); // 대략 24KB/s 기대

    // dr_mp3로 다시 디코드해 내용 확인
    std::ifstream in(mp3, std::ios::binary | std::ios::ate);
    CHECK((bool)in);
    const std::streamsize sz = in.tellg();
    std::vector<uint8_t> bytes((std::size_t)sz);
    in.seekg(0);
    in.read((char*)bytes.data(), sz);
    auto back = audio::decodeMp3(bytes.data(), bytes.size(), "tone");
    CHECK(back != nullptr);
    if (back) {
        CHECK(back->sampleRate == 44100);
        // 인코더 지연 패딩이 있어 정확히 같진 않지만 1초 근처여야 한다
        CHECK(back->frames() > 40000 && back->frames() < 50000);
        // 무음이 아니어야 한다 (에너지 확인)
        double sum = 0;
        for (float v : back->pcm) sum += (double)v * v;
        CHECK(std::sqrt(sum / (double)back->pcm.size()) > 0.1);
    }

    // 지원하지 않는 샘플레이트는 거부
    audio::AudioClip bad = tone;
    bad.sampleRate = 22050;
    CHECK(!audio::writeMp3File(bad, dir / "bad.mp3"));

    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    testRoundTrip();
    testWavRoundTrip();
    testClipSpeedDuration();
    testClipSplit();
    testClipGain();
    testReverseClip();
    testMergeClips();
    testPitchStretch();
    testVersionsRoundTrip();
    testTrackExtrasRoundTrip();
    testProjectFileRoundTrip();
    testMp3RoundTrip();
    if (g_failures == 0) {
        std::cout << "[OK] project tests passed\n";
        return 0;
    }
    std::cout << "[FAIL] " << g_failures << " check(s) failed\n";
    return 1;
}
