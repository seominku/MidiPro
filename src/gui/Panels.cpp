// =============================================================
// MidiPro - gui/Panels.cpp
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h" // 분리된 패널 파일들과 공유하는 위젯

#include "audio/SynthPreset.h"
#include "guitar/Fretboard.h"
#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"
#include "midi2/Ump.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"
#include "vst/PluginScanner.h"

#include "imgui.h"
#include "imgui_internal.h" // 트랙 뷰 테이블의 내부 스크롤 제어

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace midipro::gui {

// 실제 내용의 끝(틱): MIDI 이벤트 끝과 오디오 클립 끝 중 더 긴 쪽. 플레이헤드는 제외.
// (Panels.h에 공개 — 내보내기의 기본 구간 계산에 App도 쓴다)
uint32_t songEndTicks(const AppState& state) {
    uint32_t t = state.song.lengthTicks();
    for (const auto& tr : state.song.tracks)
        for (const auto& cp : tr.clips) {
            if (!cp || cp->sampleRate <= 0) continue;
            // 템포 맵 반영: 클립 시작의 절대 초 + 길이 -> 끝 틱
            const uint32_t end = (uint32_t)seq::songSecToTick(
                state.song,
                seq::songTickToSec(state.song, cp->startTick) + cp->durationSeconds());
            t = std::max(t, end);
        }
    return t;
}

// (파일 분할로 여러 TU가 공유하게 되어 익명 네임스페이스 해제)

// 노트 번호 -> "C4" 문자열 (짧은 래퍼)
std::string noteName(uint8_t note) {
    return midi::MidiMessage::noteName(note);
}

// 재생 중이면 곡의 현재 틱, 아니면 0
uint32_t playheadTick(const AppState& state) {
    return state.player ? state.player->currentTick() : 0;
}

// 내용 끝 + 현재 플레이헤드까지 포함(타임라인 표시 길이용).
uint32_t contentTicksWithAudio(const AppState& state) {
    return std::max(songEndTicks(state), state.playPosTick);
}

// 클립의 타임라인상 끝 틱 (배속·트림·템포 맵 반영)
double clipEndTick(const audio::AudioClip& c, const seq::Song& song) {
    return seq::songSecToTick(song,
                              seq::songTickToSec(song, c.startTick) + c.durationSeconds());
}

// 클립의 화면 x 범위 [cx0, cx1] (배속·트림 반영)
void clipScreenX(const audio::AudioClip& c, float originX, float zoom, const seq::Song& song,
                 float& cx0, float& cx1) {
    cx0 = originX + (float)c.startTick * zoom;
    cx1 = originX + (float)clipEndTick(c, song) * zoom;
}

// 마우스 x가 어느 클립 위(몸통 또는 끝 핸들 ±6px)인가. 없으면 -1.
// 겹치면 나중에 그려진(위에 보이는) 클립이 우선이라 뒤에서부터 검사한다.
int clipHitTest(const std::vector<std::shared_ptr<audio::AudioClip>>& clips, float mx,
                float originX, float zoom, const seq::Song& song) {
    for (int ci = (int)clips.size() - 1; ci >= 0; --ci) {
        if (!clips[(std::size_t)ci]) continue;
        float cx0, cx1;
        clipScreenX(*clips[(std::size_t)ci], originX, zoom, song, cx0, cx1);
        if (mx >= cx0 - 6.0f && mx <= cx1 + 6.0f) return ci;
    }
    return -1;
}

// 오디오 클립을 뚜렷한 블록으로 그린다: 어두운 배경 + 테두리 + 좌/우 끝 핸들 + 이름.
// 파형은 이 위에 겹쳐 그린다.
void drawClipBlock(ImDrawList* dl, const audio::AudioClip& clip, float originX, float topY, float h,
                   float zoom, const seq::Song& song, bool sel) {
    if (clip.sampleRate <= 0) return;
    const double startSec = seq::songTickToSec(song, clip.startTick);
    const double endTk = clipEndTick(clip, song);
    const float cx0 = originX + (float)clip.startTick * zoom;
    const float cx1 = originX + (float)endTk * zoom;
    const float y0 = topY + 2.0f;
    const float y1 = topY + h - 2.0f;

    // 어두운 클립 배경 + 테두리
    dl->AddRectFilled(ImVec2(cx0, y0), ImVec2(cx1, y1), IM_COL32(18, 22, 28, 235), 4.0f);
    dl->AddRect(ImVec2(cx0, y0), ImVec2(cx1, y1),
                sel ? IM_COL32(130, 190, 255, 255) : IM_COL32(90, 110, 140, 255), 4.0f, 0, 1.5f);
    // 좌(시작)·우(끝) 끝 핸들
    dl->AddRectFilled(ImVec2(cx0, y0), ImVec2(cx0 + 4.0f, y1), IM_COL32(120, 200, 120, 235));
    dl->AddRectFilled(ImVec2(cx1 - 4.0f, y0), ImVec2(cx1, y1), IM_COL32(240, 150, 90, 235));
    // 이름 + 시작/끝 라벨
    dl->AddText(ImVec2(cx0 + 8.0f, y0 + 2.0f), IM_COL32(225, 225, 235, 255), clip.name.c_str());
    dl->AddText(ImVec2(cx0 + 8.0f, y1 - 16.0f), IM_COL32(150, 220, 150, 230), "▶시작");
    dl->AddText(ImVec2(cx1 - 34.0f, y1 - 16.0f), IM_COL32(240, 170, 110, 230), "끝◀");
    // 페이드 인/아웃 표시 (대각선). 길이는 초 -> 틱(템포 맵) -> 픽셀.
    if (clip.fadeInSec > 0.0) {
        const double ft = seq::songSecToTick(song, startSec + clip.fadeInSec);
        const float w = (float)(ft - (double)clip.startTick) * zoom;
        dl->AddLine(ImVec2(cx0, y1), ImVec2(std::min(cx0 + w, cx1), y0),
                    IM_COL32(255, 255, 255, 150), 1.5f);
    }
    if (clip.fadeOutSec > 0.0) {
        const double endSec = seq::songTickToSec(song, endTk);
        const double ft = seq::songSecToTick(song, endSec - clip.fadeOutSec);
        const float w = (float)(endTk - ft) * zoom;
        dl->AddLine(ImVec2(cx1, y1), ImVec2(std::max(cx1 - w, cx0), y0),
                    IM_COL32(255, 255, 255, 150), 1.5f);
    }
    // 타임스트레치된 클립은 배속을 표시한다 (1.00x면 생략)
    if (std::fabs(clip.speed - 1.0) > 1e-3) {
        char sbuf[24];
        std::snprintf(sbuf, sizeof(sbuf), "%.2fx", clip.speed);
        const ImVec2 ts = ImGui::CalcTextSize(sbuf);
        const ImVec2 tp(cx1 - ts.x - 8.0f, y0 + 2.0f);
        dl->AddRectFilled(ImVec2(tp.x - 3, tp.y), ImVec2(tp.x + ts.x + 3, tp.y + ts.y),
                          IM_COL32(20, 20, 26, 200));
        dl->AddText(tp, IM_COL32(250, 210, 120, 255), sbuf);
    }
}

// 오디오 파형을 픽셀 단위로 그려 끊김 없이 연결한다(가운데 밴드).
// 각 픽셀의 소스 프레임 구간을 덮는 피크 min/max를 세로선으로 채운다.
void drawWaveform(ImDrawList* dl, const audio::AudioClip& clip, float originX, float topY, float h,
                  float zoom, const seq::Song& song, ImU32 col) {
    if (clip.sampleRate <= 0 || clip.peakMax.empty()) return;
    const double sr = (double)clip.sampleRate;
    const float midY = topY + h * 0.5f;
    const float amp = h * 0.45f;
    const double startSec = seq::songTickToSec(song, clip.startTick);
    const float x0 = originX + (float)clip.startTick * zoom;
    const float x1 = originX + (float)clipEndTick(clip, song) * zoom;
    const float vx0 = std::max(x0, dl->GetClipRectMin().x);
    const float vx1 = std::min(x1, dl->GetClipRectMax().x);
    const int nb = (int)clip.peakMax.size();
    for (float px = vx0; px <= vx1; px += 1.0f) {
        const double tickL = (double)(px - originX) / zoom;
        const double tickR = (double)(px + 1.0f - originX) / zoom;
        // 경과 초 = 절대 초(템포 맵) - 클립 시작 초, 소스 프레임 = trimStart + 경과 프레임
        const double s0 = std::max(0.0, seq::songTickToSec(song, tickL) - startSec);
        const double s1 = std::max(0.0, seq::songTickToSec(song, tickR) - startSec);
        const double f0 = (double)clip.trimStart + s0 * sr * clip.speed;
        const double f1 = (double)clip.trimStart + s1 * sr * clip.speed;
        int b0 = (int)(f0 / clip.peakStride);
        int b1 = (int)(f1 / clip.peakStride);
        if (b0 >= nb) { // 소스 끝을 넘은 공백 구간: 가는 중앙선
            dl->AddLine(ImVec2(px, midY), ImVec2(px, midY), col);
            continue;
        }
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        if (b1 < b0) b1 = b0;
        float lo = 1.0f, hi = -1.0f;
        for (int b = b0; b <= b1; ++b) {
            if (clip.peakMin[b] < lo) lo = clip.peakMin[b];
            if (clip.peakMax[b] > hi) hi = clip.peakMax[b];
        }
        // 클립 게인을 파형 높이에 반영해 조절 결과가 바로 보이게 한다
        lo = std::clamp(lo * clip.gain, -1.0f, 1.0f);
        hi = std::clamp(hi * clip.gain, -1.0f, 1.0f);
        dl->AddLine(ImVec2(px, midY - hi * amp), ImVec2(px, midY - lo * amp), col);
    }
}

// 트랙 뷰 표의 내부(스크롤) 창을 가장자리 자동 스크롤한다.
// 무언가를 드래그하는 중 마우스가 좌우 끝에 가까우면 뷰가 옆으로 흐른다.
// (표 안에서 호출해야 한다 — GetCurrentTable 기준)
void trackViewEdgeScroll(float mouseX) {
    ImGuiTable* tbl = ImGui::GetCurrentTable();
    if (!tbl) return;
    ImGuiWindow* inner = tbl->InnerWindow;
    if (!inner) return;
    constexpr float kEdge = 36.0f, kMaxSpeed = 18.0f;
    const float left = inner->Pos.x;
    const float right = inner->Pos.x + inner->Size.x;
    float dx = 0.0f;
    if (mouseX < left + kEdge)
        dx = -(1.0f - std::clamp((mouseX - left) / kEdge, 0.0f, 1.0f)) * kMaxSpeed;
    else if (mouseX > right - kEdge)
        dx = (1.0f - std::clamp((right - mouseX) / kEdge, 0.0f, 1.0f)) * kMaxSpeed;
    if (dx != 0.0f) ImGui::SetScrollX(inner, std::max(0.0f, inner->Scroll.x + dx));
}

// 템포 변경 지점을 주황 세로선(+ 위쪽 "N BPM" 라벨)으로 표시한다.
// 트랙 뷰/피아노 롤 공용 — 어디서 템포가 바뀌는지 한눈에 보이게 한다.
void drawTempoMarkers(ImDrawList* dl, const seq::Song& song, float originX, float zoom, float y0,
                      float y1, bool withLabels, int selectedIdx) {
    for (int k = 0; k < (int)song.tempoChanges.size(); ++k) {
        const auto& tc = song.tempoChanges[(std::size_t)k];
        const float x = originX + (float)tc.tick * zoom;
        if (x < dl->GetClipRectMin().x - 60.0f || x > dl->GetClipRectMax().x + 60.0f) continue;
        const bool selM = (k == selectedIdx);
        // 선택된 마커: 흰 아웃라인을 먼저 깔아 "클릭됨"이 보이게 한다
        if (selM) dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, 230), 4.5f);
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 165, 70, 215), 2.0f);
        if (!withLabels) continue;
        char buf[24]; // 램프(점진 변화)면 >> 로 표시
        std::snprintf(buf, sizeof(buf), tc.ramp ? "%.0f BPM >>" : "%.0f BPM", tc.bpm);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 r0(x + 2.0f, y0 + 1.0f), r1(x + ts.x + 10.0f, y0 + ts.y + 5.0f);
        dl->AddRectFilled(r0, r1, IM_COL32(95, 58, 16, 235), 3.0f);
        if (selM) dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 240), 3.0f, 0, 1.8f);
        dl->AddText(ImVec2(x + 6.0f, y0 + 3.0f), IM_COL32(255, 205, 135, 255), buf);
    }
}

// 지금 Note On을 보내고, durationSec 뒤에 Note Off를 보내도록 예약한다.
// 왜: 기타 도우미 버튼은 "쳤다 뗀다"라서 자동으로 소리를 멈춰야 한다.
void triggerNote(AppState& state, uint8_t channel, uint8_t note, uint8_t velocity,
                 double durationSec) {
    if (state.output == nullptr) return;
    if (!state.output->isOpen()) state.output->openPort((unsigned)state.selectedOutputPort);
    state.output->send(midi::MidiMessage::makeNoteOn(channel, note, velocity));
    state.pendingOffs.push_back({channel, note, ImGui::GetTime() + durationSec});
}

// 녹음 중 아직 안 뗀 노트를 현재 틱에서 닫아 트랙에 확정한다.
void finalizeRecording(AppState& state) {
    if (!state.recording) return;
    const uint32_t nowTick = playheadTick(state);
    if (state.selectedTrack < (int)state.song.tracks.size()) {
        auto& track = state.song.tracks[state.selectedTrack];
        for (int n = 0; n < 128; ++n) {
            auto& open = state.openRecNotes[n];
            if (open.active) {
                const uint32_t dur = nowTick > open.startTick ? nowTick - open.startTick : 1;
                track.addNote(open.startTick, dur, (uint8_t)n, open.velocity);
                seq::adoptNoteIntoClips(track, (uint8_t)n, open.startTick);
                open.active = false;
            }
        }
        track.sortEvents();
    }
    state.recording = false;
}

// 틱 -> 엔진 샘플 프레임 (오디오 클립 위치/시크 계산, 템포 맵 반영)
int64_t tickToFrame(AppState& state, uint32_t tick) {
    if (!state.audioClips) return 0;
    const double sr = state.audioClips->engineSampleRate();
    return (int64_t)(seq::songTickToSec(state.song, tick) * sr);
}

// 트랙별 볼륨/팬/마스터가 반영된 오디오 믹스 스냅샷을 엔진에 넘긴다.
// GUI 스레드에서 매 프레임 재구성한다(할당은 GUI 스레드이므로 Rule 3 무관).


void rebuildAudioMixAt(AppState& state, uint32_t tick) {
    if (!state.audioClips) return;
    const double sr = state.audioClips->engineSampleRate();
    if (sr <= 0.0) return;
    auto mix = std::make_shared<std::vector<audio::AudioMixClip>>();
    for (const auto& t : state.song.tracks) {
        // 마스터 볼륨은 엔진 최종 출력에서 한 번만 적용된다(여기선 트랙 볼륨/팬만).
        // 오토메이션 곡선이 있으면 tick 시점 값이 페이더를 대신한다.
        const float av = seq::autoValueAt(t.volAuto, tick, t.volume);
        const float ap = seq::autoValueAt(t.panAuto, tick, t.pan);
        const float g = av * t.gain;
        const float gl = g * (ap <= 0.0f ? 1.0f : 1.0f - ap);
        const float gr = g * (ap >= 0.0f ? 1.0f : 1.0f + ap);
        for (const auto& cp : t.clips) {
            if (!cp) continue;
            audio::AudioMixClip c;
            c.clip = cp;
            c.startFrame = tickToFrame(state, cp->startTick);
            c.srcPerEngine = (double)cp->sampleRate / sr * cp->speed;
            c.sourceOffset = cp->trimStart;
            c.lengthSrcFrames = cp->playLen();
            c.gainL = gl * cp->gain; // 클립 게인은 트랙 페이더와 곱해진다
            c.gainR = gr * cp->gain;
            c.muted = t.muted;
            c.bus = t.channel & 0x0F; // 트랙 버스 -> 그 트랙의 이펙트 체인을 탄다
            // 페이드: 사용자 설정과 최소 3ms 디클릭 중 큰 쪽 (경계 클릭음 제거)
            constexpr double kDeclickSec = 0.003;
            c.lengthEngFrames =
                (int64_t)((double)c.lengthSrcFrames / std::max(1e-9, c.srcPerEngine));
            c.fadeInFrames = (int64_t)(std::max(cp->fadeInSec, kDeclickSec) * sr);
            c.fadeOutFrames = (int64_t)(std::max(cp->fadeOutSec, kDeclickSec) * sr);
            mix->push_back(std::move(c));
        }
    }
    state.audioClips->setAudioMix(mix);
}

// (파일 분할로 여러 TU가 공유하게 되어 익명 네임스페이스 해제)

// 프레임마다: 재생 헤드 시점의 오토메이션을 반영해 믹스를 갱신한다
void rebuildAudioMix(AppState& state) { rebuildAudioMixAt(state, state.playPosTick); }

// 정지: 녹음 중이면 먼저 마무리한 뒤 트랜스포트를 멈춘다.
void stopTransport(AppState& state) {
    finalizeRecording(state);
    if (state.player) state.player->stop();
    if (state.audioClips) state.audioClips->stopAudio();
    state.audioStartPending = false; // 카운트인 대기 중이었다면 취소
}

// 현재 출력의 모든 소리를 즉시 끈다 (All Sound Off 120 + All Notes Off 123).
// 정지 후 남은 잔음이나 눌린 노트가 이어지지 않게 할 때 쓴다.
void silenceOutput(AppState& state) {
    if (!state.output) return;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        state.output->send({(uint8_t)(midi::kStatusControlChange | ch), 120, 0});
        state.output->send({(uint8_t)(midi::kStatusControlChange | ch), 123, 0});
    }
}

// undo/redo 후 선택 트랙이 범위를 벗어나지 않게 보정한다.
void clampSelection(AppState& state) {
    const int count = (int)state.song.tracks.size();
    if (state.selectedTrack >= count) state.selectedTrack = count > 0 ? count - 1 : 0;
    if (state.selectedTrack < 0) state.selectedTrack = 0;
}

void doUndo(AppState& state) {
    if (!state.history.canUndo()) return;
    stopTransport(state); // 재생/녹음 중 곡이 바뀌면 꼬이므로 먼저 정지
    auto cur = AppState::makeSongSnapshot(state.song); // redo용 현재 상태
    if (state.history.undo(cur)) {
        AppState::applySongSnapshot(state.song, cur);
        clampSelection(state);
        rebuildAudioMix(state); // 클립 배치가 바뀌었을 수 있다
        // 클립/마커 인덱스가 어긋날 수 있으니 선택은 비운다
        state.selClips.clear();
        state.selClipTrack = state.selClipIndex = -1;
        state.selMidiClipTrack = state.selMidiClipIndex = -1;
        state.clipRange = AppState::ClipRangeSel{};
        state.selectedTempoMarker = -1;
        state.selectedMarker = -1;
        state.statusMessage = "실행취소";
    }
}

void doRedo(AppState& state) {
    if (!state.history.canRedo()) return;
    stopTransport(state);
    auto cur = AppState::makeSongSnapshot(state.song);
    if (state.history.redo(cur)) {
        AppState::applySongSnapshot(state.song, cur);
        clampSelection(state);
        rebuildAudioMix(state);
        state.selClips.clear();
        state.selClipTrack = state.selClipIndex = -1;
        state.selMidiClipTrack = state.selMidiClipIndex = -1;
        state.clipRange = AppState::ClipRangeSel{};
        state.selectedTempoMarker = -1;
        state.selectedMarker = -1;
        state.statusMessage = "다시실행";
    }
}

// 재생 시작 (재생 버튼과 스페이스바가 공유)
void startPlayback(AppState& state) {
    if (!state.output || !state.player) return;
    if (!state.output->isOpen()) {
        const auto ports = state.output->listPorts();
        if (!ports.empty()) state.output->openPort((unsigned)state.selectedOutputPort);
    }
    // 루프면 구간 시작, 아니면 눈금자로 지정한 재생 위치부터
    const uint32_t start = state.loopEnabled ? state.loopStartTick : state.playPosTick;
    state.loopCount = 0; // 새 재생 = 반복 카운터 리셋
    // 카운트인이 켜져 있으면 녹음이 아니어도 클릭 후 시작한다.
    const uint32_t preRoll = state.countIn ? countInTicks(state) : 0;
    // 노트 유무와 상관없이 정지를 누를 때까지 트랜스포트를 계속 진행한다.
    state.player->play(state.song, start, /*keepAlive=*/true, preRoll);
    if (state.audioClips) {
        if (preRoll > 0) {
            // 카운트인 동안 오디오는 대기 — 끝나면 applyTransportState가 시작
            state.audioClips->stopAudio();
            state.audioClips->seekAudio(tickToFrame(state, start));
            state.audioStartPending = true;
        } else {
            state.audioClips->startAudio(tickToFrame(state, start)); // 오디오 동기 시작
        }
    }
    state.statusMessage = preRoll > 0 ? "카운트인..." : "재생 중";
}

// 재생 중 곡을 편집했을 때 새 내용을 반영한다.
// 재생 엔진은 시작 시점의 스냅샷으로 도니, 현재 위치에서 다시 시작해
// 갱신된 곡을 반영한다(짧은 이음매만 생긴다).
void refreshPlaybackIfPlaying(AppState& state) {
    if (state.player && state.player->isPlaying() && !state.recording)
        state.player->play(state.song, state.playPosTick, /*keepAlive=*/true);
}

// 재생 위치로 이동(시크). 재생 중이면 그 지점부터 다시 시작한다.
// scrollView=false면 뷰를 움직이지 않는다 (이미 보이는 곳을 클릭한 경우).
void seekTo(AppState& state, uint32_t tick, bool scrollView) {
    state.playPosTick = tick;
    if (scrollView) state.scrollToPlayhead = true; // 뷰가 따라 이동하도록
    if (state.audioClips) state.audioClips->seekAudio(tickToFrame(state, tick));
    if (state.player && state.player->isPlaying() && !state.recording) {
        state.player->play(state.song, tick, /*keepAlive=*/true);
        if (state.audioClips) state.audioClips->startAudio(tickToFrame(state, tick));
    }
}

// 스페이스바: 재생 중이면 정지, 아니면 재생
void togglePlayback(AppState& state) {
    if (state.player && state.player->isPlaying()) {
        stopTransport(state);
        state.statusMessage = state.recording ? "녹음 정지" : "정지";
    } else {
        startPlayback(state);
    }
}



// ---------------------------------------------------------
// 예약된 Note Off 처리 (프레임마다 호출)
// ---------------------------------------------------------
void updatePendingNotes(AppState& state) {
    const double now = ImGui::GetTime();

    // 버튼 연주(MIDI 1.0) 자동 Note Off
    if (state.output && !state.pendingOffs.empty()) {
        auto it = state.pendingOffs.begin();
        while (it != state.pendingOffs.end()) {
            if (now >= it->dueTime) {
                state.output->send(midi::MidiMessage::makeNoteOff(it->channel, it->note));
                it = state.pendingOffs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // MIDI 2.0 데모 자동 Note Off (UMP로 전송)
    if (state.midi2 && !state.pendingUmpOffs.empty()) {
        auto it = state.pendingUmpOffs.begin();
        while (it != state.pendingUmpOffs.end()) {
            if (now >= it->dueTime) {
                const auto u = midi2::makeNoteOff(0, it->channel, it->note, 0);
                state.midi2->sendUmp(u.words, u.count);
                it = state.pendingUmpOffs.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ---------------------------------------------------------
// 입력 큐 펌프 (Rule 3: 로깅/녹음은 GUI 스레드에서)
//   poll()이 큐를 비우므로 로깅·녹음·소프트스루를 이 한 루프에서
//   함께 처리한다 (다른 곳에서 또 poll하면 메시지를 뺏긴다).
// ---------------------------------------------------------
void pumpMonitor(AppState& state) {
    if (state.input == nullptr) return;

    const bool canRecord = state.recording && !state.song.tracks.empty() &&
                           state.selectedTrack < (int)state.song.tracks.size();
    const uint32_t nowTick = playheadTick(state);

    midi::MidiMessage msg;
    int guard = 0; // 한 프레임에 너무 많이 처리해 멈추지 않도록 상한
    while (state.input->poll(msg) && guard++ < 512) {
        if (state.monitorEnabled) state.log(msg.toString());

        // 소프트 스루: 입력을 현재 출력(신스 등)으로 흘려 모니터링.
        // 원본 바이트를 그대로 보내 채널과 표현(피치벤드/애프터터치/CC74)까지
        // 보존한다 -> MPE 컨트롤러의 노트별 표현이 내장 신스에 전달된다.
        if (state.softThru && state.output && state.output->isOpen()) {
            const auto t = msg.type();
            const bool channelVoice =
                t == midi::MessageType::NoteOn || t == midi::MessageType::NoteOff ||
                t == midi::MessageType::PitchBend || t == midi::MessageType::ChannelAftertouch ||
                t == midi::MessageType::PolyAftertouch || t == midi::MessageType::ControlChange;
            if (channelVoice && msg.rawSize() > 0)
                state.output->send(std::vector<uint8_t>(msg.raw(), msg.raw() + msg.rawSize()));
        }

        // 컨트롤러 CC: MIDI Learn 또는 매핑된 파라미터 조절
        if (msg.type() == midi::MessageType::ControlChange) {
            const uint8_t cc = msg.data1();
            if (state.learnArmed) {
                // 학습: 방금 움직인 노브의 CC를 대상 파라미터에 연결
                state.midiMap.bind(cc, msg.channel(), state.learnTarget);
                state.learnArmed = false;
                state.statusMessage =
                    "MIDI Learn: CC " + std::to_string(cc) + " 연결됨";
            } else {
                mapping::ParamTarget target;
                if (state.midiMap.findTarget(cc, target)) {
                    // CC 값(0~127) -> 파라미터 범위로 적용 후 신스에 반영
                    mapping::applyNormalized(state.synthParams, target,
                                             (float)msg.data2() / 127.0f);
                    if (state.synth) state.synth->setParams(state.synthParams);
                }
            }
        }

        // 녹음: Note On에서 시작 틱 기록, Note Off에서 트랙에 노트 확정
        if (canRecord) {
            const uint8_t note = msg.data1();
            if (msg.type() == midi::MessageType::NoteOn) {
                state.openRecNotes[note] = {true, nowTick, msg.data2()};
            } else if (msg.type() == midi::MessageType::NoteOff) {
                auto& open = state.openRecNotes[note];
                if (open.active) {
                    const uint32_t dur = nowTick > open.startTick ? nowTick - open.startTick : 1;
                    auto& track = state.song.tracks[state.selectedTrack];
                    track.addNote(open.startTick, dur, note, open.velocity);
                    seq::adoptNoteIntoClips(track, note, open.startTick);
                    track.sortEvents();
                    open.active = false;
                }
            }
        }
    }
}

// 한 박의 틱 수. 6/8은 8분음표가 한 박이다.
uint32_t songTicksPerBeat(const AppState& state) {
    static const int kSigUnit[3] = {4, 4, 8};
    const int si = std::clamp(state.metroSigIndex, 0, 2);
    const uint32_t ppqn = (uint32_t)std::max(1, state.song.ppqn);
    return std::max<uint32_t>(1, kSigUnit[si] == 8 ? ppqn / 2 : ppqn);
}

// 한 마디의 틱 수. 4/4=ppqn*4, 3/4=ppqn*3, 6/8=(ppqn/2)*6=ppqn*3.
uint32_t songTicksPerBar(const AppState& state) {
    static const int kSigBeats[3] = {4, 3, 6};
    const int si = std::clamp(state.metroSigIndex, 0, 2);
    return std::max<uint32_t>(1, songTicksPerBeat(state) * (uint32_t)kSigBeats[si]);
}

// 카운트인 길이(틱): 사용자가 정한 클릭 횟수 x 박 단위(박자 설정의 4분/8분).
uint32_t countInTicks(const AppState& state) {
    const uint32_t beats = (uint32_t)std::clamp(state.countInBeats, 1, 16);
    return std::max<uint32_t>(1, songTicksPerBeat(state) * beats);
}

// ---------------------------------------------------------
// 루프/메트로놈을 매 프레임 플레이어에 반영 (트랜스포트 창을 닫아도 동작)
// ---------------------------------------------------------
void applyTransportState(AppState& state) {
    // 레벨 미터 원시 피크: 프레임당 한 번만 걷는다. poll은 읽으면 0으로
    // 리셋되는 소비형이라 여러 창이 직접 부르면 서로 피크를 뺏는다.
    if (state.audioClips) {
        for (int b = 0; b < 16; ++b) state.busPeakCache[b] = state.audioClips->pollBusPeak(b);
        state.audioClips->pollMasterPeak(state.masterPeakCache[0], state.masterPeakCache[1]);

        // 트랙 프리즈 -> 버스 플래그. 같은 버스(채널)를 쓰는 트랙이 "전부"
        // 프리즈일 때만 버스를 끈다 (하나만 프리즈면 다른 트랙이 죽으므로).
        bool anyT[16] = {}, allF[16];
        for (int b = 0; b < 16; ++b) allF[b] = true;
        for (const auto& tr : state.song.tracks) {
            const int b = tr.channel & 0x0F;
            anyT[b] = true;
            if (!tr.frozen) allF[b] = false;
        }
        for (int b = 0; b < 16; ++b) state.audioClips->setBusFrozen(b, anyT[b] && allF[b]);
    }

    // 타임라인 마디 수: 내용(오디오 포함)이 끝 5마디 이내로 다가오면
    // 20마디 여유를 미리 확보한다. 줄지 않고 늘기만 한다.
    {
        const uint32_t tpbb = songTicksPerBar(state);
        const uint32_t contentBars =
            tpbb > 0 ? (contentTicksWithAudio(state) + tpbb - 1) / tpbb : 0;
        if (contentBars + 5 >= state.timelineBars) state.timelineBars = contentBars + 20;
        if (state.timelineBars < 20) state.timelineBars = 20;
    }

    if (!state.player) return;
    const uint32_t tpb = songTicksPerBar(state);
    state.player->setLoop(state.loopEnabled, state.loopStartTick, state.loopEndTick);
    state.player->setMetronome(state.metronome ||
                               (state.countIn && (state.recording || state.audioRecPending ||
                                                  state.player->isCountingIn())));
    // 메트로놈이 꺼져 있으면(카운트인만) 시작점 이후 클릭을 플레이어가 틱 기준으로
    // 정확히 끊는다 — 마지막 강조 뒤에 클릭이 하나 더 새는 것을 막는다.
    state.player->setMetronomeCountInOnly(!state.metronome);
    // 카운트인+루프면 매 바퀴 되감을 때도 같은 길이만큼 클릭 후 다시 시작한다
    state.player->setLoopCountIn(state.countIn ? countInTicks(state) : 0);
    // 클릭 음 높이/박자를 플레이어(생성)와 엔진(샘플 매칭)에 함께 반영
    state.player->setClickNotes((uint8_t)state.metroClickNote, (uint8_t)state.countInClickNote,
                                (uint8_t)state.accentClickNote,
                                (uint8_t)state.countInAccentClickNote);
    {
        static const int kSigBeats[3] = {4, 3, 6};
        static const int kSigUnit[3] = {4, 4, 8};
        const int si = std::clamp(state.metroSigIndex, 0, 2);
        state.player->setMetroSignature(kSigBeats[si], kSigUnit[si]);
    }
    state.player->setClickVolumes(state.metroVolume, state.countInVolume);
    if (state.audioClips)
        state.audioClips->setClickPitches((uint8_t)state.metroClickNote,
                                          (uint8_t)state.countInClickNote,
                                          (uint8_t)state.accentClickNote,
                                          (uint8_t)state.countInAccentClickNote);
    state.player->setBpm(state.song.bpm); // 재생 중 BPM 변경 실시간 반영
    // 재생 중엔 플레이헤드가 현재 위치를 따라간다(정지 시 그 자리에 머문다).
    if (state.player->isPlaying()) state.playPosTick = state.player->currentTick();

    // 카운트인이 끝나면 실제 캡처와 오디오 클립 재생을 시작한다.
    // 벽시계 타이머가 아니라 "플레이어의 카운트인 상태"로 판정해야 플레이어
    // 시계와 정확히 맞는다 (타이머면 카운트인 도중에 시작될 수 있다).
    if (state.audioRecPending && state.audioInput && state.player &&
        state.player->isPlaying() && !state.player->isCountingIn()) {
        state.audioRecPending = false;
        state.audioInput->startRecording();
        if (state.audioClips)
            state.audioClips->startAudio(tickToFrame(state, state.audioRecStartTick));
        state.statusMessage = "녹음 시작";
    }

    // 재생/루프 카운트인이 끝나면 대기시켜 둔 오디오 클립 재생을 시작한다
    if (state.audioStartPending && state.player->isPlaying() &&
        !state.player->isCountingIn()) {
        state.audioStartPending = false;
        if (state.audioClips)
            state.audioClips->startAudio(tickToFrame(state, state.playPosTick));
        state.statusMessage = "재생 중";
    }

    // 루프 되감김 감지(위치가 크게 뒤로 점프): 오디오 클립 재생을 루프 시작으로
    // 재동기화하고, 루프 녹음 중이면 한 바퀴에서 자동 펀치아웃(재생은 계속).
    if (state.player->isPlaying() && !state.audioRecPending) {
        const uint32_t tpbw = songTicksPerBar(state);
        if (state.playPosTick + tpbw / 4 < state.prevPlayPosTick) {
            if (state.audioClips) {
                if (state.player->isCountingIn()) {
                    // 루프 카운트인: 클릭이 끝날 때까지 오디오도 대기시킨다
                    state.audioClips->stopAudio();
                    state.audioClips->seekAudio(tickToFrame(state, state.playPosTick));
                    state.audioStartPending = true;
                } else {
                    state.audioClips->seekAudio(tickToFrame(state, state.playPosTick));
                }
            }
            if (state.audioRecTrack >= 0 && state.loopEnabled)
                stopAudioRecording(state, /*alsoStopTransport=*/false); // 테이크 확정
            if (state.loopEnabled) ++state.loopCount; // 연습 반복 카운터
        }
    }
    state.prevPlayPosTick = state.playPosTick;
    // 트랙 볼륨/팬을 오디오 엔진에 실시간 반영
    rebuildAudioMix(state);

    // 마스터 볼륨·게인·팬: 신스/VST/클립/모니터 모두에 적용되도록 엔진 최종 단에 전달.
    if (state.audioClips) {
        state.audioClips->setMasterGain(state.song.masterVolume * state.song.masterGain);
        state.audioClips->setMasterPan(state.song.masterPan);
    }

    // 트랙 볼륨·게인/팬/뮤트를 그 트랙의 MIDI 채널에 전달 -> 신스 트랙에도 믹서가 적용된다.
    // (같은 채널을 쓰는 트랙이 여럿이면 뒤 트랙이 이긴다 — MIDI는 채널 단위 믹싱)
    if (state.synth)
        for (const auto& t : state.song.tracks) {
            // 오토메이션 곡선이 있으면 재생 헤드 시점 값이 페이더를 대신한다
            const float av = seq::autoValueAt(t.volAuto, state.playPosTick, t.volume);
            const float ap = seq::autoValueAt(t.panAuto, state.playPosTick, t.pan);
            state.synth->setChannelMix(t.channel, t.muted ? 0.0f : av * t.gain, ap);
            // 센드 노브 -> 리턴 버스 (뮤트면 리버브에도 안 보낸다)
            if (state.audioClips)
                state.audioClips->setBusSend(t.channel & 0x0F,
                                             t.muted ? 0.0f : t.sendLevel);
        }
    // ASIO 모니터 입력이 담당 트랙의 FX 체인을 타도록 버스를 알려준다
    if (state.audioClips)
        state.audioClips->setMonitorBus(
            state.asioTrack >= 0 && state.asioTrack < (int)state.song.tracks.size()
                ? (state.song.tracks[(std::size_t)state.asioTrack].channel & 0x0F)
                : -1);

    // 녹음 버퍼 청크를 앞서서 할당해 녹음 길이 제한을 없앤다 (녹음 중에만 동작).
    if (state.audioInput) state.audioInput->pumpRecording();

    // VSTi 출력 라우팅: 지정 트랙의 채널 버스로 (트랙 채널이 바뀌어도 따라간다).
    if (state.vst) {
        int bus = -1;
        if (state.vstInstrumentTrack >= 0 &&
            state.vstInstrumentTrack < (int)state.song.tracks.size())
            bus = state.song.tracks[state.vstInstrumentTrack].channel & 0x0F;
        state.vst->setInstrumentBus(bus);
    }

    // 모니터 게인: ASIO로 모니터 중인 트랙의 볼륨·게인(뮤트면 0)을 입력에 적용.
    if (state.audioInput) {
        float mg = 1.0f;
        if (state.asioTrack >= 0 && state.asioTrack < (int)state.song.tracks.size()) {
            const auto& mt = state.song.tracks[state.asioTrack];
            mg = mt.muted ? 0.0f : mt.volume * mt.gain;
        }
        state.audioInput->setMonitorGain(mg);
    }

}

// ---------------------------------------------------------
// 트랙/선택 편집 헬퍼 (메뉴 바·트랙 뷰·피아노 롤에서 공용)
// ---------------------------------------------------------
// 트랙 유형 배지: 채널 10 = 드럼(주황), isGuitar = 기타(초록).
// 이름만으로는 유형이 안 보여서, 트랙이 나오는 모든 목록에 함께 그린다.
bool trackTypeBadge(const seq::Track& t, bool sameLine) {
    const bool drum = (t.channel & 0x0F) == 9;
    if (drum)
        ImGui::TextColored(ImVec4(1.0f, 0.64f, 0.25f, 1.0f), "[드럼]");
    else if (t.isGuitar)
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "[기타]");
    else
        return false;
    if (sameLine) ImGui::SameLine(0.0f, 4.0f);
    return true;
}

// Shift+클릭으로 트랙을 타브 창 표시 목록에 토글한다. 클릭한 순서대로 쌓인다.
void toggleTabTrack(AppState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto it = std::find(state.tabTracks.begin(), state.tabTracks.end(), trackIndex);
    if (it != state.tabTracks.end()) {
        state.tabTracks.erase(it);
        state.statusMessage = "타브 표시에서 뺌: " +
                              state.song.tracks[(std::size_t)trackIndex].name;
    } else {
        state.tabTracks.push_back(trackIndex);
        state.showTab = true;
        state.statusMessage =
            "타브 표시 추가 (" + std::to_string(state.tabTracks.size()) + "번째): " +
            state.song.tracks[(std::size_t)trackIndex].name;
    }
}

// 새 트랙을 만들고 선택한다.
void addTrack(AppState& state) {
    state.snapshot();
    seq::Track t;
    t.name = "Track " + std::to_string(state.song.tracks.size() + 1);
    t.channel = (uint8_t)(state.song.tracks.size() & 0x0F);
    state.song.tracks.push_back(t);
    state.selectedTrack = (int)state.song.tracks.size() - 1;
    addTrackEq(state, state.song.tracks.back()); // 기본 EQ 장착
    state.statusMessage = "트랙 생성";
}

// 새 기타 트랙을 만들고 선택 + 타브 악보 창을 연다.
void addGuitarTrack(AppState& state) {
    state.snapshot();
    int guitars = 0;
    for (const auto& t : state.song.tracks)
        if (t.isGuitar) ++guitars;
    seq::Track t;
    t.name = guitars == 0 ? "기타" : ("기타 " + std::to_string(guitars + 1));
    t.channel = (uint8_t)(state.song.tracks.size() & 0x0F);
    if ((t.channel & 0x0F) == 9) t.channel = 10; // 드럼 채널(10)은 피한다
    t.isGuitar = true;
    state.song.tracks.push_back(std::move(t));
    state.selectedTrack = (int)state.song.tracks.size() - 1;
    addTrackEq(state, state.song.tracks.back());
    state.showTab = true; // 타브 악보 창 바로 열기
    state.statusMessage = "기타 트랙 생성 — 노트는 피아노 롤에서, 타브는 이 창에서";
}

// 새 연습 트랙(기타 연습 창 전용)을 만들고 선택한다.
// practice=true라서 트랙 뷰·믹서·피아노 롤에는 나타나지 않는다 — 곡 작업용
// 트랙과 연습용 악보가 한 목록에 섞이지 않게 하려는 것.
void addPracticeTrack(AppState& state) {
    state.snapshot();
    int n = 0;
    for (const auto& t : state.song.tracks)
        if (t.practice) ++n;
    seq::Track t;
    t.name = n == 0 ? "연습 기타" : ("연습 기타 " + std::to_string(n + 1));
    t.channel = (uint8_t)(state.song.tracks.size() & 0x0F);
    if ((t.channel & 0x0F) == 9) t.channel = 10; // 드럼 채널(10)은 피한다
    t.isGuitar = true;
    t.practice = true;
    state.song.tracks.push_back(std::move(t));
    state.practiceTrack = (int)state.song.tracks.size() - 1;
    addTrackEq(state, state.song.tracks.back());
    state.showTab = true;
    state.statusMessage = "연습 트랙 생성 — 타브를 가져와 연습하세요";
}

// 새 드럼 트랙(채널 10)을 만들고 선택 + 드럼 트랙 에디터를 연다.
void addDrumTrack(AppState& state) {
    state.snapshot();
    int drums = 0; // 이름 번호: 기존 드럼 트랙 수 + 1
    for (const auto& t : state.song.tracks)
        if ((t.channel & 0x0F) == 9) ++drums;
    seq::Track t;
    t.name = drums == 0 ? "드럼" : ("드럼 " + std::to_string(drums + 1));
    t.channel = 9; // GM 드럼 채널 10
    state.song.tracks.push_back(std::move(t));
    state.selectedTrack = (int)state.song.tracks.size() - 1;
    addTrackEq(state, state.song.tracks.back());
    state.showDrums = true; // 바로 찍을 수 있게 에디터를 연다
    state.statusMessage = "드럼 트랙 생성";
}

// 특정 트랙을 삭제한다(재생 꼬임 방지 위해 정지 후).
// 트랙 인덱스를 기억하는 모든 상태(선택/녹음/모니터/라우팅)를 함께 재매핑한다.
void deleteTrack(AppState& state, int index) {
    if (index < 0 || index >= (int)state.song.tracks.size()) return;
    // 이 트랙이 오디오 입력을 점유 중이면 먼저 놓는다
    if (state.audioRecTrack == index) stopAudioRecording(state, /*alsoStopTransport=*/true);
    if (state.asioTrack == index && state.audioInput) {
        state.audioInput->stopAsio();
        state.asioTrack = -1;
    }
    stopTransport(state);
    state.snapshot();
    // 오디오 엔진은 악기/이펙트/EQ를 트랙이 아니라 "채널 번호"로 들고 있다.
    // 트랙만 지우고 엔진 채널을 안 비우면, 같은 채널을 재사용하는 새 트랙에
    // 옛 악기·이펙트·EQ가 그대로 남는다(사용자가 겪은 버그). 지우는 트랙의
    // 채널을 다른 트랙이 안 쓸 때만 엔진 상태도 함께 비운다.
    const int goneChannel = state.song.tracks[(std::size_t)index].channel & 0x0F;
    state.song.tracks.erase(state.song.tracks.begin() + index);
    bool channelStillUsed = false;
    for (const auto& t : state.song.tracks)
        if ((t.channel & 0x0F) == goneChannel) { channelStillUsed = true; break; }
    if (!channelStillUsed && state.vst) {
        state.vst->clearTrackInstrument(goneChannel);
        state.vst->clearTrackEffects(goneChannel); // EQ 포함 (EQ도 이펙트 체인에 있다)
    }

    const auto remap = [&](int x) { // 삭제된 트랙 = -1, 그 뒤는 한 칸 당김
        return x < index ? x : (x == index ? -1 : x - 1);
    };
    state.selectedTrack = remap(state.selectedTrack);
    if (state.selectedTrack < 0) state.selectedTrack = 0;
    if (state.selectedTrack >= (int)state.song.tracks.size())
        state.selectedTrack = std::max(0, (int)state.song.tracks.size() - 1);
    state.selClipTrack = state.selClipTrack >= 0 ? remap(state.selClipTrack) : -1;
    if (state.selClipTrack < 0) state.selClipIndex = -1;
    state.selMidiClipTrack =
        state.selMidiClipTrack >= 0 ? remap(state.selMidiClipTrack) : -1;
    if (state.selMidiClipTrack < 0) state.selMidiClipIndex = -1;
    if (state.clipRange.track >= 0) {
        state.clipRange.track = remap(state.clipRange.track);
        if (state.clipRange.track < 0) state.clipRange = AppState::ClipRangeSel{};
    }
    if (state.audioRecTrack >= 0) state.audioRecTrack = remap(state.audioRecTrack);
    if (state.asioTrack >= 0) state.asioTrack = remap(state.asioTrack);
    if (state.vstInstrumentTrack >= 0)
        state.vstInstrumentTrack = remap(state.vstInstrumentTrack);
    std::set<std::pair<int, int>> ns;
    for (const auto& sc : state.selClips) {
        const int nt = remap(sc.first);
        if (nt >= 0) ns.insert({nt, sc.second});
    }
    state.selClips.swap(ns);
    state.selectedNotes.clear();
    state.statusMessage = "트랙 삭제";
}

// ---------------------------------------------------------
// 오디오 녹음 시작/정지 (R키·Space·트랙의 녹음 버튼이 공용으로 쓴다)
//  - 카운트인이 켜져 있으면 한 마디 메트로놈 뒤에 실제 캡처가 시작된다.
//  - 루프가 켜져 있으면 루프 시작부터 녹음하고, 한 바퀴 돌면 자동 펀치아웃.
// ---------------------------------------------------------
// 녹음을 시작하고 곧바로 재생도 시작한다. 이미 다른 트랙이 녹음 중이면 마무리한다.
bool startAudioRecording(AppState& state, int trackIndex) {
    if (!state.audioInput || !state.player) return false;
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) {
        state.statusMessage = "녹음할 트랙을 먼저 선택하세요";
        return false;
    }
    auto* in = state.audioInput;

    // 다른 트랙이 녹음 중이면 먼저 마무리해 그 트랙에 붙인다.
    if (state.audioRecTrack >= 0 && state.audioRecTrack < (int)state.song.tracks.size()) {
        auto prev = in->stopRecording();
        if (prev) {
            prev->startTick = state.audioRecStartTick;
            state.song.tracks[state.audioRecTrack].clips.push_back(std::move(prev));
        }
        state.audioRecTrack = -1;
        state.audioRecPending = false;
    }

    auto& track = state.song.tracks[trackIndex];
    // 연습 트랙은 MIDI 쪽 선택을 건드리지 않는다 (트랙 뷰에 보이지도 않는 트랙이
    // 선택되면 피아노 롤 등이 엉뚱한 트랙을 편집하게 된다).
    if (!track.practice) state.selectedTrack = trackIndex;

    // ASIO가 캡처 중이 아니면 이 트랙 설정으로 연다.
    bool ready = in->asioActive();
    if (!ready && in->startAsio(state.asioDeviceIndex, track.inputChannelMode)) {
        state.asioTrack = trackIndex;
        ready = true;
    }
    if (!ready) {
        state.statusMessage = "ASIO를 열 수 없어 녹음할 수 없습니다";
        return false;
    }

    // 루프면 루프 시작부터 (한 바퀴 돌면 applyTransportState가 자동 펀치아웃)
    const uint32_t tpb = songTicksPerBar(state);
    uint32_t startTick = state.playPosTick;
    if (state.loopEnabled) startTick = state.loopStartTick;
    state.playPosTick = startTick;
    state.audioRecStartTick = startTick; // 클립을 이 위치에 배치한다
    state.audioRecTrack = trackIndex;

    if (state.countIn) {
        // 한 마디 카운트인(박자 설정 기준): MIDI는 프리롤로 시작(메트로놈이 울림),
        // 실제 캡처와 오디오 클립 재생은 카운트인이 끝난 뒤 applyTransportState가 시작.
        const uint32_t preRoll = countInTicks(state);
        if (state.audioClips) {
            state.audioClips->stopAudio();
            state.audioClips->seekAudio(tickToFrame(state, startTick));
        }
        state.player->play(state.song, startTick, /*keepAlive=*/true, preRoll);
        state.audioRecPending = true;
        state.audioRecPendingUntil =
            ImGui::GetTime() + seq::ticksToSeconds(preRoll,
                                                   seq::bpmAtTick(state.song, startTick),
                                                   state.song.ppqn);
        state.statusMessage = "카운트인... (한 마디 뒤 녹음 시작)";
    } else {
        in->startRecording();
        state.statusMessage = "녹음 시작: " + track.name;
        startPlayback(state); // 녹음과 동시에 재생 (기존 트랙에 맞춰 연주)
    }
    return true;
}

// 녹음을 멈춰 클립을 트랙에 붙인다. alsoStopTransport=false면 재생은 계속
// (루프 녹음 펀치아웃: 테이크를 붙이고 곧바로 루프로 들어본다).
void stopAudioRecording(AppState& state, bool alsoStopTransport) {
    if (!state.audioInput || state.audioRecTrack < 0) return;
    auto* in = state.audioInput;
    const int idx = state.audioRecTrack;
    state.audioRecTrack = -1;

    if (state.audioRecPending) {
        // 카운트인 중 취소: 캡처가 시작되기 전이라 클립 없음
        state.audioRecPending = false;
        stopTransport(state);
        state.statusMessage = "녹음 취소 (카운트인 중)";
        return;
    }

    auto clip = in->stopRecording();
    if (clip && idx < (int)state.song.tracks.size()) {
        state.snapshot();
        clip->startTick = state.audioRecStartTick; // 녹음을 시작한 지점에 놓는다
        // 여러 테이크: 기존 클립을 교체하지 않고 추가한다.
        clip->name = "녹음 " + std::to_string(state.song.tracks[idx].clips.size() + 1);
        state.song.tracks[idx].clips.push_back(std::move(clip));
        state.statusMessage = "녹음 완료: " + state.song.tracks[idx].name;
    } else {
        state.statusMessage = "녹음 데이터가 없습니다";
    }
    if (!in->asioActive()) in->stopInput();
    if (alsoStopTransport) stopTransport(state); // 녹음을 멈추면 재생도 멈춘다
    else refreshPlaybackIfPlaying(state);        // 재생 유지 (새 클립 즉시 반영)
}

// 클립을 atTick 지점에서 둘로 자른다 (가위). 오른쪽 조각은 바로 뒤에 삽입.
void splitTrackClip(AppState& state, int trackIndex, int clipIndex, uint32_t atTick) {
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto& t = state.song.tracks[trackIndex];
    if (clipIndex < 0 || clipIndex >= (int)t.clips.size() || !t.clips[(std::size_t)clipIndex])
        return;
    auto& clip = *t.clips[(std::size_t)clipIndex];
    if (atTick <= clip.startTick) return;
    // 클립 내 상대 초 = 절대 초 차이 (템포 맵 반영)
    const double atSec = seq::songTickToSec(state.song, atTick) -
                         seq::songTickToSec(state.song, clip.startTick);
    state.snapshot();
    auto right = audio::splitClipAt(clip, atSec);
    if (!right) {
        state.statusMessage = "자를 수 없는 위치입니다 (클립 끝에 너무 가까움)";
        return;
    }
    right->startTick = atTick;
    t.clips.insert(t.clips.begin() + clipIndex + 1, std::move(right));
    state.selClips.clear(); // 인덱스가 밀렸으니 다중 선택 해제
    state.clipRange = AppState::ClipRangeSel{};
    state.statusMessage = "클립 분할";
    refreshPlaybackIfPlaying(state);
}

// 트랙의 특정 오디오 클립을 지운다 (clipIndex<0 이면 마지막 클립). 트랙은 남는다.
void deleteTrackClip(AppState& state, int trackIndex, int clipIndex) {
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto& t = state.song.tracks[trackIndex];
    if (t.clips.empty()) return;
    if (clipIndex < 0) clipIndex = (int)t.clips.size() - 1; // 기본: 가장 최근 클립
    if (clipIndex >= (int)t.clips.size()) return;
    state.snapshot();
    const std::string name = t.clips[(std::size_t)clipIndex]
                                 ? t.clips[(std::size_t)clipIndex]->name
                                 : std::string("클립");
    t.clips.erase(t.clips.begin() + clipIndex);
    state.selClips.clear(); // 인덱스가 밀렸으니 다중 선택 해제
    state.clipRange = AppState::ClipRangeSel{};
    state.statusMessage = "오디오 삭제: " + name;
    refreshPlaybackIfPlaying(state);
}

// 트랙을 from에서 to 위치로 옮기고, 트랙 인덱스를 저장한 모든 상태를 재매핑한다.
void moveTrackTo(AppState& state, int from, int to) {
    const int n = (int)state.song.tracks.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    stopTransport(state);
    state.snapshot();
    auto t = std::move(state.song.tracks[(std::size_t)from]);
    state.song.tracks.erase(state.song.tracks.begin() + from);
    state.song.tracks.insert(state.song.tracks.begin() + to, std::move(t));

    const auto remap = [&](int x) {
        if (x == from) return to;
        if (from < to && x > from && x <= to) return x - 1;
        if (to < from && x >= to && x < from) return x + 1;
        return x;
    };
    state.selectedTrack = remap(state.selectedTrack);
    if (state.selClipTrack >= 0) state.selClipTrack = remap(state.selClipTrack);
    if (state.clipRange.track >= 0) state.clipRange.track = remap(state.clipRange.track);
    if (state.audioRecTrack >= 0) state.audioRecTrack = remap(state.audioRecTrack);
    if (state.asioTrack >= 0) state.asioTrack = remap(state.asioTrack);
    if (state.vstInstrumentTrack >= 0)
        state.vstInstrumentTrack = remap(state.vstInstrumentTrack);
    std::set<std::pair<int, int>> ns;
    for (const auto& sc : state.selClips) ns.insert({remap(sc.first), sc.second});
    state.selClips.swap(ns);
    state.statusMessage = "트랙 순서 변경: " + std::to_string(from + 1) + " → " +
                          std::to_string(to + 1);
}

// ---------------------------------------------------------
// 트랙 프리즈: MIDI를 그 트랙의 악기+FX 체인을 거친 오디오로 구워 클립으로
// 붙이고, 이후 이 트랙의 MIDI 재생과 VST 처리를 건너뛴다 (CPU 절약).
// ---------------------------------------------------------
void freezeTrack(AppState& state, int trackIndex) {
    if (!state.audioClips || trackIndex < 0 || trackIndex >= (int)state.song.tracks.size())
        return;
    auto& t = state.song.tracks[(std::size_t)trackIndex];
    if (t.frozen) return;
    if (t.events.empty()) {
        state.statusMessage = "프리즈할 MIDI가 없습니다";
        return;
    }
    const double sr = state.audioClips->engineSampleRate();
    if (sr <= 0.0) return;
    stopTransport(state);
    silenceOutput(state);
    state.snapshot();

    const int bus = t.channel & 0x0F;
    const uint32_t endTick = t.lengthTicks();
    const int64_t endFrame = (int64_t)(seq::songTickToSec(state.song, endTick) * sr);
    const int64_t tailFrames = (int64_t)(2.0 * sr); // 릴리스/리버브 여운

    std::vector<seq::MidiEvent> evs = t.events;
    std::stable_sort(evs.begin(), evs.end(),
                     [](const seq::MidiEvent& a, const seq::MidiEvent& b) {
                         return a.tick < b.tick;
                     });

    // 렌더 조건: 채널 믹스를 1/중앙으로 (페이더는 클립 재생 때 다시 적용되므로
    // 여기서도 곱하면 이중 적용), 기존 오디오 클립은 믹스에서 제외.
    if (state.synth) state.synth->setChannelMix(bus, 1.0f, 0.0f);
    state.audioClips->setAudioMix(std::make_shared<const std::vector<audio::AudioMixClip>>());

    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 2;
    clip->sampleRate = (int)sr;
    clip->name = "프리즈: " + t.name;
    clip->freezeBounce = true;
    clip->startTick = 0;
    clip->pcm.reserve((std::size_t)((endFrame + tailFrames) * 2));

    constexpr unsigned kBlock = 1024;
    std::vector<float> block((std::size_t)kBlock * 2);
    state.audioClips->beginOfflineRender(0);
    std::size_t ei = 0;
    for (int64_t f = 0; f < endFrame; f += kBlock) {
        const unsigned n = (unsigned)std::min<int64_t>(kBlock, endFrame - f);
        const double blockEndSec = (double)(f + n) / sr;
        while (ei < evs.size() &&
               seq::songTickToSec(state.song, evs[ei].tick) < blockEndSec + 1e-9) {
            state.audioClips->queueMidi(evs[ei].status, evs[ei].data1, evs[ei].data2);
            ++ei;
        }
        state.audioClips->renderOfflineBlockBus(bus, block.data(), n, /*preFx=*/true);
        clip->pcm.insert(clip->pcm.end(), block.begin(), block.begin() + (std::size_t)n * 2);
    }
    // 남은 노트를 릴리스시키고 여운을 렌더한다
    state.audioClips->queueMidi((uint8_t)(midi::kStatusControlChange | bus), 123, 0);
    for (int64_t f = 0; f < tailFrames; f += kBlock) {
        const unsigned n = (unsigned)std::min<int64_t>(kBlock, tailFrames - f);
        state.audioClips->renderOfflineBlockBus(bus, block.data(), n, /*preFx=*/true);
        clip->pcm.insert(clip->pcm.end(), block.begin(), block.begin() + (std::size_t)n * 2);
    }
    state.audioClips->endOfflineRender();

    clip->trimLen = (int64_t)clip->frames();
    clip->buildPeaks();
    t.clips.push_back(std::move(clip));
    t.frozen = true; // 버스 프리즈 플래그는 applyTransportState가 매 프레임 반영
    state.statusMessage = "트랙 프리즈 완료: " + t.name + " (해제하면 MIDI로 복귀)";
}

void unfreezeTrack(AppState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto& t = state.song.tracks[(std::size_t)trackIndex];
    if (!t.frozen) return;
    stopTransport(state);
    state.snapshot();
    t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                 [](const std::shared_ptr<audio::AudioClip>& c) {
                                     return c && c->freezeBounce;
                                 }),
                  t.clips.end());
    t.frozen = false;
    state.statusMessage = "프리즈 해제: " + t.name;
}

// ---------------------------------------------------------
// 오디오 클립 복사/붙여넣기 (Ctrl+C/V + 우클릭 메뉴)
// ---------------------------------------------------------
void copySelectedClip(AppState& state) {
    if (state.selClipTrack < 0 || state.selClipTrack >= (int)state.song.tracks.size()) return;
    auto& t = state.song.tracks[(std::size_t)state.selClipTrack];
    if (state.selClipIndex < 0 || state.selClipIndex >= (int)t.clips.size()) return;
    if (!t.clips[(std::size_t)state.selClipIndex]) return;
    // 깊은 복사: 이후 원본을 트림/삭제해도 클립보드는 그대로다
    state.clipClipboard =
        std::make_shared<audio::AudioClip>(*t.clips[(std::size_t)state.selClipIndex]);
    state.statusMessage = "클립 복사됨 (Ctrl+V: 재생 위치에 붙여넣기)";
}

void pasteClipAt(AppState& state, int trackIndex, uint32_t tick) {
    if (!state.clipClipboard) return;
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    state.snapshot();
    auto c = std::make_shared<audio::AudioClip>(*state.clipClipboard);
    c->startTick = tick;
    c->freezeBounce = false; // 복사본은 일반 클립으로
    auto& t = state.song.tracks[(std::size_t)trackIndex];
    t.clips.push_back(std::move(c));
    state.selectedTrack = trackIndex;
    state.selClipTrack = trackIndex;
    state.selClipIndex = (int)t.clips.size() - 1;
    state.selClips.clear();
    state.selClips.insert({trackIndex, state.selClipIndex});
    refreshPlaybackIfPlaying(state);
    state.statusMessage = "클립 붙여넣기: " + t.name;
}

// 트랙의 지정 클립들을 하나로 병합해 교체한다 (배치 그대로, 공백은 무음).
// 트림/배속/게인/페이드가 구워진 평범한 클립 하나가 남는다. 언두 가능.
void mergeTrackClips(AppState& state, int trackIndex, std::vector<int> indices) {
    if (trackIndex < 0 || trackIndex >= (int)state.song.tracks.size()) return;
    auto& t = state.song.tracks[(std::size_t)trackIndex];
    std::vector<audio::MergeItem> items;
    std::vector<int> valid;
    for (int ci : indices) {
        if (ci < 0 || ci >= (int)t.clips.size() || !t.clips[(std::size_t)ci]) continue;
        audio::MergeItem it;
        it.clip = t.clips[(std::size_t)ci].get();
        it.startSec = seq::songTickToSec(state.song, t.clips[(std::size_t)ci]->startTick);
        items.push_back(it);
        valid.push_back(ci);
    }
    if (items.size() < 2) return;
    const double engSr = state.audioClips ? state.audioClips->engineSampleRate() : 0.0;
    const int outRate = engSr > 0.0 ? (int)engSr : items[0].clip->sampleRate;
    auto merged = audio::mergeClips(items, outRate);
    if (!merged) {
        state.statusMessage = "병합할 수 없습니다";
        return;
    }
    double baseSec = 1e300;
    for (const auto& it : items) baseSec = std::min(baseSec, it.startSec);
    state.snapshot();
    merged->name = "병합: " + t.name;
    merged->startTick = (uint32_t)std::max(0.0, seq::songSecToTick(state.song, baseSec));
    std::sort(valid.begin(), valid.end(), std::greater<int>()); // 뒤에서부터 제거
    for (int ci : valid) t.clips.erase(t.clips.begin() + ci);
    t.clips.push_back(merged);
    state.selClips.clear(); // 인덱스가 밀렸으니 다중 선택은 결과 클립으로 재설정
    state.clipRange = AppState::ClipRangeSel{};
    state.selClipTrack = trackIndex;
    state.selClipIndex = (int)t.clips.size() - 1;
    state.selClips.insert({trackIndex, state.selClipIndex});
    refreshPlaybackIfPlaying(state);
    state.statusMessage = "클립 " + std::to_string(items.size()) + "개 병합";
}

// ---------------------------------------------------------
// 클립 구간 선택 (Ctrl+드래그): 그 부분만 복사하거나 삭제한다
// ---------------------------------------------------------

// 구간 선택이 유효하면 대상 클립을 돌려준다 (아니면 nullptr)
std::shared_ptr<audio::AudioClip> rangeSelClip(AppState& state) {
    const auto& r = state.clipRange;
    if (r.track < 0 || r.track >= (int)state.song.tracks.size()) return nullptr;
    auto& t = state.song.tracks[(std::size_t)r.track];
    if (r.clip < 0 || r.clip >= (int)t.clips.size()) return nullptr;
    if (r.t1 <= r.t0) return nullptr;
    return t.clips[(std::size_t)r.clip];
}

// 선택 구간만 클립보드로 (트림을 구간에 맞춘 깊은 복사 -> Ctrl+V로 붙여넣기)
void copyClipRange(AppState& state) {
    auto clip = rangeSelClip(state);
    if (!clip) return;
    const auto& r = state.clipRange;
    const double startSec = seq::songTickToSec(state.song, clip->startTick);
    const double dur = clip->durationSeconds();
    const double a =
        std::clamp(seq::songTickToSec(state.song, r.t0) - startSec, 0.0, dur);
    const double b =
        std::clamp(seq::songTickToSec(state.song, r.t1) - startSec, 0.0, dur);
    if (b - a < 0.01) return;
    auto c = std::make_shared<audio::AudioClip>(*clip);
    c->trimStart = clip->trimStart + (int64_t)(a * clip->sampleRate * clip->speed);
    c->trimLen =
        std::max<int64_t>(1, (int64_t)((b - a) * clip->sampleRate * clip->speed));
    c->fadeInSec = c->fadeOutSec = 0.0;
    c->freezeBounce = false;
    state.clipClipboard = std::move(c);
    state.statusMessage = "선택 구간 복사됨 (Ctrl+V: 재생 위치에 붙여넣기)";
}

// 선택 구간을 클립에서 잘라낸다.
// closeGap=false: 가운데를 빼고 좌우 조각을 남긴다 (자리는 공백).
// closeGap=true : 뒷부분을 앞으로 당겨 이어 붙이고, 하나의 클립으로 병합한다.
void deleteClipRange(AppState& state, bool closeGap) {
    auto clip = rangeSelClip(state);
    if (!clip) return;
    auto& r = state.clipRange;
    auto& t = state.song.tracks[(std::size_t)r.track];
    const double startSec = seq::songTickToSec(state.song, clip->startTick);
    const double dur = clip->durationSeconds();
    const double a =
        std::clamp(seq::songTickToSec(state.song, r.t0) - startSec, 0.0, dur);
    const double b =
        std::clamp(seq::songTickToSec(state.song, r.t1) - startSec, 0.0, dur);
    if (b - a < 0.01) return;
    state.snapshot();
    constexpr double kEps = 0.011; // splitClipAt의 10ms 가드보다 살짝 크게
    if (b >= dur - kEps && a <= kEps) {
        t.clips.erase(t.clips.begin() + r.clip); // 구간 = 클립 전체 -> 삭제
    } else if (b >= dur - kEps) {
        audio::splitClipAt(*clip, a); // 끝까지 선택: 오른쪽 조각을 버린다
    } else if (a <= kEps) {
        // 처음부터 선택: 오른쪽만 남긴다. 당겨 붙이기면 원래 시작 위치로
        auto right = audio::splitClipAt(*clip, b);
        if (right) {
            right->startTick = closeGap ? r.t0 : r.t1;
            t.clips[(std::size_t)r.clip] = std::move(right);
        }
    } else {
        auto right = audio::splitClipAt(*clip, b); // [b, 끝)
        audio::splitClipAt(*clip, a);              // 가운데 [a,b) 조각은 버린다
        if (right) {
            if (closeGap) {
                // 뒷부분을 구간 시작으로 당기고, 왼쪽 조각과 하나로 병합한다
                const double leftSec = seq::songTickToSec(state.song, clip->startTick);
                const double rightSec = seq::songTickToSec(state.song, r.t0);
                std::vector<audio::MergeItem> items = {{clip.get(), leftSec},
                                                       {right.get(), rightSec}};
                const double engSr =
                    state.audioClips ? state.audioClips->engineSampleRate() : 0.0;
                const int outRate = engSr > 0.0 ? (int)engSr : clip->sampleRate;
                if (auto merged = audio::mergeClips(items, outRate)) {
                    merged->name = clip->name;
                    merged->startTick = clip->startTick;
                    t.clips[(std::size_t)r.clip] = std::move(merged);
                } else { // 병합 실패 시에도 당겨 붙인 두 조각은 남긴다
                    right->startTick = r.t0;
                    t.clips.insert(t.clips.begin() + r.clip + 1, std::move(right));
                }
            } else {
                right->startTick = r.t1;
                t.clips.insert(t.clips.begin() + r.clip + 1, std::move(right));
            }
        }
    }
    r = AppState::ClipRangeSel{};
    state.selClips.clear();
    refreshPlaybackIfPlaying(state);
    state.statusMessage = closeGap ? "선택 구간 잘라내고 당겨 붙임" : "선택 구간 삭제";
}

// 선택된 노트들을 실제 NoteSpan으로 모은다.
std::vector<seq::NoteSpan> gatherSelected(const AppState& state, const seq::Track& track) {
    std::vector<seq::NoteSpan> out;
    for (const auto& n : seq::extractNotes(track))
        if (state.selectedNotes.count({n.note, n.startTick})) out.push_back(n);
    return out;
}

// ---------------------------------------------------------
// Ctrl+D 복제: 선택 노트/클립을 자기 길이만큼 뒤에 이어 붙인다
// ---------------------------------------------------------
void duplicateSelectedNotes(AppState& state) {
    if (state.selectedTrack >= (int)state.song.tracks.size()) return;
    auto& track = state.song.tracks[(std::size_t)state.selectedTrack];
    const auto sel = gatherSelected(state, track);
    if (sel.empty()) return;
    uint32_t minS = 0xFFFFFFFFu, maxE = 0;
    for (const auto& s : sel) {
        minS = std::min(minS, s.startTick);
        maxE = std::max(maxE, s.endTick);
    }
    uint32_t shift = maxE > minS ? maxE - minS : 0;
    if (shift == 0) return;
    // 복제 간격을 16분 격자로 올림: 노트 길이가 격자보다 짧아도 (드럼 등)
    // 복제본이 마디/박에 딱 맞는다. (예: 한 마디 패턴 -> 정확히 다음 마디)
    const uint32_t g16 = (uint32_t)std::max(1, state.song.ppqn / 4);
    shift = (shift + g16 - 1) / g16 * g16;
    state.snapshot();
    state.selectedNotes.clear(); // 새 복제본이 선택되어 연속 Ctrl+D가 가능하다
    for (const auto& s : sel) {
        const uint32_t dur = s.endTick > s.startTick ? s.endTick - s.startTick : 1;
        track.addNote(s.startTick + shift, dur, s.note, s.velocity);
        seq::adoptNoteIntoClips(track, s.note, s.startTick + shift);
        state.selectedNotes.insert({s.note, s.startTick + shift});
    }
    track.sortEvents();
    refreshPlaybackIfPlaying(state);
    state.statusMessage = "노트 " + std::to_string(sel.size()) + "개 복제";
}

void duplicateSelectedClips(AppState& state) {
    // 대상: Shift 다중 선택 (비면 단일 선택)
    std::vector<std::pair<int, int>> targets(state.selClips.begin(), state.selClips.end());
    if (targets.empty() && state.selClipTrack >= 0)
        targets.push_back({state.selClipTrack, state.selClipIndex});
    double minS = 1e300, maxE = 0.0;
    std::vector<std::pair<int, int>> valid;
    for (const auto& tc : targets) {
        if (tc.first < 0 || tc.first >= (int)state.song.tracks.size()) continue;
        const auto& t = state.song.tracks[(std::size_t)tc.first];
        if (tc.second < 0 || tc.second >= (int)t.clips.size() ||
            !t.clips[(std::size_t)tc.second])
            continue;
        const auto& c = *t.clips[(std::size_t)tc.second];
        minS = std::min(minS, (double)c.startTick);
        maxE = std::max(maxE, clipEndTick(c, state.song));
        valid.push_back(tc);
    }
    if (valid.empty() || maxE <= minS) return;
    const uint32_t shift = (uint32_t)(maxE - minS);
    state.snapshot();
    state.selClips.clear();
    for (const auto& tc : valid) {
        auto& t = state.song.tracks[(std::size_t)tc.first];
        auto copy = std::make_shared<audio::AudioClip>(*t.clips[(std::size_t)tc.second]);
        copy->startTick += shift;
        copy->freezeBounce = false;
        t.clips.push_back(std::move(copy));
        state.selClips.insert({tc.first, (int)t.clips.size() - 1});
        state.selClipTrack = tc.first;
        state.selClipIndex = (int)t.clips.size() - 1;
    }
    refreshPlaybackIfPlaying(state);
    state.statusMessage = "클립 " + std::to_string(valid.size()) + "개 복제";
}

void deleteSelectedNotes(AppState& state, seq::Track& track) {
    auto sel = gatherSelected(state, track);
    if (sel.empty()) return;
    state.snapshot();
    for (const auto& s : sel) seq::removeNote(track, s);
    track.sortEvents();
    state.selectedNotes.clear();
    state.statusMessage = "선택 노트 삭제";
    refreshPlaybackIfPlaying(state);
}

void copySelectedNotes(AppState& state, const seq::Track& track) {
    auto sel = gatherSelected(state, track);
    state.noteClipboard.clear();
    if (sel.empty()) return;
    uint32_t base = UINT32_MAX;
    for (const auto& s : sel) base = std::min(base, s.startTick);
    for (const auto& s : sel)
        state.noteClipboard.push_back(
            {(int32_t)(s.startTick - base), s.endTick - s.startTick, s.note, s.velocity});
    state.statusMessage = "복사: " + std::to_string(sel.size()) + "개";
}

// 클립보드를 재생 위치(플레이헤드)에 붙여넣고 붙인 노트를 새 선택으로 만든다.
void pasteNotes(AppState& state, seq::Track& track) {
    if (state.noteClipboard.empty()) return;
    state.snapshot();
    const uint32_t at = state.playPosTick;
    state.selectedNotes.clear();
    for (const auto& c : state.noteClipboard) {
        const uint32_t st = at + (uint32_t)std::max(0, c.dTick);
        track.addNote(st, std::max<uint32_t>(c.dur, 1), c.note, c.velocity);
        seq::adoptNoteIntoClips(track, c.note, st);
        state.selectedNotes.insert({c.note, st});
    }
    track.sortEvents();
    state.statusMessage = "붙여넣기: " + std::to_string(state.noteClipboard.size()) + "개";
    refreshPlaybackIfPlaying(state);
}

// 퀀타이즈: 선택 노트가 있으면 선택만, 없으면 트랙 전체를 격자에 스냅한다.
void quantizeNotes(AppState& state, seq::Track& track, uint32_t gridTicks) {
    if (gridTicks == 0) return;
    state.snapshot();
    int changed = 0;
    if (state.selectedNotes.empty()) {
        // 전체 퀀타이즈: 이동된 노트를 선택 상태(노란색)로 만들어 결과가 보이게 한다
        const auto before = seq::extractNotes(track);
        changed = seq::quantizeTrack(track, gridTicks);
        if (changed > 0) {
            state.selectedNotes.clear();
            for (const auto& s : before) {
                const uint32_t q = ((s.startTick + gridTicks / 2) / gridTicks) * gridTicks;
                if (q != s.startTick) state.selectedNotes.insert({s.note, q});
            }
        }
    } else {
        auto sel = gatherSelected(state, track);
        std::set<std::pair<uint8_t, uint32_t>> newSel;
        for (const auto& s : sel) {
            const uint32_t q = ((s.startTick + gridTicks / 2) / gridTicks) * gridTicks;
            if (q != s.startTick) {
                const uint32_t dur = s.endTick > s.startTick ? s.endTick - s.startTick : 1;
                seq::removeNote(track, s);
                track.addNote(q, dur, s.note, s.velocity);
                seq::adoptNoteIntoClips(track, s.note, q);
                ++changed;
            }
            newSel.insert({s.note, q}); // 선택 키가 새 위치로 바뀐다
        }
        if (changed) track.sortEvents();
        state.selectedNotes = std::move(newSel);
    }
    state.statusMessage = changed > 0 ? "퀀타이즈: " + std::to_string(changed) + "개 이동"
                                      : "퀀타이즈: 이미 격자에 맞음";
    refreshPlaybackIfPlaying(state);
}

// 노트 추출 캐시: 트랙 뷰가 매 프레임 트랙마다 extractNotes를 부르면 큰 곡에서
// 프레임이 떨어진다. 트랙의 editStamp(+크기/내용 프로브)가 그대로면 지난 결과를
// 재사용한다. GUI 스레드 전용.
const std::vector<seq::NoteSpan>& cachedNotes(const seq::Track& track, int trackIndex) {
    struct Entry {
        uint64_t stamp = ~0ull;
        std::size_t count = 0;
        uint32_t probe = 0;
        std::vector<seq::NoteSpan> notes;
    };
    static std::vector<Entry> cache;
    static std::vector<seq::NoteSpan> empty;
    if (trackIndex < 0) return empty;
    if ((int)cache.size() <= trackIndex) cache.resize((std::size_t)trackIndex + 1);
    auto& e = cache[(std::size_t)trackIndex];
    // 프로브: 트랙 순서가 바뀌어도 (스탬프가 우연히 같아도) 내용으로 구분한다
    const uint32_t probe =
        track.events.empty()
            ? 0u
            : (track.events.front().tick ^ (track.events.back().tick << 1) ^
               ((uint32_t)track.events.size() << 16) ^ ((uint32_t)track.channel << 28));
    if (e.stamp != track.editStamp || e.count != track.events.size() || e.probe != probe) {
        e.notes = seq::extractNotes(track);
        e.stamp = track.editStamp;
        e.count = track.events.size();
        e.probe = probe;
    }
    return e.notes;
}

// 구간 복제 (어레인지): [startTick, endTick)의 모든 내용(노트/CC/클립/오토메이션/
// 템포/마커)을 구간 바로 뒤에 끼워 넣고, 그 뒤의 내용은 구간 길이만큼 민다.
// Verse를 하나 더 만들거나 코러스를 반복시킬 때 쓴다. (경계에 걸쳐 있는 노트는
// "시작 틱" 기준으로 처리한다 — 걸친 꼬리는 그대로 늘어난다)
void duplicateSection(AppState& state, uint32_t startTick, uint32_t endTick) {
    if (endTick <= startTick) return;
    const uint32_t len = endTick - startTick;
    stopTransport(state); // 구조가 크게 바뀌므로 안전하게 정지
    state.snapshot();
    for (auto& t : state.song.tracks) {
        // MIDI 클립 구간을 "먼저" 민다/복사한다 (멤버 키 포함). 노트 이동이
        // removeNote를 거치며 소속 키를 지우는데, 키를 미리 새 위치로 옮겨두면
        // 옛 위치 키와 안 겹쳐 안전하고, 옮겨진 노트가 새 키와 다시 맞물린다.
        {
            std::vector<seq::MidiClip> addMc;
            for (auto& mc : t.midiClips) {
                if (mc.startTick >= endTick) {
                    mc.startTick += len;
                    mc.endTick += len;
                    for (auto& m : mc.members) m.second += len; // 노트도 곧 밀린다
                } else if (mc.startTick >= startTick) {
                    seq::MidiClip c = mc;
                    c.startTick += len;
                    c.endTick += len;
                    for (auto& m : c.members) m.second += len; // 복사본 노트의 키
                    addMc.push_back(c);
                }
            }
            t.midiClips.insert(t.midiClips.end(), addMc.begin(), addMc.end());
            std::stable_sort(t.midiClips.begin(), t.midiClips.end(),
                             [](const seq::MidiClip& a, const seq::MidiClip& b) {
                                 return a.startTick < b.startTick;
                             });
        }
        // MIDI: 노트는 스팬(쌍) 단위로 다뤄 경계에 걸친 노트의 On/Off 짝이
        // 깨지지 않게 한다 (낱개 이벤트로 밀면 걸친 노트가 꼬인다).
        {
            const auto notes = seq::extractNotes(t);
            std::vector<seq::NoteSpan> shiftN, copyN;
            for (const auto& n : notes) {
                if (n.startTick >= endTick) shiftN.push_back(n);
                else if (n.startTick >= startTick) copyN.push_back(n);
            }
            for (const auto& n : shiftN) seq::removeNote(t, n);
            // 노트 외 이벤트 (CC/벤드/프로그램): 뒤 밀기 + 구간 복사
            std::vector<seq::MidiEvent> addEv;
            for (auto& e : t.events) {
                if (e.isNoteOn() || e.isNoteOff()) continue;
                if (e.tick >= endTick) {
                    e.tick += len;
                } else if (e.tick >= startTick) {
                    seq::MidiEvent c = e;
                    c.tick += len;
                    addEv.push_back(c);
                }
            }
            t.events.insert(t.events.end(), addEv.begin(), addEv.end());
            for (const auto& n : shiftN)
                t.addNote(n.startTick + len,
                          n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                          n.velocity);
            for (const auto& n : copyN)
                t.addNote(n.startTick + len,
                          n.endTick > n.startTick ? n.endTick - n.startTick : 1, n.note,
                          n.velocity);
            t.sortEvents();
        }
        // (MIDI 클립 구간/멤버 키는 위에서 노트보다 먼저 옮겼다)
        // 오디오 클립: 시작 틱 기준, 복사본은 깊은 복사 (편집이 서로 안 섞이게)
        std::vector<std::shared_ptr<audio::AudioClip>> addClips;
        for (auto& cp : t.clips) {
            if (!cp) continue;
            if (cp->startTick >= endTick) {
                cp->startTick += len;
            } else if (cp->startTick >= startTick) {
                auto c = std::make_shared<audio::AudioClip>(*cp);
                c->startTick += len;
                addClips.push_back(std::move(c));
            }
        }
        for (auto& c : addClips) t.clips.push_back(std::move(c));
        // 오토메이션 곡선
        const auto shiftAuto = [&](std::vector<seq::Track::AutoPoint>& pts) {
            std::vector<seq::Track::AutoPoint> addP;
            for (auto& p : pts) {
                if (p.tick >= endTick) p.tick += len;
                else if (p.tick >= startTick) addP.push_back({p.tick + len, p.value});
            }
            pts.insert(pts.end(), addP.begin(), addP.end());
            std::sort(pts.begin(), pts.end(),
                      [](const seq::Track::AutoPoint& a, const seq::Track::AutoPoint& b) {
                          return a.tick < b.tick;
                      });
        };
        shiftAuto(t.volAuto);
        shiftAuto(t.panAuto);
    }
    // 템포 지점
    {
        std::vector<seq::TempoChange> addT;
        for (auto& tc : state.song.tempoChanges) {
            if (tc.tick >= endTick) tc.tick += len;
            else if (tc.tick >= startTick) {
                seq::TempoChange c = tc;
                c.tick += len;
                addT.push_back(c);
            }
        }
        state.song.tempoChanges.insert(state.song.tempoChanges.end(), addT.begin(),
                                       addT.end());
        std::stable_sort(state.song.tempoChanges.begin(), state.song.tempoChanges.end(),
                         [](const seq::TempoChange& a, const seq::TempoChange& b) {
                             return a.tick < b.tick;
                         });
    }
    // 구간 마커 (복제된 구간의 마커도 함께)
    {
        std::vector<seq::SectionMarker> addM;
        for (auto& mk : state.song.markers) {
            if (mk.tick >= endTick) mk.tick += len;
            else if (mk.tick >= startTick) addM.push_back({mk.tick + len, mk.name});
        }
        state.song.markers.insert(state.song.markers.end(), addM.begin(), addM.end());
        std::stable_sort(state.song.markers.begin(), state.song.markers.end(),
                         [](const seq::SectionMarker& a, const seq::SectionMarker& b) {
                             return a.tick < b.tick;
                         });
    }
    // 루프가 구간 뒤에 있었으면 함께 민다
    if (state.loopStartTick >= endTick) {
        state.loopStartTick += len;
        state.loopEndTick += len;
    }
    // 인덱스 기반 선택은 전부 무효 — 비운다
    state.selectedNotes.clear();
    state.selClips.clear();
    state.selClipTrack = state.selClipIndex = -1;
    state.clipRange = AppState::ClipRangeSel{};
    rebuildAudioMix(state);
    state.statusMessage =
        "구간 복제: " + std::to_string(len / songTicksPerBar(state)) + "마디 분량이 뒤에 추가됨";
}

// 스윙(그루브): 격자로 정렬한 뒤 "짝수 번째 칸(엇박)"의 노트를 뒤로 민다.
// pct 0=정직, 100=셋잇단 느낌 (밀리는 양 = 격자의 1/3). 선택이 있으면 선택만.
void applySwing(AppState& state, seq::Track& track, uint32_t grid, int pct) {
    if (grid == 0 || pct <= 0) return;
    const auto sel = gatherSelected(state, track);
    const bool useSel = !sel.empty();
    const auto targets = useSel ? sel : seq::extractNotes(track);
    if (targets.empty()) return;
    state.snapshot();
    const uint32_t delay = (uint32_t)((int64_t)grid * pct / 300); // 100% = grid/3
    std::set<std::pair<uint8_t, uint32_t>> newSel;
    int changed = 0;
    for (const auto& n : targets) {
        const uint32_t q = (n.startTick + grid / 2) / grid * grid; // 격자 정렬
        const uint32_t target = q + (((q / grid) % 2 != 0) ? delay : 0);
        if (target != n.startTick) {
            const uint32_t dur = n.endTick > n.startTick ? n.endTick - n.startTick : 1;
            seq::removeNote(track, n);
            track.addNote(target, dur, n.note, n.velocity);
            seq::adoptNoteIntoClips(track, n.note, target);
            ++changed;
        }
        if (useSel) newSel.insert({n.note, target});
    }
    if (changed) track.sortEvents();
    if (useSel) state.selectedNotes = std::move(newSel);
    state.statusMessage = "스윙 " + std::to_string(pct) + "%: " + std::to_string(changed) +
                          "개 이동";
    refreshPlaybackIfPlaying(state);
}

// 선택 노트 무리를 (dTick, dNote)만큼 이동한다.
void moveSelectedNotes(AppState& state, seq::Track& track, int dTick, int dNote) {
    if (dTick == 0 && dNote == 0) return;
    auto sel = gatherSelected(state, track);
    if (sel.empty()) return;
    state.snapshot();
    std::set<std::pair<uint8_t, uint32_t>> newSel;
    for (const auto& s : sel) seq::removeNote(track, s);
    for (const auto& s : sel) {
        long nt = (long)s.startTick + dTick;
        int nn = std::clamp((int)s.note + dNote, 0, 127);
        if (nt < 0) nt = 0;
        track.addNote((uint32_t)nt, s.endTick - s.startTick, (uint8_t)nn, s.velocity);
        seq::adoptNoteIntoClips(track, (uint8_t)nn, (uint32_t)nt);
        newSel.insert({(uint8_t)nn, (uint32_t)nt});
    }
    track.sortEvents();
    state.selectedNotes = std::move(newSel);
    refreshPlaybackIfPlaying(state);
}

// ---------------------------------------------------------
// 내보내기 설정 창: 경로 + 형식(WAV/MP3) + 구간(전체/사용자 지정 마디)
// 실제 렌더/저장은 App이 exportRunRequested를 받아 오프라인으로 수행한다.
// ---------------------------------------------------------
void drawExportDialog(AppState& state) {
    if (!state.showExportDialog) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::Begin("내보내기", &state.showExportDialog, ImGuiWindowFlags_AlwaysAutoResize);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // 저장 위치(폴더)와 파일 이름을 따로 지정한다
    ImGui::TextUnformatted("저장 위치 (폴더)");
    char dbuf[512];
    std::snprintf(dbuf, sizeof(dbuf), "%s", state.exportDir.c_str());
    ImGui::SetNextItemWidth(330);
    if (ImGui::InputText("##exdir", dbuf, sizeof(dbuf))) state.exportDir = dbuf;
    ImGui::SameLine();
    if (ImGui::Button("찾아보기...")) state.exportBrowseRequested = true;

    ImGui::TextUnformatted("파일 이름");
    char nbuf[256];
    std::snprintf(nbuf, sizeof(nbuf), "%s", state.exportFileName.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("##exname", nbuf, sizeof(nbuf))) state.exportFileName = nbuf;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.exportUseMp3 ? ".mp3" : ".wav");

    // 형식
    ImGui::Separator();
    ImGui::TextUnformatted("형식");
    if (ImGui::RadioButton("WAV (무손실)", !state.exportUseMp3)) state.exportUseMp3 = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("MP3 (192kbps)", state.exportUseMp3)) state.exportUseMp3 = true;

    // 대상: 전체 믹스 / 트랙별 스템 / 선택한 트랙 하나만
    ImGui::Separator();
    ImGui::TextUnformatted("대상");
    if (ImGui::RadioButton("전체 믹스", state.exportMode == 0)) state.exportMode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("트랙별 스템", state.exportMode == 1)) state.exportMode = 1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("각 트랙을 \"파일이름_트랙이름\"으로 하나씩 저장합니다.\n"
                          "트랙 볼륨/팬/FX는 반영되고 마스터 이펙트는 제외됩니다.\n"
                          "Send를 쓰는 트랙이 있으면 \"_리턴리버브\" 스템도 함께 나옵니다.");
    if (state.exportMode == 1) {
        ImGui::SameLine();
        ImGui::Checkbox("리미터 적용##stemlim", &state.exportStemLimiter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("각 스템(리턴 리버브 포함)에 마스터 리미터와\n"
                              "같은 세팅을 적용합니다 (스템 클리핑 방지)");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("선택 트랙만", state.exportMode == 2)) state.exportMode = 2;
    if (state.exportMode == 2) {
        const char* tn = state.selectedTrack < (int)state.song.tracks.size()
                             ? state.song.tracks[(std::size_t)state.selectedTrack].name.c_str()
                             : "(없음)";
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", tn);
    }

    // 구간
    ImGui::Separator();
    ImGui::Checkbox("사용자 설정 (구간 지정)", &state.exportCustomRange);
    const uint32_t tpb = songTicksPerBar(state);
    const uint32_t contentEnd = songEndTicks(state);
    const int contentBars = tpb > 0 ? (int)((contentEnd + tpb - 1) / tpb) : 0;
    if (state.exportCustomRange) {
        // 처음 열릴 때 구간을 내용 길이로 초기화
        if (state.exportEndTick == 0) {
            state.exportStartTick = 0;
            state.exportEndTick = std::max(contentEnd, tpb * 4);
        }
        const uint32_t minLen = tpb / 8; // 최소 구간 길이
        // ── 미니 타임라인: 트랙 미리보기 + 드래그 구간 지정 ──
        //  · 가운데 드래그: 마디 스냅으로 새 구간 선택
        //  · 좌/우 가장자리 핸들 드래그: 시간 단위로 미세 조절 (스냅 없음)
        const uint32_t totalTicks =
            std::max({contentEnd, state.exportEndTick, tpb * 4}) + tpb;
        const int totalBars = (int)((totalTicks + tpb - 1) / tpb);
        // 연습 트랙은 곡의 일부가 아니다 — 내보내기 미리보기에서 제외
        std::vector<int> exIdx;
        for (int i = 0; i < (int)state.song.tracks.size(); ++i)
            if (!state.song.tracks[(std::size_t)i].practice) exIdx.push_back(i);
        const int nTracks = std::max(1, (int)exIdx.size());
        const float w = 430.0f;
        const float rowH = std::clamp(44.0f / (float)nTracks, 5.0f, 12.0f);
        const float h = std::max(30.0f, rowH * (float)nTracks + 6.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("exrange", ImVec2(w, h));

        const auto tickToX = [&](double t) { return p.x + w * (float)(t / (double)totalTicks); };
        const auto xToTick = [&](float x) {
            return (uint32_t)std::clamp((double)(x - p.x) / w * (double)totalTicks, 0.0,
                                        (double)totalTicks);
        };
        float sx = tickToX(state.exportStartTick);
        float ex = tickToX(state.exportEndTick);

        // 상호작용: 가장자리 핸들 vs 가운데(마디 스냅 선택)
        static int dragMode = 0; // 0=없음 1=왼쪽핸들 2=오른쪽핸들 3=새 선택
        static uint32_t dragAnchorTick = 0;
        const float mxp = ImGui::GetIO().MousePos.x;
        const bool nearL = std::fabs(mxp - sx) <= 6.0f;
        const bool nearR = std::fabs(mxp - ex) <= 6.0f;
        if (ImGui::IsItemHovered() && (nearL || nearR))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
            if (nearL) dragMode = 1;
            else if (nearR) dragMode = 2;
            else {
                dragMode = 3;
                dragAnchorTick = (xToTick(mxp) / tpb) * tpb; // 마디 스냅
            }
        }
        if (ImGui::IsItemActive()) {
            const uint32_t t = xToTick(mxp);
            if (dragMode == 1) {
                state.exportStartTick = std::min(t, state.exportEndTick - minLen);
            } else if (dragMode == 2) {
                state.exportEndTick = std::max(t, state.exportStartTick + minLen);
            } else if (dragMode == 3) {
                const uint32_t snapped = (t / tpb) * tpb;
                state.exportStartTick = std::min(dragAnchorTick, snapped);
                state.exportEndTick = std::max(dragAnchorTick, snapped) + tpb;
            }
        } else {
            dragMode = 0;
        }

        // ── 그리기 ──
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(28, 28, 34, 255), 3.0f);
        // 마디선 (좁으면 4마디마다)
        const int stepBar = totalBars > 32 ? 4 : 1;
        for (int b = 0; b <= totalBars; b += stepBar) {
            const float x = tickToX((double)b * tpb);
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), IM_COL32(70, 70, 82, 130));
        }
        // 트랙 미리보기: 트랙마다 색을 다르게, 노트/클립을 선으로 표시
        static const ImU32 kTrackCols[8] = {
            IM_COL32(90, 170, 250, 255),  IM_COL32(250, 170, 90, 255),
            IM_COL32(120, 220, 140, 255), IM_COL32(230, 120, 200, 255),
            IM_COL32(250, 220, 100, 255), IM_COL32(140, 130, 250, 255),
            IM_COL32(100, 220, 220, 255), IM_COL32(250, 120, 120, 255)};
        for (int row = 0; row < (int)exIdx.size(); ++row) {
            const int ti = exIdx[(std::size_t)row];
            const auto& tr = state.song.tracks[(std::size_t)ti];
            const ImU32 col = kTrackCols[ti % 8];
            const float ry = p.y + 3.0f + rowH * (float)row;
            const float rcy = ry + rowH * 0.5f;
            // 오디오 클립: 반투명 채움 블록
            for (const auto& cp : tr.clips) {
                if (!cp) continue;
                const double endTk = clipEndTick(*cp, state.song);
                dl->AddRectFilled(ImVec2(tickToX(cp->startTick), ry + 1.0f),
                                  ImVec2(tickToX((uint32_t)endTk), ry + rowH - 1.0f),
                                  (col & 0x00FFFFFF) | 0x60000000);
            }
            // MIDI 노트: 가는 선
            for (const auto& n : seq::extractNotes(tr)) {
                const float x0 = tickToX(n.startTick);
                const float x1 = std::max(tickToX(n.endTick), x0 + 1.5f);
                dl->AddLine(ImVec2(x0, rcy), ImVec2(x1, rcy), col, 2.0f);
            }
        }
        // 선택 구간 강조 + 좌우 핸들
        sx = tickToX(state.exportStartTick);
        ex = tickToX(state.exportEndTick);
        dl->AddRectFilled(ImVec2(sx, p.y), ImVec2(ex, p.y + h), IM_COL32(120, 190, 255, 55));
        dl->AddRect(ImVec2(sx, p.y), ImVec2(ex, p.y + h), IM_COL32(140, 200, 255, 230), 0, 0,
                    2.0f);
        dl->AddRectFilled(ImVec2(sx - 2.5f, p.y), ImVec2(sx + 2.5f, p.y + h),
                          IM_COL32(160, 210, 255, 255));
        dl->AddRectFilled(ImVec2(ex - 2.5f, p.y), ImVec2(ex + 2.5f, p.y + h),
                          IM_COL32(160, 210, 255, 255));
        if (ImGui::IsItemHovered() && !nearL && !nearR)
            ImGui::SetTooltip("가운데 드래그: 마디 단위 선택 · 가장자리 드래그: 시간 미세 조절");

        // 마디 입력 (마디 경계로 스냅) + 초 입력 (시간으로 직접)
        int sb = (int)(state.exportStartTick / tpb) + 1;
        int eb = (int)((state.exportEndTick + tpb - 1) / tpb);
        ImGui::SetNextItemWidth(96);
        if (ImGui::InputInt("시작 마디", &sb))
            state.exportStartTick = (uint32_t)std::max(0, sb - 1) * tpb;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96);
        if (ImGui::InputInt("끝 마디", &eb))
            state.exportEndTick =
                std::max((uint32_t)std::max(1, eb) * tpb, state.exportStartTick + minLen);
        ImGui::SameLine();
        if (ImGui::SmallButton("루프 구간 가져오기")) {
            state.exportStartTick = state.loopStartTick;
            state.exportEndTick = state.loopEndTick;
        }
        double s0 = seq::songTickToSec(state.song, state.exportStartTick);
        double s1 = seq::songTickToSec(state.song, state.exportEndTick);
        ImGui::SetNextItemWidth(96);
        if (ImGui::InputDouble("시작 (초)", &s0, 0, 0, "%.2f"))
            state.exportStartTick =
                (uint32_t)std::max(0.0, seq::songSecToTick(state.song, s0));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96);
        if (ImGui::InputDouble("끝 (초)", &s1, 0, 0, "%.2f"))
            state.exportEndTick =
                std::max((uint32_t)seq::songSecToTick(state.song, std::max(s1, 0.0)),
                         state.exportStartTick + minLen);
        if (state.exportEndTick <= state.exportStartTick)
            state.exportEndTick = state.exportStartTick + minLen;
        ImGui::Text("구간: %d:%04.1f ~ %d:%04.1f  (길이 %.1f초)", (int)(s0 / 60.0),
                    s0 - (int)(s0 / 60.0) * 60, (int)(s1 / 60.0), s1 - (int)(s1 / 60.0) * 60,
                    s1 - s0);
    } else {
        const double lenSec = seq::songTickToSec(state.song, contentEnd);
        ImGui::TextDisabled("전체: 처음부터 내용 끝(%d마디, %.1f초)까지 + 여운 2초", contentBars,
                            lenSec);
    }

    // 실행
    ImGui::Separator();
    if (ImGui::Button("내보내기", ImVec2(120, 0))) {
        if (state.exportDir.empty())
            state.statusMessage = "저장 위치(폴더)를 먼저 지정하세요";
        else if (state.exportFileName.empty())
            state.statusMessage = "파일 이름을 입력하세요";
        else
            state.exportRunRequested = true; // App이 오프라인 렌더 + 저장
    }
    ImGui::SameLine();
    if (ImGui::Button("닫기", ImVec2(120, 0))) state.showExportDialog = false;
    ImGui::TextDisabled("재생 없이 즉시 렌더링됩니다 (곡 길이보다 훨씬 빠름)");
    ImGui::End();
}

// ---------------------------------------------------------
// 뮤지컬 타이핑: 컴퓨터 키보드로 연주 (Z줄 = 흰건반, S줄 = 검은건반)
// 녹음 중이면 MIDI 건반처럼 노트가 기록된다. [ ] = 옥타브 이동.
// ---------------------------------------------------------
static void updateMusicalTyping(AppState& state) {
    struct KM {
        ImGuiKey key;
        int semi;
    };
    static const KM kMap[] = {
        {ImGuiKey_Z, 0},  {ImGuiKey_S, 1},  {ImGuiKey_X, 2},  {ImGuiKey_D, 3},
        {ImGuiKey_C, 4},  {ImGuiKey_V, 5},  {ImGuiKey_G, 6},  {ImGuiKey_B, 7},
        {ImGuiKey_H, 8},  {ImGuiKey_N, 9},  {ImGuiKey_J, 10}, {ImGuiKey_M, 11},
        {ImGuiKey_Comma, 12}, // 한 옥타브 위 도
    };
    static int heldNote[IM_ARRAYSIZE(kMap)];
    static uint8_t heldCh[IM_ARRAYSIZE(kMap)];
    static bool init = false;
    if (!init) {
        for (auto& h : heldNote) h = -1;
        init = true;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool allow = state.musicalTyping && !io.WantTextInput && !io.KeyCtrl;
    if (allow) { // 옥타브 이동
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false))
            state.mtOctave = std::max(0, state.mtOctave - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))
            state.mtOctave = std::min(8, state.mtOctave + 1);
    }
    const uint8_t ch = state.selectedTrack < (int)state.song.tracks.size()
                           ? state.song.tracks[(std::size_t)state.selectedTrack].channel
                           : (uint8_t)0;

    for (int k = 0; k < IM_ARRAYSIZE(kMap); ++k) {
        const int note = state.mtOctave * 12 + 12 + kMap[k].semi; // 옥타브4의 Z = C4(60)
        // 누름: Note On + (녹음 중이면) 기록 시작
        if (allow && heldNote[k] < 0 && note >= 0 && note <= 127 &&
            ImGui::IsKeyPressed(kMap[k].key, false)) {
            heldNote[k] = note;
            heldCh[k] = ch;
            if (state.output) {
                if (!state.output->isOpen())
                    state.output->openPort((unsigned)state.selectedOutputPort);
                state.output->send(midi::MidiMessage::makeNoteOn(ch, (uint8_t)note, 100));
            }
            if (state.recording) {
                auto& open = state.openRecNotes[note];
                if (!open.active) {
                    open.active = true;
                    open.startTick = playheadTick(state);
                    open.velocity = 100;
                }
            }
        }
        // 뗌 (또는 기능이 꺼짐): Note Off + 기록 확정
        if (heldNote[k] >= 0 &&
            (ImGui::IsKeyReleased(kMap[k].key) || !state.musicalTyping)) {
            const int n = heldNote[k];
            const uint8_t offCh = heldCh[k];
            heldNote[k] = -1;
            if (state.output && state.output->isOpen())
                state.output->send(
                    {(uint8_t)(midi::kStatusNoteOff | (offCh & 0x0F)), (uint8_t)n, 0});
            if (state.recording && state.selectedTrack < (int)state.song.tracks.size()) {
                auto& open = state.openRecNotes[n];
                if (open.active) {
                    const uint32_t nowT = playheadTick(state);
                    const uint32_t dur = nowT > open.startTick ? nowT - open.startTick : 1;
                    auto& trk = state.song.tracks[(std::size_t)state.selectedTrack];
                    trk.addNote(open.startTick, dur, (uint8_t)n, open.velocity);
                    seq::adoptNoteIntoClips(trk, (uint8_t)n, open.startTick);
                    trk.sortEvents();
                    open.active = false;
                }
            }
        }
    }
}

// ---------------------------------------------------------
// 성능 창: FPS / 오디오 콜백 부하 / 버퍼 / 보이스 (Tool 메뉴에서 켜고 끔)
// ---------------------------------------------------------
void drawPerf(AppState& state) {
    if (!state.showPerf) return;
    ImGui::SetNextWindowSize(ImVec2(280, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("성능", &state.showPerf);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("화면: %.0f FPS (%.1f ms)", io.Framerate,
                io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    if (state.sysCpuPercent >= 0.0f)
        ImGui::Text("CPU 사용률: %.0f%% (시스템 전체)", state.sysCpuPercent);
    else
        ImGui::TextDisabled("CPU 사용률: 측정 중...");
    ImGui::Separator();
    if (state.audioClips) {
        const double sr = state.audioClips->engineSampleRate();
        const unsigned bf = state.audioClips->engineBufferFrames();
        ImGui::Text("오디오: %.0f Hz · 버퍼 %u 프레임 (%.1f ms)", sr, bf,
                    sr > 0.0 ? (double)bf * 1000.0 / sr : 0.0);
        const float load = std::clamp(state.audioClips->audioLoad(), 0.0f, 1.5f);
        ImGui::TextUnformatted("오디오 콜백 부하 (100% = 버퍼 시간 전부 사용)");
        // 여유=초록, 빠듯=노랑, 위험=빨강
        const ImVec4 lc = load < 0.5f   ? ImVec4(0.35f, 0.78f, 0.42f, 1.0f)
                          : load < 0.8f ? ImVec4(0.90f, 0.78f, 0.30f, 1.0f)
                                        : ImVec4(0.92f, 0.34f, 0.30f, 1.0f);
        char lbuf[24];
        std::snprintf(lbuf, sizeof(lbuf), "%.0f%%", load * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, lc);
        ImGui::ProgressBar(std::min(load, 1.0f), ImVec2(-1, 0), lbuf);
        ImGui::PopStyleColor();
        if (load > 0.85f)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f),
                               "끊김 위험 — 버퍼를 키우거나 무거운 트랙을 프리즈하세요");
    } else {
        ImGui::TextDisabled("오디오 엔진 없음");
    }
    if (state.synth) {
        ImGui::Separator();
        ImGui::Text("내장 신스 보이스: %d", state.synth->activeVoiceCount());
    }
    ImGui::End();
}

// ---------------------------------------------------------
// 도움말: 단축키 목록 (F1 또는 메뉴 > 도움말 > 단축키)
// ---------------------------------------------------------
static void drawHelpWindow(AppState& state) {
    if (!state.showHelp) return;
    ImGui::SetNextWindowSize(ImVec2(470, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("단축키", &state.showHelp)) {
        ImGui::End();
        return;
    }

    struct Row {
        const char* key;
        const char* what;
    };
    auto section = [](const char* title, const Row* rows, int n) {
        ImGui::SeparatorText(title);
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("d", ImGuiTableColumnFlags_WidthStretch);
            for (int i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", rows[i].key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(rows[i].what);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    };

    static const Row kFile[] = {
        {"Ctrl+S", "프로젝트 저장 (.midipro)"},
        {"Ctrl+O", "프로젝트 열기"},
        {"Ctrl+E", "내보내기 창 (MP3/WAV)"},
        {"Ctrl+Z", "실행취소"},
        {"Ctrl+Y / Ctrl+Shift+Z", "다시실행"},
        {"F1", "이 단축키 창 열기/닫기"},
    };
    static const Row kTransport[] = {
        {"Space", "재생 / 정지 (녹음 중이면 녹음 정지)"},
        {"Ctrl+Space", "처음으로 (정지 + 화면도 맨 앞으로)"},
        {"Shift+Space", "현재 마디 처음부터 재생"},
        {"휠 / Shift+휠 / Ctrl+휠", "확대 / 세로 스크롤 / 가로 스크롤 (트랙 뷰·피아노 롤·드럼)"},
        {"R", "녹음 시작 / 정지 (선택 트랙)"},
        {"건반 연주 켜면", "Z~M = 도~시 · S D G H J = 검은건반 · [ ] 옥타브"},
        {"Home", "처음으로 (틱 0)"},
        {"← → / ↑ ↓", "트랙 뷰·피아노 롤 가로/세로 스크롤"},
        {"M", "메트로놈 켜기/끄기"},
        {"L", "루프 켜기/끄기"},
    };
    static const Row kPianoRoll[] = {
        {"1 / 2 / 3 / 4", "새 노트 길이: 4 / 8 / 16 / 32분음표"},
        {"클릭 (빈 칸)", "노트 추가 · 드래그하면 경로에 계속 그리기 (페인트)"},
        {"노트 끝 드래그", "노트 길이 조절 (↔ 커서)"},
        {"Shift+클릭 (빈 칸)", "직전 노트와 같은 길이·세기로 추가"},
        {"Shift+드래그", "범위 선택 (여러 노트)"},
        {"Ctrl+C / Ctrl+V", "선택 노트 복사 / 붙여넣기"},
        {"Ctrl+클릭 (노트)", "노트 길이 반으로 자르기"},
        {"Q", "퀀타이즈 (선택 노트, 없으면 트랙 전체)"},
        {"Del", "선택 노트 삭제"},
    };
    static const Row kTrackView[] = {
        {"클릭 (빈 곳)", "플레이헤드 이동 + 트랙 선택 (클립 선택 해제)"},
        {"클립 클릭", "클립 선택 (흰 테두리)"},
        {"Shift+클립 클릭", "다중 선택 토글 · Ctrl+M 또는 우클릭으로 병합"},
        {"Ctrl+클립 드래그", "구간 선택 — Ctrl+C 복사 / Del 삭제(공백)"},
        {"Shift+Del", "선택 구간 잘라내고 뒷부분을 당겨 붙이기"},
        {"Ctrl+M", "Shift로 선택한 클립들 병합"},
        {"클립 드래그", "이동 · 위/아래 다른 레인에 놓으면 그 트랙으로 이동"},
        {"클립 끝 드래그", "왼쪽/오른쪽 트림"},
        {"Shift+끝 드래그", "배속 조절 (음정 변함)"},
        {"Ctrl+끝 드래그", "음정 유지 길이 조절 (놓는 순간 처리)"},
        {"Ctrl+C / Ctrl+V", "선택 클립 복사 / 재생 위치에 붙여넣기"},
        {"Ctrl+D", "선택 노트/클립을 바로 뒤에 복제"},
        {"우클릭", "메뉴: 자르기·복사·삭제·페이드·게인·붙여넣기·템포"},
        {"Del", "선택된 것 삭제 (템포 마커 → 노트 → 선택 클립 → 트랙 순)"},
    };
    static const Row kTempo[] = {
        {"마커 드래그", "템포 지점 위치 이동 (스냅 없음)"},
        {"마커 우클릭", "BPM 수정 / 삭제"},
        {"마커 클릭 + Del", "템포 지점 삭제"},
    };

    section("파일 · 편집", kFile, IM_ARRAYSIZE(kFile));
    section("재생 · 녹음", kTransport, IM_ARRAYSIZE(kTransport));
    section("피아노 롤", kPianoRoll, IM_ARRAYSIZE(kPianoRoll));
    section("트랙 뷰", kTrackView, IM_ARRAYSIZE(kTrackView));
    section("템포 마커", kTempo, IM_ARRAYSIZE(kTempo));
    ImGui::End();
}

// ---------------------------------------------------------
// 메뉴 바
// ---------------------------------------------------------
void drawMenuBar(AppState& state, bool& openRequested, bool& saveRequested) {
    // 키보드 단축키 (텍스트 입력 중이 아닐 때만)
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            io.KeyShift ? doRedo(state) : doUndo(state);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            doRedo(state);
        } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            state.projectSaveRequested = true; // Ctrl+S: 프로젝트 저장
        } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            state.projectLoadRequested = true; // Ctrl+O: 프로젝트 열기
        } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            state.showExportDialog = true; // Ctrl+E: 내보내기
        }
    }
    // 단독 키: Home(처음으로) / M(메트로놈) / L(루프) / Q(퀀타이즈) / F1(도움말)
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() && !io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) seekTo(state, 0);
        // M = 메트로놈 (뮤지컬 타이핑 중에는 M이 '시' 건반이라 쉬게 한다)
        if (!state.musicalTyping && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
            state.metronome = !state.metronome;
            state.statusMessage = state.metronome ? "메트로놈 켜짐" : "메트로놈 꺼짐";
        }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
            state.loopEnabled = !state.loopEnabled;
            if (state.loopEnabled && state.clipRange.track >= 0 &&
                state.clipRange.t1 > state.clipRange.t0) {
                // 선택 구간이 있으면 그 범위를 루프로 (트랜스포트 체크박스와 동일)
                state.loopStartTick = state.clipRange.t0;
                state.loopEndTick = state.clipRange.t1;
                state.statusMessage = "루프 = 선택 구간";
            } else {
                state.statusMessage = state.loopEnabled ? "루프 켜짐" : "루프 꺼짐";
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false) &&
            state.selectedTrack < (int)state.song.tracks.size()) {
            const uint32_t grid =
                std::max<uint32_t>(1, (uint32_t)state.song.ppqn / (uint32_t)state.quantGridDiv);
            quantizeNotes(state, state.song.tracks[state.selectedTrack], grid);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) state.showHelp = !state.showHelp;
    updateMusicalTyping(state); // 컴퓨터 키보드 연주 (켜져 있을 때만 반응)
    // 방향키: 트랙 뷰·피아노 롤 스크롤 (←→ 가로, ↑↓ 세로. 누르는 동안 부드럽게)
    state.keyScrollX = 0.0f;
    state.keyScrollY = 0.0f;
    if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
        const float sp = 900.0f * io.DeltaTime; // 초당 약 900px
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) state.keyScrollX -= sp;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) state.keyScrollX += sp;
        const float spv = 600.0f * io.DeltaTime; // 세로는 조금 느리게
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) state.keyScrollY -= spv;
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) state.keyScrollY += spv;
    }
    // Ctrl+D: 선택 복제 — 노트 > MIDI 클립 > 오디오 클립 순으로 대상이 정해진다
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        const bool mcValid =
            state.selMidiClipTrack >= 0 &&
            state.selMidiClipTrack < (int)state.song.tracks.size() &&
            state.selMidiClipIndex >= 0 &&
            state.selMidiClipIndex <
                (int)state.song.tracks[(std::size_t)state.selMidiClipTrack].midiClips.size();
        if (!state.selectedNotes.empty()) {
            duplicateSelectedNotes(state);
        } else if (mcValid) {
            // 선택된 MIDI 클립을 바로 뒤에 복제 (우클릭 메뉴와 동일, 멤버만)
            auto& t = state.song.tracks[(std::size_t)state.selMidiClipTrack];
            const seq::MidiClip mc = t.midiClips[(std::size_t)state.selMidiClipIndex];
            state.snapshot();
            const uint32_t len = mc.endTick - mc.startTick;
            const seq::MidiClip nc = seq::copyMidiClip(t, mc, len);
            t.midiClips.push_back(nc);
            std::stable_sort(t.midiClips.begin(), t.midiClips.end(),
                             [](const seq::MidiClip& a, const seq::MidiClip& b) {
                                 return a.startTick < b.startTick;
                             });
            refreshPlaybackIfPlaying(state);
            state.statusMessage = "MIDI 클립 복제";
        } else {
            duplicateSelectedClips(state);
        }
    }
    // Ctrl+C/V/M: 트랙 뷰의 오디오 클립 (피아노 롤 노트가 선택돼 있으면 노트 쪽 우선)
    if (io.KeyCtrl && !io.WantTextInput && state.selectedNotes.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            // 구간 선택이 있으면 그 부분만, 없으면 선택 클립 전체를 복사
            if (rangeSelClip(state)) copyClipRange(state);
            else copySelectedClip(state);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false) && state.clipClipboard)
            pasteClipAt(state, state.selectedTrack, state.playPosTick);
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
            // Ctrl+M: Shift로 다중 선택한 클립 병합 (기준 트랙의 선택분)
            std::vector<int> selHere;
            for (const auto& sc : state.selClips)
                if (sc.first == state.selClipTrack) selHere.push_back(sc.second);
            if ((int)selHere.size() >= 2)
                mergeTrackClips(state, state.selClipTrack, std::move(selHere));
            else
                state.statusMessage = "병합하려면 Shift+클릭으로 클립을 2개 이상 선택하세요";
        }
    }
    // 스페이스바: 녹음 중이면 녹음+재생 정지, 아니면 재생/정지 토글.
    // Ctrl+Space = 처음으로 (정지 + 플레이헤드/화면을 맨 앞으로).
    // 텍스트 입력이나 위젯 조작 중에는 무시 (버튼 활성화용 스페이스와 충돌 방지).
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (io.KeyCtrl) {
            stopTransport(state);
            seekTo(state, 0);
            state.statusMessage = "처음으로";
        } else if (io.KeyShift) {
            // Shift+Space: 지금 있는 마디의 처음부터 재생 (구간 반복 연습에 편함)
            const uint32_t tpbS = songTicksPerBar(state);
            const uint32_t barStart = state.playPosTick / tpbS * tpbS;
            stopTransport(state);
            seekTo(state, barStart);
            startPlayback(state);
            state.statusMessage = "마디 처음부터 재생";
        } else if (state.audioRecTrack >= 0) {
            stopAudioRecording(state);
        } else {
            togglePlayback(state);
        }
    }
    // R: 녹음 토글. 시작하면 자동으로 재생도 시작하고, 다시 누르면 둘 다 정지.
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() && !io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        if (state.audioRecTrack >= 0) stopAudioRecording(state);
        else startAudioRecording(state, state.selectedTrack);
    }
    // 1/2/3/4 (단독): 새 노트 길이를 4/8/16/32분음표로 바꾼다.
    if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
        int div = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) div = 1;       // 4분음표
        else if (ImGui::IsKeyPressed(ImGuiKey_2, false)) div = 2;  // 8분음표
        else if (ImGui::IsKeyPressed(ImGuiKey_3, false)) div = 4;  // 16분음표
        else if (ImGui::IsKeyPressed(ImGuiKey_4, false)) div = 8;  // 32분음표
        if (div > 0) {
            state.editNoteLenDiv = div;
            const char* names[] = {"4분음표", "8분음표", "16분음표", "32분음표"};
            const int idx = (div == 1) ? 0 : (div == 2) ? 1 : (div == 4) ? 2 : 3;
            state.statusMessage = std::string("노트 길이: ") + names[idx];
        }
    }
    // Delete: 파괴적이지 않은 것부터. 선택 템포 마커 -> 선택 노트 -> 트랙의
    // 오디오 클립 -> 트랙 자체. (오디오가 붙은 트랙은 클립을 먼저 지워야
    // 트랙이 지워지므로 실수로 트랙을 날리지 않는다)
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        const bool validTrack =
            state.selectedTrack >= 0 && state.selectedTrack < (int)state.song.tracks.size();
        if (rangeSelClip(state)) {
            // 클립 구간 선택이 있으면 그 부분부터. Shift+Del = 당겨 붙이기
            deleteClipRange(state, /*closeGap=*/io.KeyShift);
        } else if (state.selectedMarker >= 0 &&
                   state.selectedMarker < (int)state.song.markers.size()) {
            state.snapshot();
            state.song.markers.erase(state.song.markers.begin() + state.selectedMarker);
            state.selectedMarker = -1;
            state.statusMessage = "구간 마커 삭제";
        } else if (state.selectedTempoMarker >= 0 &&
            state.selectedTempoMarker < (int)state.song.tempoChanges.size()) {
            state.snapshot();
            state.song.tempoChanges.erase(state.song.tempoChanges.begin() +
                                          state.selectedTempoMarker);
            state.selectedTempoMarker = -1;
            state.statusMessage = "템포 지점 삭제";
        } else if (state.selMidiClipTrack >= 0 &&
                   state.selMidiClipTrack < (int)state.song.tracks.size() &&
                   state.selMidiClipIndex >= 0 &&
                   state.selMidiClipIndex <
                       (int)state.song.tracks[(std::size_t)state.selMidiClipTrack]
                           .midiClips.size()) {
            // 선택된 MIDI 클립: 노트와 함께 삭제 (멤버만 — 겹친 남의 노트는 남는다)
            auto& t = state.song.tracks[(std::size_t)state.selMidiClipTrack];
            const auto rm = t.midiClips[(std::size_t)state.selMidiClipIndex];
            state.snapshot();
            seq::eraseMidiClip(t, rm);
            t.midiClips.erase(t.midiClips.begin() + state.selMidiClipIndex);
            state.selMidiClipTrack = state.selMidiClipIndex = -1;
            state.selectedNotes.clear();
            refreshPlaybackIfPlaying(state);
            state.statusMessage = "MIDI 클립 삭제 (노트 포함)";
        } else if (!state.selectedNotes.empty() && validTrack)
            deleteSelectedNotes(state, state.song.tracks[state.selectedTrack]);
        else if (state.selClipTrack >= 0 &&
                 state.selClipTrack < (int)state.song.tracks.size() &&
                 state.selClipIndex >= 0 &&
                 state.selClipIndex <
                     (int)state.song.tracks[(std::size_t)state.selClipTrack].clips.size()) {
            deleteTrackClip(state, state.selClipTrack, state.selClipIndex); // 선택된 클립
            state.selClipTrack = state.selClipIndex = -1;
        } else if (validTrack && !state.song.tracks[state.selectedTrack].clips.empty())
            deleteTrackClip(state, state.selectedTrack, -1); // 가장 최근 클립부터
        else
            // 지울 게 아무것도 없으면 아무 일도 하지 않는다. 예전엔 여기서
            // "선택된 트랙 자체"를 지웠는데, 트랙 뷰가 계속 선택된 채라 노트를
            // 지우려고 Delete를 누르다 트랙이 통째로 날아가는 사고가 났다.
            // 트랙 삭제는 트랙 헤더 우클릭 > "트랙 삭제"로만 한다.
            state.statusMessage = "지울 대상이 없습니다 (트랙 삭제는 트랙 우클릭 메뉴)";
    }

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("파일")) {
            if (ImGui::MenuItem("새 곡")) {
                stopTransport(state);
                state.snapshot(); // 되돌릴 수 있게 이전 곡을 남긴다
                state.song = seq::Song{};
                state.selectedTrack = 0;
                state.statusMessage = "새 곡을 만들었습니다";
            }
            if (ImGui::MenuItem("새 곡 (템플릿)...")) state.templateDialogRequested = true;
            if (ImGui::MenuItem("열기 (.mid)")) openRequested = true;
            if (ImGui::MenuItem("저장 (.mid)")) saveRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem("프로젝트 열기 (.midipro)", "Ctrl+O"))
                state.projectLoadRequested = true;
            if (ImGui::MenuItem("프로젝트 저장 (.midipro)", "Ctrl+S"))
                state.projectSaveRequested = true;
            if (ImGui::BeginMenu("최근 프로젝트", !state.recentProjects.empty())) {
                for (const auto& rp : state.recentProjects) {
                    // 표시는 파일 이름만, 전체 경로는 툴팁으로
                    const std::size_t slash = rp.find_last_of("\\/");
                    const std::string fname =
                        slash == std::string::npos ? rp : rp.substr(slash + 1);
                    if (ImGui::MenuItem(fname.c_str())) state.recentOpenPath = rp;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rp.c_str());
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("내보내기...", "Ctrl+E")) state.showExportDialog = true;
            if (ImGui::MenuItem("패키지로 내보내기... (샘플 동봉)"))
                state.packageExportRequested = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("프로젝트 + 사용 중인 드럼 샘플을 한 폴더에 모읍니다.\n"
                                  "다른 PC로 옮겨도 소리가 그대로 납니다.\n"
                                  "(오디오 클립/플러그인 상태는 원래부터 함께 저장됩니다)");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("편집")) {
            if (ImGui::MenuItem("실행취소", "Ctrl+Z", false, state.history.canUndo()))
                doUndo(state);
            if (ImGui::MenuItem("다시실행", "Ctrl+Y", false, state.history.canRedo()))
                doRedo(state);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("설정")) {
            // 개인설정: MIDI 장치 / 신디사이저를 탭으로 분류해 한 창에 모음
            ImGui::MenuItem("개인설정", nullptr, &state.showPreferences);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tool")) {
            // 각 창을 체크박스로 켜고 끈다 (별도 팝업 창으로 뜬다)
            ImGui::MenuItem("트랜스포트", nullptr, &state.showTransport);
            ImGui::MenuItem("MIDI 장치", nullptr, &state.showDevices);
            ImGui::MenuItem("트랙 뷰", nullptr, &state.showTrackView);
            ImGui::MenuItem("트랙 목록", nullptr, &state.showTracks);
            ImGui::MenuItem("믹서", nullptr, &state.showMixer);
            ImGui::MenuItem("채널 (마스터/선택 트랙)", nullptr, &state.showMixerCompact);
            ImGui::MenuItem("피아노 롤", nullptr, &state.showPianoRoll);
            ImGui::MenuItem("드럼 트랙", nullptr, &state.showDrums);
            ImGui::MenuItem("어레인지", nullptr, &state.showArrange);
            ImGui::MenuItem("기타 연습", nullptr, &state.showTab);
            ImGui::MenuItem("신디사이저", nullptr, &state.showSynth);
            ImGui::MenuItem("VST3 플러그인", nullptr, &state.showVst);
            ImGui::MenuItem("기타 도우미", nullptr, &state.showGuitar);
            ImGui::MenuItem("입력 모니터", nullptr, &state.showMonitor);
            ImGui::MenuItem("상태", nullptr, &state.showStatus);
            ImGui::MenuItem("성능", nullptr, &state.showPerf);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("도움말")) {
            ImGui::MenuItem("단축키", "F1", &state.showHelp);
            ImGui::MenuItem("MidiPro 정보", nullptr, &state.showAbout);
            ImGui::EndMenu();
        }
        // 최근 상태 메시지를 메뉴 바에 항상 표시한다.
        // (별도 '상태' 창은 기본으로 꺼져 있어 피드백이 안 보이는 문제 해결)
        ImGui::Separator();
        ImGui::TextDisabled("%s", state.statusMessage.c_str());
        ImGui::EndMainMenuBar();
    }

    // 새 곡 템플릿: BPM/트랙 수/박자를 정하고 한 번에 시작
    if (state.templateDialogRequested) {
        state.templateDialogRequested = false;
        ImGui::OpenPopup("새 곡 템플릿");
    }
    if (ImGui::BeginPopupModal("새 곡 템플릿", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static int tBpm = 120, tTracks = 4, tSig = 0;
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("BPM", &tBpm);
        tBpm = std::clamp(tBpm, 40, 240);
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("트랙 수", &tTracks);
        tTracks = std::clamp(tTracks, 1, 16);
        ImGui::SetNextItemWidth(110);
        const char* kTSigs[3] = {"4/4", "3/4", "6/8"};
        ImGui::Combo("박자", &tSig, kTSigs, 3);
        ImGui::Separator();
        if (ImGui::Button("만들기", ImVec2(110, 0))) {
            stopTransport(state);
            state.snapshot(); // 이전 곡으로 되돌릴 수 있게
            state.song = seq::Song{};
            state.song.bpm = (double)tBpm;
            for (int k = 0; k < tTracks; ++k) {
                seq::Track t;
                t.name = "Track " + std::to_string(k + 1);
                t.channel = (uint8_t)(k & 0x0F);
                state.song.tracks.push_back(std::move(t));
                addTrackEq(state, state.song.tracks.back()); // 기본 EQ 장착
            }
            state.selectedTrack = 0;
            state.metroSigIndex = tSig;
            state.playPosTick = 0;
            // 드럼 샘플 배정은 프로젝트 소속이라 새 곡에서는 비운다
            if (state.audioClips)
                for (int n = 0; n < 128; ++n)
                    state.audioClips->setDrumSample((uint8_t)n, nullptr);
            state.drumSamplePaths.clear();
            state.statusMessage = "새 곡: " + std::to_string(tTracks) + "트랙, " +
                                  std::to_string(tBpm) + " BPM";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("취소", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // 정보 창
    if (state.showAbout) {
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("MidiPro 정보", &state.showAbout,
                         ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("MidiPro 1.2.1");
            ImGui::TextDisabled("MIDI 시퀀서 + 오디오 녹음 + VST3 호스트");
            ImGui::Text("빌드: %s", __DATE__);
            ImGui::Separator();
            ImGui::TextDisabled("사용한 오픈소스:");
            ImGui::BulletText("Dear ImGui (UI)");
            ImGui::BulletText("RtAudio / RtMidi (오디오·MIDI 입출력)");
            ImGui::BulletText("dr_mp3 / dr_flac (디코더)");
            ImGui::BulletText("VST3 SDK / ASIO SDK (Steinberg)");
            ImGui::Separator();
            ImGui::TextDisabled("설정·자동 저장: %%LOCALAPPDATA%%\\MidiPro");
        }
        ImGui::End();
    }

    drawHelpWindow(state);
}

// ---------------------------------------------------------
// 장치 선택
// ---------------------------------------------------------
// 창/탭 어디서든 재사용할 수 있게 내용만 그린다 (Begin/End은 호출자 책임).
static void drawDevicesBody(AppState& state) {
    // 출력 대상 선택 (하드웨어 MIDI / 내장 신스)
    ImGui::TextUnformatted("출력 대상");
    if (state.output) {
        const auto& targets = state.output->targets();
        for (int i = 0; i < (int)targets.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(targets[i].label.c_str(), state.output->activeTarget() == i)) {
                // 대상 전환 시 재생 중이면 멈춰 노트 꼬임을 막는다
                if (state.player) state.player->stop();
                state.output->setActiveTarget(i);
                state.selectedOutputPort = 0;
            }
        }
    }
    ImGui::Separator();

    // 출력 포트 (선택된 대상의 포트 목록)
    ImGui::TextUnformatted("출력 포트");
    if (state.output) {
        const auto ports = state.output->listPorts();
        std::string preview = ports.empty() ? "(없음)"
                              : (state.selectedOutputPort < (int)ports.size()
                                     ? ports[state.selectedOutputPort]
                                     : "(선택)");
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##outport", preview.c_str())) {
            for (int i = 0; i < (int)ports.size(); ++i) {
                const bool selected = (i == state.selectedOutputPort);
                if (ImGui::Selectable(ports[i].c_str(), selected)) state.selectedOutputPort = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(state.output->isOpen() ? "닫기##out" : "열기##out")) {
            if (state.output->isOpen()) state.output->closePort();
            else state.output->openPort((unsigned)state.selectedOutputPort);
        }
    }

    ImGui::Separator();

    // 입력
    ImGui::TextUnformatted("입력");
    if (state.input) {
        const auto ports = state.input->listPorts();
        std::string preview = ports.empty() ? "(없음)"
                              : (state.selectedInputPort < (int)ports.size()
                                     ? ports[state.selectedInputPort]
                                     : "(선택)");
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##inport", preview.c_str())) {
            for (int i = 0; i < (int)ports.size(); ++i) {
                const bool selected = (i == state.selectedInputPort);
                if (ImGui::Selectable(ports[i].c_str(), selected)) state.selectedInputPort = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(state.input->isOpen() ? "닫기##in" : "열기##in")) {
            if (state.input->isOpen()) state.input->closePort();
            else state.input->openPort((unsigned)state.selectedInputPort);
        }
        if (ports.empty())
            ImGui::TextDisabled("입력 장치 없음 (loopMIDI 등 가상 포트 사용 가능)");
        else
            ImGui::TextDisabled("※ 열어 둔 장치는 기억했다가 다음 실행 때 자동으로 엽니다.");
    }

    ImGui::Separator();
    // 소프트 스루: 자주 안 건드는 설정이라 개인설정에 둔다.
    ImGui::Checkbox("소프트 스루 (MIDI 입력을 출력으로 통과시켜 모니터링)", &state.softThru);
}

void drawDevices(AppState& state) {
    if (!state.showDevices) return;
    ImGui::Begin("MIDI 장치", &state.showDevices);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)
    drawDevicesBody(state);
    ImGui::End();
}



// ---------------------------------------------------------
// 내장 신스 음색
// ---------------------------------------------------------
// (파일 분할로 여러 TU가 공유하게 되어 익명 네임스페이스 해제)

// 슬라이더 + MIDI Learn 버튼을 한 줄에 그린다.
// 버튼 표시: 학습 대기중="대기…", 매핑됨="CC{n}", 없음="Learn".
// 좌클릭=학습 시작/취소, 우클릭=매핑 해제.
bool learnableSlider(AppState& state, const char* label, mapping::ParamTarget target,
                     float& value, const char* fmt) {
    const mapping::ParamInfo& info = mapping::paramInfo(target);
    ImGui::PushID(label);

    ImGui::SetNextItemWidth(150.0f);
    const bool changed = ImGui::SliderFloat(label, &value, info.min, info.max, fmt);

    ImGui::SameLine();
    const bool arming = state.learnArmed && state.learnTarget == target;
    const int cc = state.midiMap.ccForTarget(target);
    char btn[24];
    if (arming) std::snprintf(btn, sizeof(btn), "대기…");
    else if (cc >= 0) std::snprintf(btn, sizeof(btn), "CC%d", cc);
    else std::snprintf(btn, sizeof(btn), "Learn");

    if (arming) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 120, 40, 255));
    if (ImGui::Button(btn, ImVec2(72, 0))) {
        if (arming) {
            state.learnArmed = false; // 다시 누르면 취소
        } else {
            state.learnArmed = true;
            state.learnTarget = target;
        }
    }
    if (arming) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) state.midiMap.clearTarget(target);
    if (ImGui::IsItemHovered() && cc >= 0) ImGui::SetTooltip("우클릭: 매핑 해제");

    ImGui::PopID();
    return changed;
}



// 창/탭 어디서든 재사용할 수 있게 내용만 그린다 (Begin/End은 호출자 책임).
static void drawSynthBody(AppState& state) {
    if (state.synth == nullptr) {
        ImGui::TextDisabled("신스를 사용할 수 없습니다.");
        return;
    }

    ImGui::TextDisabled("출력 대상을 '내장 신디사이저'로 두고 열면 소리가 납니다.");

    // ---- 오디오 출력 장치 (포커스라이트 등 인터페이스 선택) ----
    ImGui::SeparatorText("오디오 출력 장치");
    {
        const auto devices = state.synth->listOutputDevices();
        const int cur = state.synth->outputDevice();
        const char* preview = (cur >= 0 && cur < (int)devices.size()) ? devices[cur].c_str()
                                                                      : "(기본 출력)";
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("##audiodev", preview)) {
            for (int i = 0; i < (int)devices.size(); ++i)
                if (ImGui::Selectable(devices[i].c_str(), i == cur)) {
                    state.synth->setOutputDevice(i);
                    state.statusMessage = "오디오 장치 변경: " + devices[i];
                }
            ImGui::EndCombo();
        }
        ImGui::Text("샘플레이트: %.0f Hz   활성 보이스: %d / %d",
                    state.synth->currentSampleRate(), state.synth->activeVoiceCount(),
                    (int)audio::Synth::kMaxVoices);
        ImGui::TextDisabled("장치를 바꾸면 스트림이 자동으로 재시작됩니다.");

        // MPE: 노트별 피치벤드/압력/음색 (MPE 컨트롤러 + 소프트 스루로 연주)
        bool mpe = state.synth->mpeMode();
        if (ImGui::Checkbox("MPE 모드 (노트별 표현)", &mpe)) state.synth->setMpeMode(mpe);
        ImGui::SameLine();
        ImGui::TextDisabled(mpe ? "벤드 ±48반음, CC74=음색" : "벤드 ±2반음");

        // MIDI 2.0 데모: 같은 채널의 3음을 노트별로 다르게 벤딩한다.
        // (MIDI 1.0/MPE와 달리 채널을 나누지 않고도 노트별 벤딩이 가능)
        if (state.midi2 && ImGui::Button("MIDI 2.0 데모 (노트별 벤딩)")) {
            const uint8_t notes[3] = {60, 64, 67};       // C 메이저 코드
            const float bend[3] = {0.3f, 0.0f, -0.3f};   // 노트별 피치벤드(정규화)
            for (int i = 0; i < 3; ++i) {
                // 16비트 벨로시티로 노트 온 (MIDI 2.0 고해상도)
                auto on = midi2::makeNoteOn(0, 0, notes[i], midi2::vel7to16(100));
                state.midi2->sendUmp(on.words, on.count);
                // 노트별 32비트 피치벤드
                auto pb = midi2::makePerNotePitchBend(0, 0, notes[i],
                                                      midi2::normToPitch32(bend[i]));
                state.midi2->sendUmp(pb.words, pb.count);
                state.pendingUmpOffs.push_back({0, notes[i], ImGui::GetTime() + 1.6});
            }
            state.statusMessage = "MIDI 2.0: 한 채널에서 3음을 노트별로 벤딩";
        }
        if (state.midi2)
            ImGui::TextDisabled("데모는 출력 대상을 '내장 신디사이저'로 열어야 들립니다.");
    }

    audio::SynthParams& p = state.synthParams;
    bool changed = false;

    // ---- 프리셋 ----
    ImGui::SeparatorText("프리셋");
    const auto& presets = audio::builtinPresets();
    if (state.selectedPreset >= (int)presets.size()) state.selectedPreset = 0;
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##preset", presets[state.selectedPreset].name.c_str())) {
        for (int i = 0; i < (int)presets.size(); ++i)
            if (ImGui::Selectable(presets[i].name.c_str(), i == state.selectedPreset))
                state.selectedPreset = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("불러오기##preset")) {
        p = presets[state.selectedPreset].params;
        changed = true;
        state.statusMessage = "프리셋 적용: " + presets[state.selectedPreset].name;
    }
    ImGui::SameLine();
    if (ImGui::Button("파일 저장")) state.presetSaveRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("파일 열기")) state.presetLoadRequested = true;

    ImGui::Separator();

    // 파형
    const char* waves[] = {"Sine", "Saw", "Square", "Triangle"};
    int wave = (int)p.waveform;
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("파형", &wave, waves, IM_ARRAYSIZE(waves))) {
        p.waveform = (audio::Waveform)wave;
        changed = true;
    }

    ImGui::TextDisabled("각 슬라이더 옆 [Learn]을 누르고 컨트롤러 노브를 움직이면 CC가 연결됩니다.");

    using mapping::ParamTarget;
    ImGui::SeparatorText("ADSR 엔벨로프");
    changed |= learnableSlider(state, "Attack",  ParamTarget::Attack,  p.adsr.attackSec,  "%.3f s");
    changed |= learnableSlider(state, "Decay",   ParamTarget::Decay,   p.adsr.decaySec,   "%.3f s");
    changed |= learnableSlider(state, "Sustain", ParamTarget::Sustain, p.adsr.sustain,    "%.2f");
    changed |= learnableSlider(state, "Release", ParamTarget::Release, p.adsr.releaseSec, "%.3f s");

    ImGui::SeparatorText("필터 (저역통과)");
    changed |= learnableSlider(state, "Cutoff", ParamTarget::FilterCutoff, p.filterCutoff, "%.0f Hz");
    changed |= learnableSlider(state, "Resonance", ParamTarget::FilterResonance, p.filterResonance,
                               "%.2f");

    ImGui::SeparatorText("LFO (필터 변조)");
    changed |= learnableSlider(state, "Rate", ParamTarget::LfoRate, p.lfoRateHz, "%.2f Hz");
    changed |= learnableSlider(state, "Depth", ParamTarget::LfoDepth, p.lfoDepth, "%.2f");

    ImGui::SeparatorText("딜레이 (간이 리버브)");
    changed |= ImGui::SliderFloat("Time", &p.delayTimeSec, 0.02f, 0.9f, "%.2f s");
    changed |= ImGui::SliderFloat("Feedback", &p.delayFeedback, 0.0f, 0.95f, "%.2f");
    changed |= learnableSlider(state, "Mix", ParamTarget::DelayMix, p.delayMix, "%.2f");

    ImGui::SeparatorText("마스터");
    changed |= learnableSlider(state, "Volume", ParamTarget::MasterVolume, p.masterVolume, "%.2f");

    // ---- 컨트롤러 매핑 관리 ----
    ImGui::SeparatorText("컨트롤러 매핑 (MIDI Learn)");
    if (state.learnArmed)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "학습 대기 중: [%s] — 노브를 움직이세요",
                           mapping::paramInfo(state.learnTarget).name);
    const auto& maps = state.midiMap.list();
    if (maps.empty()) {
        ImGui::TextDisabled("매핑 없음");
    } else {
        for (const auto& m : maps)
            ImGui::BulletText("CC%d (ch%d) -> %s", m.cc, m.channel + 1,
                              mapping::paramInfo(m.target).name);
    }
    if (ImGui::Button("매핑 저장")) state.mapSaveRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("매핑 열기")) state.mapLoadRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("모두 해제")) state.midiMap.clearAll();

    ImGui::Separator();
    if (ImGui::Button("음색 기본값으로")) {
        p = audio::SynthParams{};
        changed = true;
    }

    // 변경 시에만 오디오 스레드로 전달 (매 프레임 큐 flooding 방지)
    if (changed) state.synth->setParams(p);
}

void drawSynth(AppState& state) {
    if (!state.showSynth) return; // Tool 메뉴로 별도 창을 원할 때만
    // 처음 열릴 때 화면 중앙에 팝업처럼 뜬다. X로 닫으면 showSynth=false.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(430, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("신디사이저", &state.showSynth);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)
    drawSynthBody(state);
    ImGui::End();
}

// ASIO 장치 검색/선택 + 버퍼(레이턴시) 설정. 개인설정 탭에서 그린다.
static void drawAsioBody(AppState& state) {
    if (!state.audioInput) {
        ImGui::TextDisabled("오디오 입력을 사용할 수 없습니다.");
        return;
    }
    auto* in = state.audioInput;

    ImGui::TextDisabled("ASIO는 오디오 인터페이스 드라이버와 직접 통신해 지연을 최소화합니다.\n"
                        "(모니터/녹음은 트랙 뷰의 각 트랙에서 시작합니다)");

    ImGui::SeparatorText("ASIO 장치");
    ImGui::TextDisabled("시작할 때 자동으로 검색하고 첫 장치를 선택합니다.");
    // 드라이버 로드가 무거워 매 프레임이 아니라 버튼을 누를 때만 다시 스캔한다.
    if (ImGui::Button("다시 검색##asio")) {
        state.asioDevices = in->listAsioDevices();
        state.asioDeviceIndex = 0;
        state.statusMessage = state.asioDevices.empty() ? "ASIO 장치를 찾지 못함"
                                                        : "ASIO 장치 검색 완료";
    }
    ImGui::SameLine();
    if (state.asioDevices.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "장치를 찾지 못했습니다");
    else
        ImGui::TextDisabled("%d개 발견", (int)state.asioDevices.size());

    if (!state.asioDevices.empty()) {
        const char* preview = (state.asioDeviceIndex >= 0 &&
                               state.asioDeviceIndex < (int)state.asioDevices.size())
                                  ? state.asioDevices[state.asioDeviceIndex].c_str()
                                  : "(선택)";
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("사용할 장치", preview)) {
            for (int d = 0; d < (int)state.asioDevices.size(); ++d)
                if (ImGui::Selectable(state.asioDevices[d].c_str(), d == state.asioDeviceIndex))
                    state.asioDeviceIndex = d;
            ImGui::EndCombo();
        }
    }

    ImGui::Text("상태: %s", in->asioActive() ? "ASIO 실행 중" : "정지");
    if (in->asioActive()) {
        ImGui::SameLine();
        if (ImGui::Button("ASIO 정지##pref")) {
            in->stopAsio();
            state.asioTrack = -1;
        }
    }

    ImGui::SeparatorText("버퍼 (레이턴시)");
    const double sr = state.audioClips ? state.audioClips->engineSampleRate() : 48000.0;
    if (in->asioActive()) {
        // ASIO는 드라이버가 버퍼·샘플레이트를 정한다 — 여기서는 보여주기만.
        ImGui::Text("ASIO 자동 (드라이버 권장): %u 프레임  ≈ %.1f ms", in->bufferFrames(),
                    1000.0 * (double)in->bufferFrames() / std::max(1.0, sr));
        ImGui::TextDisabled("ASIO에서는 버퍼·샘플레이트를 드라이버 제어판에서 바꿉니다.");
    } else {
        // WASAPI(기본 출력): 사용자가 버퍼 크기와 샘플레이트를 직접 고른다.
        ImGui::TextDisabled("작을수록 지연이 줄지만, 너무 작으면 소리가 끊깁니다.");
        static const unsigned kBufSizes[] = {64, 128, 256, 512, 1024, 2048};
        char blabel[48];
        std::snprintf(blabel, sizeof(blabel), "%u 프레임 (%.1f ms)", in->bufferFrames(),
                      1000.0 * (double)in->bufferFrames() / std::max(1.0, sr));
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("버퍼 크기", blabel)) {
            for (unsigned b : kBufSizes) {
                char it[48];
                std::snprintf(it, sizeof(it), "%u 프레임 (%.1f ms)", b,
                              1000.0 * (double)b / std::max(1.0, sr));
                if (ImGui::Selectable(it, b == in->bufferFrames())) in->setBufferFrames(b);
            }
            ImGui::EndCombo();
        }

        const auto rates = in->supportedSampleRates();
        if (!rates.empty()) {
            const unsigned cur = (unsigned)std::lround(sr);
            char rlabel[32];
            std::snprintf(rlabel, sizeof(rlabel), "%u Hz", cur);
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("샘플레이트", rlabel)) {
                // '장치 기본'으로 되돌리는 선택지
                if (ImGui::Selectable("장치 기본값", in->preferredSampleRate() == 0))
                    in->setPreferredSampleRate(0);
                for (unsigned r : rates) {
                    char it[32];
                    std::snprintf(it, sizeof(it), "%u Hz", r);
                    if (ImGui::Selectable(it, r == cur)) in->setPreferredSampleRate(r);
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("※ 바꾸면 출력을 잠깐 다시 엽니다. 올려둔 악기도 새 값으로 맞춰집니다.");
        }
    }
}

// 배경 이미지 '직접 지정' 모드의 크기·위치 조절 (전체 배경·창 배경 공용).
// 위치는 0=왼쪽/위, 0.5=가운데, 1=오른쪽/아래 (이미지가 크면 그만큼 밀린다).
static bool drawBgPlacement(float& scale, float& posX, float& posY, const char* idSuffix) {
    bool changed = false;
    char id[32];
    ImGui::Indent(12.0f);
    std::snprintf(id, sizeof(id), "크기##bgpl%s", idSuffix);
    ImGui::SetNextItemWidth(200);
    float pct = scale * 100.0f;
    if (ImGui::DragFloat(id, &pct, 1.0f, 2.0f, 2000.0f, "%.0f%%")) {
        scale = std::clamp(pct / 100.0f, 0.02f, 20.0f);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("원본 크기 대비 배율입니다 (100%% = 원본 그대로).\n"
                          "드래그로 조절, 더블클릭으로 직접 입력.");
    std::snprintf(id, sizeof(id), "가로 위치##bgpl%s", idSuffix);
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat(id, &posX, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0=왼쪽 · 0.5=가운데 · 1=오른쪽");
    std::snprintf(id, sizeof(id), "세로 위치##bgpl%s", idSuffix);
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat(id, &posY, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0=위 · 0.5=가운데 · 1=아래");
    std::snprintf(id, sizeof(id), "가운데·원본 크기로##bgpl%s", idSuffix);
    if (ImGui::SmallButton(id)) {
        scale = 1.0f;
        posX = posY = 0.5f;
        changed = true;
    }
    ImGui::Unindent(12.0f);
    return changed;
}

// 배경 이미지 레이어 목록 편집 (전체 배경·창별 배경 공용).
// target: -1 = 전체 배경, 0.. = 그 창. 바뀌면 true.
static bool drawBgLayers(AppState& state, std::vector<BgLayer>& layers, int target) {
    bool changed = false;
    const char* sfx = target < 0 ? "g" : "w";
    char id[64];

    std::snprintf(id, sizeof(id), "이미지 추가...##bgl%s", sfx);
    if (ImGui::Button(id)) {
        state.bgImageTargetWindow = target;
        state.bgImageOpenRequested = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("여러 장을 넣을 수 있습니다 — 목록 위쪽이 아래에 깔리고\n"
                          "아래쪽이 그 위에 겹쳐집니다 (예: 벽지 + 구석 로고).");
    if (layers.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(target < 0 ? "창 전체 뒤에 깔립니다" : "이 창 안쪽에만 깔립니다");
        return changed;
    }

    // 목록: 아래→위 순서. 선택한 레이어만 아래에서 자세히 조절한다.
    int sel = std::clamp(state.bgLayerSel, 0, (int)layers.size() - 1);
    int moveFrom = -1, moveTo = -1, removeAt = -1;
    for (int i = 0; i < (int)layers.size(); ++i) {
        ImGui::PushID(i);
        bool vis = layers[(std::size_t)i].visible;
        if (ImGui::Checkbox("##vis", &vis)) {
            layers[(std::size_t)i].visible = vis;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("잠시 끄기/켜기");
        ImGui::SameLine();
        std::string nm = layers[(std::size_t)i].image;
        const std::size_t sl = nm.find_last_of("/\\");
        if (sl != std::string::npos) nm = nm.substr(sl + 1);
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "%d. %s", i + 1, nm.c_str());
        if (ImGui::Selectable(lbl, i == sel, 0, ImVec2(240, 0))) sel = i;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", layers[(std::size_t)i].image.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("▲")) { moveFrom = i; moveTo = i - 1; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= (int)layers.size());
        if (ImGui::SmallButton("▼")) { moveFrom = i; moveTo = i + 1; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("삭제")) removeAt = i;
        ImGui::PopID();
    }
    if (moveFrom >= 0) { // 겹치는 순서 바꾸기
        std::swap(layers[(std::size_t)moveFrom], layers[(std::size_t)moveTo]);
        sel = moveTo;
        state.bgLayersDirty = target < 0 ? 0 : target + 1; // 텍스처 순서도 맞춘다
        changed = true;
    }
    if (removeAt >= 0) {
        layers.erase(layers.begin() + removeAt);
        if (sel >= (int)layers.size()) sel = (int)layers.size() - 1;
        state.bgLayersDirty = target < 0 ? 0 : target + 1;
        changed = true;
    }
    state.bgLayerSel = sel < 0 ? 0 : sel;
    if (layers.empty()) return changed;

    // 선택한 레이어의 설정
    BgLayer& L = layers[(std::size_t)state.bgLayerSel];
    ImGui::Separator();
    ImGui::TextDisabled("%d번 이미지 설정", state.bgLayerSel + 1);
    std::snprintf(id, sizeof(id), "진하기##bgl%s", sfx);
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat(id, &L.opacity, 0.0f, 1.0f, "%.2f");
    std::snprintf(id, sizeof(id), "표시 방식##bgl%s", sfx);
    ImGui::SetNextItemWidth(160);
    const char* kFits[] = {"채우기 (잘림)", "맞추기 (여백)", "타일", "직접 지정"};
    changed |= ImGui::Combo(id, &L.fit, kFits, 4);
    if (L.fit == 3) changed |= drawBgPlacement(L.scale, L.posX, L.posY, sfx);
    return changed;
}

// ---------------------------------------------------------
// 테마: 프리셋 + 간단 파라미터(강조색/배경/글자/둥글기) + 고급 편집기
// ---------------------------------------------------------
void drawThemeBody(AppState& state) {
    ThemeParams& t = state.theme;
    bool changed = false;

    // 처음 쓰는 사람이 겁먹지 않도록 단계적으로 보여준다.
    // 0=기본(프리셋만) · 1=자세히(색 조절) · 2=고급(창별·내 테마·개별 색)
    const int lv = std::clamp(state.themeUiLevel, 0, 2);

    ImGui::TextDisabled("테마 고르기 — 누르면 바로 적용됩니다");
    {
        // 프리셋 버튼에 그 테마의 실제 색을 칠해 준다 (이름만 보고 상상 안 하도록).
        // 줄바꿈은 창 폭에 맞춰 자동 — 프리셋이 늘어도 UI를 안 고쳐도 된다.
        const ThemePreset* ps = themePresets();
        const int n = themePresetCount();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float btnW = 112.0f;
        float used = 0.0f;
        for (int i = 0; i < n; ++i) {
            const ThemeParams pp = ps[i].make();
            if (i > 0 && used + btnW < avail) ImGui::SameLine();
            else used = 0.0f;
            // 버튼 = 그 테마의 배경색, 글자 = 그 테마의 글자색 (미리보기 역할)
            const ImVec4 bgc(std::clamp(pp.bg + 0.06f, 0.0f, 1.0f),
                             std::clamp(pp.bg + 0.06f, 0.0f, 1.0f),
                             std::clamp(pp.bg + 0.08f, 0.0f, 1.0f), 1.0f);
            const ImVec4 acc(pp.accent[0], pp.accent[1], pp.accent[2], 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, bgc);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, acc);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, acc);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(pp.text, pp.text, pp.text, 1.0f));
            ImGui::PushID(i);
            const bool hit = ImGui::Button(ps[i].name, ImVec2(btnW, 0));
            // 강조색 띠를 버튼 아래쪽에 그려 색을 한눈에 알 수 있게
            {
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(mn.x + 3.0f, mx.y - 5.0f), ImVec2(mx.x - 3.0f, mx.y - 2.0f),
                    ImGui::GetColorU32(acc), 1.5f);
            }
            ImGui::PopID();
            ImGui::PopStyleColor(4);
            if (hit) {
                // 배경 이미지·위젯 스킨은 그대로 두고 색만 바꾼다
                auto keepLayers = std::move(t.bgLayers);
                UiSkin keepSkins[kSkinSlotCount];
                for (int k = 0; k < kSkinSlotCount; ++k) keepSkins[k] = t.skins[k];
                t = ps[i].make();
                t.bgLayers = std::move(keepLayers);
                for (int k = 0; k < kSkinSlotCount; ++k) t.skins[k] = keepSkins[k];
                changed = true;
            }
            used += btnW + ImGui::GetStyle().ItemSpacing.x;
        }
    }

    // 기본 단계: 밝기 하나만 + 안전장치
    ImGui::Spacing();
    ImGui::SetNextItemWidth(220);
    changed |= ImGui::SliderFloat("밝기", &t.bg, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("왼쪽=어둡게, 오른쪽=밝게");
    ImGui::SameLine();
    if (ImGui::Button("기본으로 되돌리기")) state.themeResetRequested = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("색·창별 설정·배경 이미지를 모두 처음 상태로 되돌립니다.\n"
                          "저장해 둔 '내 테마'는 지워지지 않습니다.");

    // 단계 전환
    ImGui::Spacing();
    if (lv == 0) {
        if (ImGui::SmallButton("더 자세히 ▾")) state.themeUiLevel = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("강조색·글자·모서리·배경 이미지를 직접 조절합니다");
    } else {
        if (ImGui::SmallButton("간단히 ▴")) state.themeUiLevel = 0;
        if (lv == 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton("고급 설정 ▾")) state.themeUiLevel = 2;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("창별로 따로 꾸미기 · 내 테마 저장/주고받기 · 개별 색 편집");
        }
    }

    if (lv == 0) { // 기본 단계는 여기서 끝 (초보가 볼 건 이게 전부)
        if (changed) {
            applyThemeParams(t);
            state.themeDirty = true;
        }
        return;
    }

    ImGui::Separator();
    ImGui::TextDisabled("직접 꾸미기 (바꾸면 즉시 적용·자동 저장)");
    // ---- 적용 대상 (고급 단계에서만): 전체 또는 특정 창 ----
    if (lv >= 2) {
        ImGui::SetNextItemWidth(190);
        const char* cur = state.themeTargetWindow < 0
                              ? "전체 (기본)"
                              : themeWindowName(state.themeTargetWindow);
        if (ImGui::BeginCombo("적용 대상", cur)) {
            if (ImGui::Selectable("전체 (기본)", state.themeTargetWindow < 0))
                state.themeTargetWindow = -1;
            ImGui::Separator();
            for (int i = 0; i < kThemeWindowCount; ++i) {
                char lbl[64];
                std::snprintf(lbl, sizeof(lbl), "%s%s", themeWindowName(i),
                              state.windowStyles[i].enabled ? "  *" : "");
                if (ImGui::Selectable(lbl, state.themeTargetWindow == i))
                    state.themeTargetWindow = i;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("특정 창을 고르면 그 창만 다르게 꾸밀 수 있습니다.\n"
                              "* 표시 = 이미 따로 설정된 창");
    } else {
        state.themeTargetWindow = -1; // 자세히 단계에서는 항상 전체
    }

    if (state.themeTargetWindow < 0) {
        // 전체 테마 (밝기는 위 기본 단계에 이미 있으므로 여기선 뺀다)
        changed |= ImGui::ColorEdit3("강조색", t.accent);
        changed |= ImGui::SliderFloat("글자 밝기", &t.text, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("모서리 둥글기", &t.rounding, 0.0f, 12.0f, "%.0f");
        changed |= ImGui::SliderFloat("패널 불투명도", &t.panelAlpha, 0.15f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("창(패널) 배경의 불투명도입니다.\n"
                              "낮추면 뒤에 깔아둔 배경 이미지가 비쳐 보입니다.");
    } else {
        // 창 하나만: 체크한 항목만 덮어쓰고 나머지는 전체 테마를 따라간다
        WindowStyleOverride& ov = state.windowStyles[state.themeTargetWindow];
        if (ImGui::Checkbox("이 창만 다르게", &ov.enabled)) {
            // 처음 켤 때 현재 전체 값에서 출발하면 조절이 자연스럽다
            if (ov.enabled && !ov.anyField()) {
                ov.accent[0] = t.accent[0];
                ov.accent[1] = t.accent[1];
                ov.accent[2] = t.accent[2];
                ov.bg = t.bg;
                ov.text = t.text;
                ov.rounding = t.rounding;
                ov.panelAlpha = t.panelAlpha;
            }
            changed = true;
        }
        ImGui::TextDisabled("체크한 항목만 이 창에 적용되고, 나머지는 전체 설정을 따라갑니다.");
        ImGui::BeginDisabled(!ov.enabled);
        changed |= ImGui::Checkbox("##ua", &ov.useAccent);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov.useAccent);
        changed |= ImGui::ColorEdit3("강조색##w", ov.accent);
        ImGui::EndDisabled();

        changed |= ImGui::Checkbox("##ub", &ov.useBg);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov.useBg);
        changed |= ImGui::SliderFloat("배경 밝기##w", &ov.bg, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();

        changed |= ImGui::Checkbox("##ut", &ov.useText);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov.useText);
        changed |= ImGui::SliderFloat("글자 밝기##w", &ov.text, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();

        changed |= ImGui::Checkbox("##ur", &ov.useRounding);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov.useRounding);
        changed |= ImGui::SliderFloat("모서리 둥글기##w", &ov.rounding, 0.0f, 12.0f, "%.0f");
        ImGui::EndDisabled();

        changed |= ImGui::Checkbox("##up", &ov.usePanelAlpha);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov.usePanelAlpha);
        changed |= ImGui::SliderFloat("패널 불투명도##w", &ov.panelAlpha, 0.15f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // 이 창만의 배경 이미지들
        ImGui::Spacing();
        ImGui::TextDisabled("이 창의 배경 이미지 (여러 장 가능)");
        changed |= drawBgLayers(state, ov.bgLayers, state.themeTargetWindow);
        if (!ov.bgLayers.empty())
            ImGui::TextDisabled("※ 이 창의 패널 불투명도를 낮춰야 잘 보입니다.");

        if (ImGui::Button("전체 설정 따라가기로 되돌리기")) {
            ov = WindowStyleOverride{}; // 배경 레이어도 함께 비워진다
            state.bgLayersDirty = state.themeTargetWindow + 1; // 텍스처도 정리
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("이 창의 개별 설정을 지우고 전체 테마를 그대로 씁니다.");
        ImGui::TextDisabled("※ 창별 설정을 켜면 그 창은 위 항목으로 팔레트를 새로 만듭니다\n"
                            "   (아래 고급 편집기의 개별 색은 전체 창에만 적용됩니다).");
    }

    // ---- 배경 이미지/GIF ----
    ImGui::Separator();
    ImGui::TextDisabled("배경 이미지 (PNG · JPG · BMP · 움직이는 GIF) — 여러 장 가능");
    if (state.bgImageInfo[0] && !t.bgLayers.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", state.bgImageInfo);
    }
    changed |= drawBgLayers(state, t.bgLayers, -1);
    if (!t.bgLayers.empty() && t.panelAlpha > 0.97f)
        ImGui::TextDisabled("※ 패널 불투명도를 낮춰야 배경이 잘 보입니다.");

    // ---- 위젯 스킨: 버튼·탭·제목 표시줄 (창별이 아니라 모든 창 공통) ----
    ImGui::Separator();
    ImGui::TextDisabled("버튼 · 탭 · 제목 표시줄 이미지 (모든 창 공통)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("창마다 따로가 아니라 앱 전체에 한 벌만 적용됩니다.\n"
                          "움직이는 GIF는 첫 장만 쓰입니다.");
    for (int i = 0; i < kSkinSlotCount; ++i) {
        UiSkin& sk = t.skins[i];
        ImGui::PushID(1000 + i);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", uiSkinSlotName(i));
        ImGui::SameLine(110);
        if (sk.image.empty()) {
            if (ImGui::Button("이미지 넣기...")) {
                state.skinImageSlot = i;
                state.skinImageOpenRequested = true;
            }
        } else {
            std::string nm = sk.image;
            const std::size_t sl = nm.find_last_of("/\\");
            if (sl != std::string::npos) nm = nm.substr(sl + 1);
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", nm.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sk.image.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("바꾸기")) {
                state.skinImageSlot = i;
                state.skinImageOpenRequested = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("빼기")) {
                sk.image.clear();
                changed = true;
            }
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("진하기", &sk.opacity, 0.1f, 1.0f, "%.2f")) changed = true;
            // 붙이는 범위 오프셋 (픽셀). 늘리면 넘쳐서 잘리고, 줄이면 안쪽으로 당겨진다.
            const float ow = 66.0f;
            ImGui::SetNextItemWidth(ow);
            if (ImGui::DragFloat("##ofsL", &sk.ofsL, 0.5f, -64.0f, 64.0f, "왼 %.0f"))
                changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ow);
            if (ImGui::DragFloat("##ofsR", &sk.ofsR, 0.5f, -64.0f, 64.0f, "오 %.0f"))
                changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ow);
            if (ImGui::DragFloat("##ofsT", &sk.ofsT, 0.5f, -64.0f, 64.0f, "위 %.0f"))
                changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ow);
            if (ImGui::DragFloat("##ofsB", &sk.ofsB, 0.5f, -64.0f, 64.0f, "아래 %.0f"))
                changed = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("0으로")) {
                sk.ofsL = sk.ofsR = sk.ofsT = sk.ofsB = 0.0f;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("네 방향 오프셋을 0으로 되돌립니다.");
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("※ 마우스를 올리거나 누른 상태는 같은 이미지를 밝기만 달리해 보여줍니다.\n"
                        "   왼/오/위/아래는 이미지를 붙일 범위(px) — 키우면 그 방향으로 넘쳐\n"
                        "   잘려 보이고, 줄이면 안쪽으로 당겨집니다.");

    if (changed) {
        applyThemeParams(t);
        state.themeDirty = true;
    }

    if (lv < 2) return; // 자세히 단계는 여기까지 (적용은 바로 위에서 했다)

    // ---- 내 테마: 전체 + 창별 설정을 통째로 이름 붙여 저장 ----
    ImGui::Separator();
    ImGui::TextDisabled("내 테마 (지금 설정을 통째로 저장 — 창별 설정·배경까지 포함)");
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##themename", "이름 (예: 야간 작업용)", state.themeSaveName,
                             sizeof(state.themeSaveName));
    ImGui::SameLine();
    ImGui::BeginDisabled(state.themeSaveName[0] == '\0');
    if (ImGui::Button("저장##theme")) state.themeSaveRequested = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("같은 이름이 있으면 덮어씁니다.");

    if (state.themeFiles.empty()) {
        ImGui::TextDisabled("저장된 테마가 없습니다.");
    } else {
        for (const auto& name : state.themeFiles) {
            ImGui::PushID(name.c_str());
            if (ImGui::SmallButton("불러오기")) state.themeLoadRequested = name;
            ImGui::SameLine();
            if (ImGui::SmallButton("내보내기")) state.themeExportRequested = name;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("배경 이미지까지 담아 한 파일로 저장합니다.\n"
                                  "그 파일을 남에게 보내면 그대로 쓸 수 있습니다.");
            ImGui::SameLine();
            if (ImGui::SmallButton("삭제")) state.themeDeleteRequested = name;
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopID();
        }
    }
    if (ImGui::Button("테마 파일 가져오기...")) state.themeImportRequested = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("남이 보내준 .mptheme 파일을 불러옵니다 (배경 이미지 포함).\n"
                          "파일을 창에 끌어다 놓아도 됩니다.");

    ImGui::Separator();
    ImGui::TextDisabled("세부 조정이 필요하면 (색상 하나하나 개별 수정)");
    ImGui::Checkbox("고급 스타일 편집기", &state.showStyleEditor);
    ImGui::TextDisabled("고급 편집기에서 바꾼 색도 프로그램 종료 시 함께 저장됩니다.\n"
                        "단, 위의 간단 설정을 다시 만지면 전체 팔레트가 새로 생성됩니다.");
}

// 고급 편집기는 개인설정 창과 별개의 창으로 띄운다 (넓게 써야 해서).
// ImGui 색상 전체(ImGuiCol_*)를 이름 검색 + 개별 색 편집으로 노출한다.
void drawStyleEditorWindow(AppState& state) {
    if (!state.showStyleEditor) return;
    ImGui::SetNextWindowSize(ImVec2(430, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("스타일 편집기 (고급)", &state.showStyleEditor)) {
        ImGuiStyle& s = ImGui::GetStyle();
        bool changed = false;

        ImGui::TextDisabled("UI 요소별 색을 개별 수정합니다. 마우스를 올리면 어떤 요소인지\n"
                            "이름이 영어로 표시됩니다 (Button=버튼, WindowBg=창 배경...).");
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-80);
        ImGui::InputTextWithHint("##stylefilter", "이름으로 검색 (예: Button)", filter,
                                 sizeof(filter));
        ImGui::SameLine();
        if (ImGui::Button("전체 초기화")) {
            applyThemeParams(state.theme); // 간단 파라미터로 팔레트 재생성
            changed = true;
        }
        ImGui::Separator();

        ImGui::BeginChild("style_cols", ImVec2(0, 0), false);
        auto containsNoCase = [](const char* hay, const char* needle) {
            if (!needle[0]) return true;
            std::string h(hay), n(needle);
            for (auto& ch : h) ch = (char)tolower((unsigned char)ch);
            for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            return h.find(n) != std::string::npos;
        };
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const char* name = ImGui::GetStyleColorName(i);
            if (!containsNoCase(name, filter)) continue;
            ImGui::PushID(i);
            changed |= ImGui::ColorEdit4(name, (float*)&s.Colors[i],
                                         ImGuiColorEditFlags_AlphaBar);
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (changed) state.themeDirty = true;
    }
    ImGui::End();
}

// ---------------------------------------------------------
// 개인설정: ASIO / 메트로놈 / MIDI 장치 / 신디사이저를 탭으로 분류해 한 창에 모음
// ---------------------------------------------------------
void drawPreferences(AppState& state) {
    if (!state.showPreferences) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(470, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("개인설정", &state.showPreferences);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)
    if (ImGui::BeginTabBar("prefs_tabs")) {
        if (ImGui::BeginTabItem("ASIO")) {
            drawAsioBody(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI 장치")) {
            drawDevicesBody(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("신디사이저")) {
            // 내용이 길어 탭 안에서 독립 스크롤되게 자식 영역에 담는다.
            ImGui::BeginChild("synth_scroll", ImVec2(0, 0), false);
            drawSynthBody(state);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("테마")) {
            drawThemeBody(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    drawStyleEditorWindow(state);
}

// ---------------------------------------------------------
// VST3 플러그인 (악기/이펙트)
// ---------------------------------------------------------
void drawVst(AppState& state) {
    if (!state.showVst) return;
    ImGui::Begin("VST3 플러그인", &state.showVst);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    if (state.vst == nullptr) {
        ImGui::TextDisabled("VST 호스트를 사용할 수 없습니다.");
        ImGui::End();
        return;
    }

    // ---- 설치된 플러그인 자동 스캔 ----
    // 처음 열릴 때 한 번 자동으로 표준 폴더를 훑는다.
    if (!state.vstScanDone) {
        state.vstScanned = vst::scanVst3Plugins();
        state.vstScanDone = true;
    }
    ImGui::SeparatorText("설치된 플러그인");
    if (ImGui::Button("새로고침")) {
        state.vstScanned = vst::scanVst3Plugins();
        state.vstEffectsFiltered = false; // +FX 필터 캐시 무효화
        state.vstPickInstrument = std::min(state.vstPickInstrument, (int)state.vstScanned.size() - 1);
        state.vstPickEffect = std::min(state.vstPickEffect, (int)state.vstScanned.size() - 1);
    }
    ImGui::SameLine();
    ImGui::Text("%d개 발견", (int)state.vstScanned.size());
    ImGui::SameLine();
    if (ImGui::Button("다시 조사")) {
        // 악기/이펙트 판정은 플러그인을 실제로 열어 봐야 해서 느리다 —
        // 그래서 결과를 기억해 둔다. 이 버튼은 그 기억을 버린다.
        state.vst->clearPluginCache();
        state.vstInstrumentsFiltered = false;
        state.vstEffectsFiltered = false;
        state.statusMessage = "플러그인 조사 결과를 지웠습니다 (다음에 다시 열어 봅니다)";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("악기/이펙트 구분이 잘못됐을 때만 누르세요.\n"
                          "다음에 목록을 열 때 플러그인을 다시 열어 보므로 느려집니다.");
    if (state.vstScanned.empty())
        ImGui::TextDisabled("표준 VST3 폴더에서 플러그인을 찾지 못했습니다.");

    // ---- 악기 (VSTi) ----
    ImGui::SeparatorText("악기 (VSTi)");
    if (!state.vst->instrumentActive() && !state.vstScanned.empty()) {
        if (state.vstPickInstrument < 0) state.vstPickInstrument = 0;
        if (state.vstPickInstrument >= (int)state.vstScanned.size())
            state.vstPickInstrument = 0;
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("##pickinst", state.vstScanned[state.vstPickInstrument].name.c_str())) {
            for (int i = 0; i < (int)state.vstScanned.size(); ++i)
                if (ImGui::Selectable(state.vstScanned[i].name.c_str(), i == state.vstPickInstrument))
                    state.vstPickInstrument = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("불러오기##pickinst")) {
            const auto& entry = state.vstScanned[state.vstPickInstrument];
            std::string err;
            if (state.vst->loadInstrument(entry.path, 0, err)) {
                state.vstInstrumentPath = entry.path;
                state.vstInstrumentClass = 0;
                state.statusMessage = "VST 악기 로드: " + state.vst->instrumentHost().activeName();
            } else {
                state.statusMessage = "VST 악기 로드 실패: " + err;
            }
        }
    }
    auto& ih = state.vst->instrumentHost();
    if (state.vst->instrumentActive()) {
        ImGui::Text("로드됨: %s", ih.activeName().c_str());
        // 클래스가 여러 개면 선택해 재로드
        const auto& cls = ih.classes();
        if (cls.size() > 1) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("클래스##inst", cls[state.vstInstrumentClass].name.c_str())) {
                for (int i = 0; i < (int)cls.size(); ++i)
                    if (ImGui::Selectable(cls[i].name.c_str(), i == state.vstInstrumentClass)) {
                        state.vstInstrumentClass = i;
                        std::string err;
                        state.vst->loadInstrument(state.vstInstrumentPath, i, err);
                    }
                ImGui::EndCombo();
            }
        }
        if (ImGui::Button("에디터 열기##inst")) ih.openEditor();
        ImGui::SameLine();
        if (ImGui::Button("해제##inst")) state.vst->clearInstrument();

        // 출력 라우팅: 트랙 버스로 보내면 그 트랙의 볼륨/팬/이펙트가 걸린다.
        {
            if (state.vstInstrumentTrack >= (int)state.song.tracks.size())
                state.vstInstrumentTrack = -1;
            const char* preview =
                state.vstInstrumentTrack < 0
                    ? "마스터 직행"
                    : state.song.tracks[(std::size_t)state.vstInstrumentTrack].name.c_str();
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("출력 트랙##instbus", preview)) {
                if (ImGui::Selectable("마스터 직행", state.vstInstrumentTrack < 0))
                    state.vstInstrumentTrack = -1;
                for (int ti = 0; ti < (int)state.song.tracks.size(); ++ti) {
                    if (state.song.tracks[(std::size_t)ti].practice) continue;
                    if (ImGui::Selectable(state.song.tracks[(std::size_t)ti].name.c_str(),
                                          ti == state.vstInstrumentTrack))
                        state.vstInstrumentTrack = ti;
                }
                ImGui::EndCombo();
            }
            if (state.vstInstrumentTrack >= 0)
                ImGui::TextDisabled("이 트랙의 볼륨·팬·이펙트 체인이 악기 소리에 걸립니다.");
        }
        ImGui::TextDisabled("출력 대상을 '내장 신디사이저'로 열면 이 악기로 소리납니다.");
    } else {
        if (ImGui::Button("플러그인 선택...##inst")) state.vstInstrumentLoadRequested = true;
        ImGui::TextDisabled("VSTi를 불러오면 내장 신스 대신 이 악기가 발음합니다.");
    }

    // ---- 이펙트 ----
    ImGui::SeparatorText("이펙트 (출력 후처리)");
    if (!state.vst->effectActive() && !state.vstScanned.empty()) {
        if (state.vstPickEffect < 0 || state.vstPickEffect >= (int)state.vstScanned.size())
            state.vstPickEffect = 0;
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("##pickfx", state.vstScanned[state.vstPickEffect].name.c_str())) {
            for (int i = 0; i < (int)state.vstScanned.size(); ++i)
                if (ImGui::Selectable(state.vstScanned[i].name.c_str(), i == state.vstPickEffect))
                    state.vstPickEffect = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("불러오기##pickfx")) {
            const auto& entry = state.vstScanned[state.vstPickEffect];
            std::string err;
            if (state.vst->loadEffect(entry.path, 0, err)) {
                state.vstEffectPath = entry.path;
                state.vstEffectClass = 0;
                state.statusMessage = "VST 이펙트 로드: " + state.vst->effectHost().activeName();
            } else {
                state.statusMessage = "VST 이펙트 로드 실패: " + err;
            }
        }
    }
    auto& eh = state.vst->effectHost();
    if (state.vst->effectActive()) {
        ImGui::Text("로드됨: %s", eh.activeName().c_str());
        const auto& cls = eh.classes();
        if (cls.size() > 1) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("클래스##fx", cls[state.vstEffectClass].name.c_str())) {
                for (int i = 0; i < (int)cls.size(); ++i)
                    if (ImGui::Selectable(cls[i].name.c_str(), i == state.vstEffectClass)) {
                        state.vstEffectClass = i;
                        std::string err;
                        state.vst->loadEffect(state.vstEffectPath, i, err);
                    }
                ImGui::EndCombo();
            }
        }
        if (ImGui::Button("에디터 열기##fx")) eh.openEditor();
        ImGui::SameLine();
        if (ImGui::Button("해제##fx")) state.vst->clearEffect();
    } else {
        if (ImGui::Button("플러그인 선택...##fx")) state.vstEffectLoadRequested = true;
        ImGui::TextDisabled("리버브/컴프 등 이펙트로 내장 신스·악기 출력을 처리합니다.");
    }

    ImGui::End();
}

// ---------------------------------------------------------
// 기타 도우미
// ---------------------------------------------------------
void drawGuitarHelper(AppState& state) {
    if (!state.showGuitar) return;
    ImGui::Begin("기타 도우미", &state.showGuitar);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // ---- 튜닝 기준음 ----
    ImGui::SeparatorText("표준 튜닝 (누르면 재생)");
    const char* stringLabels[6] = {"6 E2", "5 A2", "4 D3", "3 G3", "2 B3", "1 E4"};
    for (int i = 0; i < guitar::kStringCount; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(stringLabels[i], ImVec2(56, 0))) {
            // 1.2초 뒤 자동으로 꺼지도록 예약 (버튼은 "쳤다 뗀다")
            triggerNote(state, 0, guitar::kStandardTuning[i], 100, 1.2);
        }
    }
    if (ImGui::Button("모든 소리 끄기") && state.output) {
        for (uint8_t ch = 0; ch < 16; ++ch)
            state.output->send({(uint8_t)(midi::kStatusControlChange | ch), 123, 0});
        state.pendingOffs.clear();
    }

    // ---- 악기 음색 ----
    ImGui::SeparatorText("기타 음색");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("GM 프로그램", &state.guitarProgram);
    state.guitarProgram = std::clamp(state.guitarProgram, 0, 127);
    ImGui::SameLine();
    if (ImGui::Button("적용") && state.output) {
        if (!state.output->isOpen()) state.output->openPort((unsigned)state.selectedOutputPort);
        state.output->send(midi::MidiMessage::makeProgramChange(0, (uint8_t)state.guitarProgram));
    }
    ImGui::TextDisabled("24 나일론 · 25 스틸 · 27 클린 · 29 오버드라이브 · 30 디스토션");

    // ---- 코드 지판 ----
    ImGui::SeparatorText("코드 지판");
    ImGui::SetNextItemWidth(80);
    const char* rootPreview = guitar::pitchClassName(state.guitarRoot);
    if (ImGui::BeginCombo("루트", rootPreview)) {
        for (int i = 0; i < 12; ++i)
            if (ImGui::Selectable(guitar::pitchClassName(i), i == state.guitarRoot))
                state.guitarRoot = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const auto& chords = guitar::commonChords();
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("코드", chords[state.guitarChordIndex].name.c_str())) {
        for (int i = 0; i < (int)chords.size(); ++i)
            if (ImGui::Selectable(chords[i].name.c_str(), i == state.guitarChordIndex))
                state.guitarChordIndex = i;
        ImGui::EndCombo();
    }

    // 코드 구성음의 pitch class 집합
    bool inChord[12] = {};
    for (int interval : chords[state.guitarChordIndex].intervals)
        inChord[(state.guitarRoot + interval) % 12] = true;

    ImGui::SameLine();
    if (ImGui::Button("코드 재생")) {
        // 구성음을 동시에 울리고 1.5초 뒤 자동으로 끈다 (스트럼처럼)
        for (int interval : chords[state.guitarChordIndex].intervals)
            triggerNote(state, 0, (uint8_t)(48 + state.guitarRoot + interval), 100, 1.5);
    }

    // ---- 지판 그리기 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr int kFrets = 12;
    constexpr float kFretW = 46.0f;
    constexpr float kStringGap = 20.0f;
    const float boardW = kFretW * (kFrets + 1);

    for (int s = 0; s < guitar::kStringCount; ++s) {
        // 화면 위쪽이 1번줄(고음)이 되도록 뒤집어 그린다
        const int stringIndex = guitar::kStringCount - 1 - s;
        const float y = origin.y + 10 + s * kStringGap;
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + boardW, y),
                    IM_COL32(120, 120, 130, 255));

        for (int f = 0; f <= kFrets; ++f) {
            const uint8_t note = guitar::noteAt(stringIndex, f);
            const int pc = note % 12;
            if (!inChord[pc]) continue;
            const float x = origin.x + f * kFretW + kFretW * 0.5f;
            const bool isRoot = (pc == state.guitarRoot % 12);
            dl->AddCircleFilled(ImVec2(x, y), 8.0f,
                                isRoot ? IM_COL32(250, 170, 60, 255)
                                       : IM_COL32(90, 170, 250, 255));
            dl->AddText(ImVec2(x - 6, y - 6), IM_COL32(20, 20, 20, 255),
                        guitar::pitchClassName(pc));
        }
    }
    // 프렛 세로선 + 번호
    for (int f = 0; f <= kFrets; ++f) {
        const float x = origin.x + f * kFretW;
        dl->AddLine(ImVec2(x, origin.y + 6),
                    ImVec2(x, origin.y + 6 + kStringGap * guitar::kStringCount),
                    IM_COL32(90, 90, 100, 255));
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%d", f);
        dl->AddText(ImVec2(x + kFretW * 0.5f - 4, origin.y + 6 + kStringGap * guitar::kStringCount),
                    IM_COL32(140, 140, 150, 255), buf);
    }
    ImGui::Dummy(ImVec2(boardW, kStringGap * guitar::kStringCount + 30));

    ImGui::End();
}

// ---------------------------------------------------------
// 입력 모니터
// ---------------------------------------------------------
void drawMonitor(AppState& state) {
    if (!state.showMonitor) return;
    ImGui::Begin("입력 모니터", &state.showMonitor);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    ImGui::Checkbox("모니터링", &state.monitorEnabled);
    ImGui::SameLine();
    if (ImGui::Button("지우기")) state.monitorLog.clear();
    ImGui::SameLine();
    if (state.input)
        ImGui::Text("드롭: %zu", state.input->droppedCount());

    ImGui::Separator();
    ImGui::BeginChild("monitor_log");
    for (const auto& line : state.monitorLog)
        ImGui::TextUnformatted(line.c_str());
    // 새 줄이 들어오면 맨 아래로 자동 스크롤
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

} // namespace midipro::gui
