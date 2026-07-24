// =============================================================
// MidiPro - gui/PanelsMixer.cpp
// 믹서(전체 스트립)/채널(컴팩트) 창 + 공용 미터·노브 위젯.
// Panels.cpp에서 분리 (동작 동일). 공용 위젯 선언은 PanelsInternal.h.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "audio/BuiltinFx.h"
#include "sequencer/TimeBase.h"

#include "imgui.h"
#include "imgui_internal.h" // FindWindowByName (채널 창을 믹서 옆에 도킹)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

namespace midipro::gui {
// ---------------------------------------------------------
// 믹서 (왼쪽=마스터, 오른쪽=선택 트랙 볼륨/팬)
// ---------------------------------------------------------
// (Panels.cpp의 익명 네임스페이스였던 위젯들 — 트랙 목록/트랙 뷰도 쓰므로 공개)

// 현재 칸(사용 가능 폭) 안에서 폭 w짜리 위젯이 가운데 오도록 커서를 옮긴다.
void centerNextItem(float w) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
}

// 원형 노브: 세로 드래그로 vmin~vmax 조절, 더블클릭 = vdefault 리셋.
// 노브 아래에 작은 영문 라벨(Pan/Gain 등)을 함께 그린다. 값이 바뀌면 true.
bool rotaryKnob(const char* id, const char* label, float* value, float vmin, float vmax,
                float vdefault, float radius) {
    const float labelH = ImGui::GetTextLineHeight();
    const ImVec2 size(radius * 2.0f, radius * 2.0f + labelH + 2.0f);
    centerNextItem(size.x);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 center(pos.x + radius, pos.y + radius);
    ImGui::InvisibleButton(id, size);

    bool changed = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        // 세로 드래그가 노브 표준 조작감: 위로 약 120px에 전체 범위.
        const float dy = ImGui::GetIO().MouseDelta.y;
        if (dy != 0.0f) {
            *value = std::clamp(*value - dy * (vmax - vmin) / 120.0f, vmin, vmax);
            changed = true;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *value = vdefault;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s %.2f (드래그: 조절 · 더블클릭: 초기화)", label, *value);

    // 그리기: 몸통 + 눈금 + 지침. 정규화 값 0~1을 위(-90°) 기준 ±135°에 매핑.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool active = ImGui::IsItemActive();
    dl->AddCircleFilled(center, radius, IM_COL32(52, 52, 60, 255), 32);
    dl->AddCircle(center, radius, active ? IM_COL32(140, 190, 255, 255) : IM_COL32(95, 95, 110, 255),
                  32, 1.5f);
    constexpr float kPi = 3.14159265f;
    const float kSweep = 0.75f * kPi; // 중앙에서 좌우로 135도
    // 최소/중앙/최대 눈금
    for (int k = -1; k <= 1; ++k) {
        const float a = -0.5f * kPi + kSweep * (float)k;
        const ImVec2 t0(center.x + std::cos(a) * (radius + 2.0f),
                        center.y + std::sin(a) * (radius + 2.0f));
        const ImVec2 t1(center.x + std::cos(a) * (radius + 5.0f),
                        center.y + std::sin(a) * (radius + 5.0f));
        dl->AddLine(t0, t1, IM_COL32(120, 120, 135, 255), 1.0f);
    }
    const float norm = (vmax > vmin) ? (*value - vmin) / (vmax - vmin) : 0.5f;
    const float ang = -0.5f * kPi + kSweep * (norm * 2.0f - 1.0f);
    const ImVec2 tip(center.x + std::cos(ang) * (radius - 4.0f),
                     center.y + std::sin(ang) * (radius - 4.0f));
    dl->AddLine(center, tip, IM_COL32(240, 210, 120, 255), 2.5f);
    dl->AddCircleFilled(center, 3.0f, IM_COL32(240, 210, 120, 255));

    // 노브 아래 작은 영문 라벨
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - ts.x * 0.5f, pos.y + radius * 2.0f + 2.0f),
                IM_COL32(160, 160, 175, 255), label);
    return changed;
}

// 가운데 정렬 텍스트 (칸 폭 기준)
void centeredText(const char* text) {
    centerNextItem(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
}

// 노브/체크박스 편집 시작 시 undo 지점을 남긴다. 노브는 값을 제자리에서
// 바꾸므로(더블클릭 리셋 포함) 이전 값으로 잠시 되돌려 "편집 전" 상태를
// 스냅샷에 담고 새 값을 다시 쓴다. 직전 위젯이 활성화된 프레임에만 동작.
void snapshotKnobEdit(AppState& state, float& live, float before) {
    if (!ImGui::IsItemActivated()) return;
    const float now = live;
    live = before;
    state.snapshot();
    live = now;
}

// 샘플 피크(선형)를 미터 높이(0~1)로. -60dB가 바닥, 0dB가 꼭대기.
// dB 스케일이어야 작은 소리의 변화도 눈에 보인다.
float peakToNorm(float peak) {
    if (peak <= 0.000001f) return 0.0f;
    const float db = 20.0f * std::log10(peak);
    return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

// 세로 미터 한 줄: 채움(초록/노랑/빨강) + 피크 홀드 라인. raw = 이번 프레임 피크.
void drawMeterBar(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
                  AppState::MeterView& m, float raw, float dt, double now) {
    const float h = p1.y - p0.y;
    const float norm = peakToNorm(raw);
    m.disp = std::max(norm, m.disp - dt * (40.0f / 60.0f)); // 하강 약 40dB/초
    if (norm >= m.hold || now - m.holdAt > 2.0) {           // 피크는 2초 홀드
        m.hold = norm;
        m.holdAt = now;
    }
    if (raw >= 1.0f) m.clip = true;

    dl->AddRectFilled(p0, p1, IM_COL32(22, 22, 27, 255), 2.0f);
    // 채움을 dB 구간별 색으로: ~-12dB 초록, -12~-3 노랑, -3~0 빨강
    struct Seg { float lo, hi; ImU32 col; };
    const Seg segs[3] = {{0.00f, 0.80f, IM_COL32(70, 200, 90, 255)},
                         {0.80f, 0.95f, IM_COL32(235, 200, 70, 255)},
                         {0.95f, 1.00f, IM_COL32(240, 80, 70, 255)}};
    for (const Seg& s : segs) {
        const float hi = std::min(m.disp, s.hi);
        if (hi <= s.lo) break;
        dl->AddRectFilled(ImVec2(p0.x, p1.y - hi * h), ImVec2(p1.x, p1.y - s.lo * h), s.col);
    }
    // -12dB/-3dB 경계 눈금 (색 구간과 같은 위치라 읽기 기준이 된다)
    for (float g : {0.80f, 0.95f})
        dl->AddLine(ImVec2(p0.x, p1.y - g * h), ImVec2(p1.x, p1.y - g * h),
                    IM_COL32(0, 0, 0, 90), 1.0f);
    if (m.hold > 0.001f) {
        const float y = p1.y - m.hold * h;
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 210), 1.5f);
    }
}

// 가로 미니 레벨 미터 (트랙 목록용): 세로 공간이 좁은 행에 맞춘 납작한 바.
// 채움 폭 = 레벨(dB 스케일), 흰 세로선 = 피크 홀드, 빨간 테두리 = 클리핑
// 래치(클릭으로 해제). 스무딩 상태(MeterView)는 버스별로 공유한다.
void miniMeterH(const char* id, AppState::MeterView& m, float raw, float w, float h) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    if (ImGui::IsItemClicked()) m.clip = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dt = ImGui::GetIO().DeltaTime;
    const double now = ImGui::GetTime();
    const float norm = peakToNorm(raw);
    m.disp = std::max(norm, m.disp - dt * (40.0f / 60.0f));
    if (norm >= m.hold || now - m.holdAt > 2.0) {
        m.hold = norm;
        m.holdAt = now;
    }
    if (raw >= 1.0f) m.clip = true;

    const ImVec2 p1(pos.x + w, pos.y + h);
    dl->AddRectFilled(pos, p1, IM_COL32(22, 22, 27, 255), 2.0f);
    struct Seg { float lo, hi; ImU32 col; };
    const Seg segs[3] = {{0.00f, 0.80f, IM_COL32(70, 200, 90, 255)},
                         {0.80f, 0.95f, IM_COL32(235, 200, 70, 255)},
                         {0.95f, 1.00f, IM_COL32(240, 80, 70, 255)}};
    for (const Seg& s : segs) {
        const float hi = std::min(m.disp, s.hi);
        if (hi <= s.lo) break;
        dl->AddRectFilled(ImVec2(pos.x + s.lo * w, pos.y + 1.0f),
                          ImVec2(pos.x + hi * w, p1.y - 1.0f), s.col);
    }
    if (m.hold > 0.001f) {
        const float hx = pos.x + m.hold * w;
        dl->AddLine(ImVec2(hx, pos.y), ImVec2(hx, p1.y), IM_COL32(255, 255, 255, 200), 1.0f);
    }
    if (m.clip) dl->AddRect(pos, p1, IM_COL32(255, 60, 50, 255), 2.0f, 0, 1.5f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", m.clip ? "클리핑! — 클릭하면 표시 해제" : "트랙 레벨");
}

// 레벨 미터 위젯: 위 클립 램프(빨강 래치, 클릭으로 해제) + 세로 바 bars개(L/R 또는 모노).
void levelMeterWidget(const char* id, AppState::MeterView* views, const float* raws, int bars,
                      float w, float h) {
    constexpr float kLampH = 7.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dt = ImGui::GetIO().DeltaTime;
    const double now = ImGui::GetTime();
    const float gap = bars > 1 ? 2.0f : 0.0f;
    const float bw = (w - gap * (float)(bars - 1)) / (float)bars;
    bool anyClip = false;
    for (int i = 0; i < bars; ++i) {
        if (clicked) views[i].clip = false;
        const ImVec2 p0(pos.x + (float)i * (bw + gap), pos.y + kLampH + 2.0f);
        const ImVec2 p1(p0.x + bw, pos.y + h);
        drawMeterBar(dl, p0, p1, views[i], raws[i], dt, now);
        dl->AddRectFilled(ImVec2(p0.x, pos.y), ImVec2(p1.x, pos.y + kLampH),
                          views[i].clip ? IM_COL32(255, 60, 50, 255)
                                        : IM_COL32(58, 42, 44, 255),
                          1.5f);
        anyClip |= views[i].clip;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", anyClip ? "클리핑! (0dB 초과) — 클릭하면 표시 해제"
                                        : "레벨 미터 (맨 위 램프 = 클리핑 표시)");
}



void drawMixer(AppState& state) {
    if (!state.showMixer) return;
    ImGui::SetNextWindowSize(ImVec2(640, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("믹서", &state.showMixer);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    // 마스터(왼쪽 고정) + 모든 트랙 스트립을 나란히. 많으면 가로 스크롤.
    constexpr float kColW = 112.0f;
    constexpr float kFaderH = 210.0f;
    constexpr float kFaderW = 28.0f;
    constexpr float kKnobR = 19.0f;
    // 연습 트랙은 '기타 연습' 창 전용 — 믹서에는 곡 트랙만 스트립을 낸다
    std::vector<int> mixIdx;
    for (int i = 0; i < (int)state.song.tracks.size(); ++i)
        if (!state.song.tracks[(std::size_t)i].practice) mixIdx.push_back(i);
    const int nTr = (int)mixIdx.size();

    if (ImGui::BeginTable("mixer_cols", 1 + std::max(1, nTr),
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollX,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, kColW);
        for (int c = 0; c < std::max(1, nTr); ++c)
            ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, kColW);
        ImGui::TableSetupScrollFreeze(1, 0); // 마스터 열은 스크롤해도 고정
        ImGui::TableNextRow();
        char pct[16];

        // 원시 피크는 applyTransportState가 프레임당 한 번 걷어둔 캐시를 읽는다
        float masterRaw[2] = {state.masterPeakCache[0], state.masterPeakCache[1]};
        constexpr float kMeterGap = 6.0f;
        constexpr float kMeterW2 = 16.0f; // 스테레오(L/R 2줄)
        constexpr float kMeterW1 = 9.0f;  // 모노(버스 합산 피크)

        // ── 왼쪽: 마스터 (페이더 + 미터 + % + Pan/Gain 노브) ──
        ImGui::TableSetColumnIndex(0);
        centeredText("마스터");
        ImGui::Spacing();
        centerNextItem(kFaderW + kMeterGap + kMeterW2);
        ImGui::BeginGroup();
        float mv = state.song.masterVolume;
        const bool mvChg = ImGui::VSliderFloat("##master", ImVec2(kFaderW, kFaderH), &mv,
                                               0.0f, 1.5f, "");
        if (ImGui::IsItemActivated()) state.snapshot(); // 조작 시작 시 1회 (값 쓰기 전)
        if (mvChg) state.song.masterVolume = mv;
        ImGui::SameLine(0.0f, kMeterGap);
        levelMeterWidget("##mmeter", state.meterMaster, masterRaw, 2, kMeterW2, kFaderH);
        ImGui::EndGroup();
        std::snprintf(pct, sizeof(pct), "%.0f%%", state.song.masterVolume * 100);
        centeredText(pct);
        ImGui::Spacing();
        float prevKnob = state.song.masterPan;
        rotaryKnob("##mpan", "Pan", &state.song.masterPan, -1.0f, 1.0f, 0.0f, kKnobR);
        snapshotKnobEdit(state, state.song.masterPan, prevKnob);
        ImGui::Spacing();
        prevKnob = state.song.masterGain;
        rotaryKnob("##mgain", "Gain", &state.song.masterGain, 0.0f, 2.0f, 1.0f, kKnobR);
        snapshotKnobEdit(state, state.song.masterGain, prevKnob);
        ImGui::Separator();
        drawMasterLimiterControls(state);
        ImGui::Separator();
        drawReturnReverbControls(state);

        // ── 트랙 스트립들 (이름 클릭 = 트랙 선택, 선택된 스트립은 하이라이트) ──
        for (int col = 0; col < nTr; ++col) {
            const int ti2 = mixIdx[(std::size_t)col];
            ImGui::TableSetColumnIndex(1 + col);
            ImGui::PushID(ti2);
            auto& t = state.song.tracks[(std::size_t)ti2];
            const float trackRaw = state.busPeakCache[t.channel & 0x0F];
            // 스트립이 좁아서 배지는 이름 윗줄에. 없는 트랙도 빈 줄을 그려
            // 옆 스트립들과 페이더 높이를 맞춘다.
            if (!trackTypeBadge(t, false)) ImGui::TextUnformatted(" ");
            if (ImGui::Selectable(t.name.c_str(), ti2 == state.selectedTrack))
                state.selectedTrack = ti2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.name.c_str());
            ImGui::Spacing();
            centerNextItem(kFaderW + kMeterGap + kMeterW1);
            ImGui::BeginGroup();
            float v = t.volume;
            const bool tvChg =
                ImGui::VSliderFloat("##trkvol", ImVec2(kFaderW, kFaderH), &v, 0.0f, 1.5f, "");
            if (ImGui::IsItemActivated()) state.snapshot();
            if (tvChg) t.volume = v;
            ImGui::SameLine(0.0f, kMeterGap);
            levelMeterWidget("##tmeter", &state.meterMix[t.channel & 0x0F], &trackRaw, 1,
                             kMeterW1, kFaderH);
            ImGui::EndGroup();
            std::snprintf(pct, sizeof(pct), "%.0f%%", t.volume * 100);
            centeredText(pct);
            ImGui::Spacing();
            float prevT = t.pan;
            rotaryKnob("##tpan", "Pan", &t.pan, -1.0f, 1.0f, 0.0f, kKnobR);
            snapshotKnobEdit(state, t.pan, prevT);
            ImGui::Spacing();
            prevT = t.gain;
            rotaryKnob("##tgain", "Gain", &t.gain, 0.0f, 2.0f, 1.0f, kKnobR);
            snapshotKnobEdit(state, t.gain, prevT);
            ImGui::Spacing();
            prevT = t.sendLevel; // 리턴 버스(공용 리버브)로 보내는 양
            rotaryKnob("##tsend", "Send", &t.sendLevel, 0.0f, 1.0f, 0.0f, kKnobR);
            snapshotKnobEdit(state, t.sendLevel, prevT);
            ImGui::Spacing();
            centerNextItem(52.0f);
            bool mm = t.muted;
            if (ImGui::Checkbox("뮤트##mx", &mm)) {
                state.snapshot(); // 토글 "전" 상태를 남긴다
                t.muted = mm;
            }

            // ── 기본 EQ (저/중/고) ──
            ImGui::Separator();
            drawTrackEqInline(state, t);

            // ── FX 체인 (엔진 처리 순서 위→아래, ▲▼로 순서 변경) ──
            if (state.vst) {
                ImGui::Separator();
                centeredText("FX 체인");
                ImGui::BeginChild("##fxbox", ImVec2(0.0f, 110.0f), ImGuiChildFlags_Borders);
                const int fxCh = t.channel & 0x0F;
                const int nfx = state.vst->trackEffectCount(fxCh);
                if (nfx == 0) {
                    centerNextItem(ImGui::CalcTextSize("(없음)").x);
                    ImGui::TextDisabled("(없음)");
                }
                // 저장 목록(t.plugins)엔 악기 항목이 섞여 있어 이펙트만 세어 찾는다
                const auto fxPlugIdx = [&t](int wantFx) {
                    int k = 0;
                    for (int idx = 0; idx < (int)t.plugins.size(); ++idx) {
                        if (t.plugins[(std::size_t)idx].isInstrument) continue;
                        if (k == wantFx) return idx;
                        ++k;
                    }
                    return -1;
                };
                // 인접 스왑: 엔진 체인 + 저장 목록을 같은 순서로 유지
                const auto swapFx = [&](int a, int b) {
                    const int pa = fxPlugIdx(a), pb = fxPlugIdx(b);
                    state.vst->moveTrackEffect(fxCh, a, b);
                    if (pa >= 0 && pb >= 0)
                        std::swap(t.plugins[(std::size_t)pa], t.plugins[(std::size_t)pb]);
                };
                for (int fi = 0; fi < nfx; ++fi) {
                    ImGui::PushID(300 + fi);
                    bool fon = state.vst->trackEffectEnabled(fxCh, fi);
                    if (ImGui::Checkbox("##mxfxon", &fon)) { // 실시간 바이패스
                        state.vst->setTrackEffectEnabled(fxCh, fi, fon);
                        const int pidx = fxPlugIdx(fi);
                        if (pidx >= 0) t.plugins[(std::size_t)pidx].enabled = fon;
                    }
                    ImGui::SameLine(0.0f, 3.0f);
                    ImGui::BeginDisabled(fi == 0);
                    if (ImGui::SmallButton("▲")) {
                        swapFx(fi, fi - 1);
                        ImGui::EndDisabled();
                        ImGui::PopID();
                        break; // 순서가 바뀌었으니 이번 프레임 목록은 여기까지
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine(0.0f, 3.0f);
                    ImGui::BeginDisabled(fi == nfx - 1);
                    if (ImGui::SmallButton("▼")) {
                        swapFx(fi, fi + 1);
                        ImGui::EndDisabled();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine(0.0f, 4.0f);
                    const std::string nm = state.vst->trackEffectName(fxCh, fi);
                    ImGui::TextColored(fon ? ImVec4(1.0f, 0.78f, 0.45f, 1.0f)
                                           : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                       "%s", nm.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n더블클릭: 편집기/파라미터 열기", nm.c_str());
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (auto* h = state.vst->trackEffectHost(fxCh, fi)) h->openEditor();
                        else if (state.vst->trackEffectBuiltin(fxCh, fi)) {
                            state.builtinFxCh = fxCh; // 내장 이펙트: 파라미터 창
                            state.builtinFxIdx = fi;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            ImGui::PopID();
        }
        if (nTr == 0) {
            ImGui::TableSetColumnIndex(1);
            centeredText("(트랙 없음)");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------
// 내장 이펙트 (EQ/딜레이/리버브)
// ---------------------------------------------------------

// 체인 인덱스 fxIndex의 저장 항목(TrackPlugin) 찾기. 채널을 공유하는 트랙이
// 있으면 프로젝트 로드가 트랙 순서대로 체인에 쌓으므로 같은 순서로 센다.
seq::TrackPlugin* trackFxPlugin(AppState& state, int channel, int fxIndex) {
    int count = 0;
    for (auto& t : state.song.tracks) {
        if ((t.channel & 0x0F) != channel) continue;
        for (auto& pl : t.plugins) {
            if (pl.isInstrument) continue;
            if (count == fxIndex) return &pl;
            ++count;
        }
    }
    return nullptr;
}

// 저장용 경로 문자열: "builtin:eq|0.0,0.0,0.0,1000.0,0" (파라미터까지 함께 저장)
std::string builtinFxPathString(const audio::BuiltinFx& fx) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "builtin:%s|%.4g,%.4g,%.4g,%.4g,%.4g",
                  audio::BuiltinFx::typeToken(fx.type()), fx.param(0), fx.param(1), fx.param(2),
                  fx.param(3), fx.param(4));
    return buf;
}

// 트랙 체인에서 첫 내장 EQ를 찾는다 (기본 채널 EQ로 취급).
static audio::BuiltinFx* findTrackEq(AppState& state, int ch, int& idxOut) {
    idxOut = -1;
    if (!state.vst) return nullptr;
    const int n = state.vst->trackEffectCount(ch);
    for (int i = 0; i < n; ++i)
        if (auto* b = state.vst->trackEffectBuiltin(ch, i))
            if (b->type() == audio::BuiltinFx::kEq) {
                idxOut = i;
                return b;
            }
    return nullptr;
}

// 트랙에 기본 EQ를 장착한다 (체인 + 저장 목록). 이미 있으면 아무것도 안 한다.
void addTrackEq(AppState& state, seq::Track& t) {
    if (!state.vst) return;
    const int ch = t.channel & 0x0F;
    int idx = -1;
    if (findTrackEq(state, ch, idx)) return;
    if (!state.vst->addBuiltinTrackEffect(ch, audio::BuiltinFx::kEq)) return;
    seq::TrackPlugin pl;
    pl.name = "EQ";
    if (auto* bf = state.vst->trackEffectBuiltin(ch, state.vst->trackEffectCount(ch) - 1))
        pl.path = builtinFxPathString(*bf);
    pl.classIndex = -1;
    pl.isInstrument = false;
    pl.enabled = true;
    t.plugins.push_back(std::move(pl));
}

// 트랙의 기본 EQ를 저/중/고 미니 페이더 3개로 그린다 (채널 창/믹서 스트립 공용).
// 체인에 EQ가 없으면 + EQ 버튼을 보여준다.
void drawTrackEqInline(AppState& state, seq::Track& t) {
    if (!state.vst) return;
    const int ch = t.channel & 0x0F;
    int eqIdx = -1;
    audio::BuiltinFx* eq = findTrackEq(state, ch, eqIdx);
    centeredText("EQ");
    if (!eq) {
        centerNextItem(56.0f);
        if (ImGui::SmallButton("+ EQ")) addTrackEq(state, t);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("기본 3밴드 EQ를 FX 체인에 추가");
        return;
    }
    // Pan/Gain 노브와 같은 원형 노브를 세로로 쌓는다 (드래그 ±12dB, 더블클릭 0)
    static const char* kBand[3] = {"Bass", "Middle", "Treble"};
    constexpr float kR = 19.0f;
    bool changed = false;
    for (int b = 0; b < 3; ++b) {
        char id[16];
        std::snprintf(id, sizeof(id), "##eqk%d", b);
        float v = eq->param(b);
        if (rotaryKnob(id, kBand[b], &v, -12.0f, 12.0f, 0.0f, kR)) {
            eq->setParam(b, v);
            changed = true;
        }
        ImGui::Spacing();
    }
    if (changed) // 저장 목록의 경로 문자열에도 새 파라미터를 새긴다
        if (auto* pl = trackFxPlugin(state, ch, eqIdx)) pl->path = builtinFxPathString(*eq);
}

// 리턴 리버브 (센드/리턴 버스) 컨트롤: 리턴 레벨 노브 + 공간/댐핑 설정 팝업.
// 각 트랙의 Send 노브가 이 공용 리버브로 신호를 보낸다.
void drawReturnReverbControls(AppState& state) {
    if (!state.audioClips) return;
    centeredText("리턴 리버브");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("트랙들의 Send 노브가 보내는 신호에 걸리는 공용 리버브.\n"
                          "트랙마다 리버브를 거는 것보다 CPU를 아끼고 공간감이 통일됩니다.");
    float lvl = state.audioClips->returnLevel();
    if (rotaryKnob("##retlvl", "Return", &lvl, 0.0f, 1.5f, 1.0f, 15.0f))
        state.audioClips->setReturnLevel(lvl);
    audio::BuiltinFx* rv = state.audioClips->returnReverb();
    if (!rv) return;
    centerNextItem(44.0f);
    if (ImGui::SmallButton("설정##ret")) ImGui::OpenPopup("retset");
    if (ImGui::BeginPopup("retset")) {
        static const char* kNames[2] = {"공간 크기", "댐핑"};
        for (int i = 0; i < 2; ++i) { // 믹스(2)는 웻 전용으로 고정이라 감춘다
            float v = rv->param(i);
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderFloat(kNames[i], &v, 0.0f, 1.0f, "%.2f")) rv->setParam(i, v);
        }
        ImGui::EndPopup();
    }
}

// 마스터 리미터 토글 + 게인 감소(GR) 표시 + 설정 팝업 (믹서/채널 창 마스터 공용).
// 볼륨/팬 뒤 최종 단계에서 피크를 눌러 클리핑을 막는다 (내보내기에도 적용).
void drawMasterLimiterControls(AppState& state) {
    if (!state.audioClips) return;
    bool on = state.audioClips->masterLimiterOn();
    if (ImGui::Checkbox("리미터", &on)) state.audioClips->setMasterLimiter(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("마스터 리미터: 0dB를 넘는 피크를 눌러\n"
                          "클리핑(찌그러짐)을 막습니다. 내보내기에도 적용됩니다.");
    audio::BuiltinFx* lim = state.audioClips->masterLimiter();
    if (!lim) return;
    if (on) { // 지금 얼마나 누르고 있는지 (Gain Reduction)
        const float gr = lim->gainReductionDb();
        if (gr > 0.05f)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "GR -%.1f dB", gr);
        else
            ImGui::TextDisabled("GR 0.0 dB");
        ImGui::SameLine();
        if (ImGui::SmallButton("설정##lim")) ImGui::OpenPopup("limset");
    }
    if (ImGui::BeginPopup("limset")) {
        const audio::BuiltinFx::ParamDesc* pd =
            audio::BuiltinFx::paramDescs(audio::BuiltinFx::kLimiter);
        for (int i = 0; i < audio::BuiltinFx::kNumParams; ++i) {
            if (!pd[i].label) continue;
            float v = lim->param(i);
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderFloat(pd[i].label, &v, pd[i].min, pd[i].max, pd[i].fmt))
                lim->setParam(i, v);
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                lim->setParam(i, pd[i].def); // 더블클릭 = 기본값
        }
        ImGui::EndPopup();
    }
}

// 내장 이펙트 파라미터 창. 슬라이더는 엔진의 atomic 파라미터를 바로 쓰고,
// 바뀔 때마다 저장 항목(plugins 경로)에도 새겨 프로젝트 저장에 반영된다.
void drawBuiltinFx(AppState& state) {
    if (state.builtinFxCh < 0 || !state.vst) return;
    audio::BuiltinFx* fx = state.vst->trackEffectBuiltin(state.builtinFxCh, state.builtinFxIdx);
    if (!fx) { // 삭제/순서 변경으로 대상이 사라짐
        state.builtinFxCh = state.builtinFxIdx = -1;
        return;
    }
    bool open = true;
    char title[96];
    std::snprintf(title, sizeof(title), "%s — 트랙 %d 이펙트###builtinfx",
                  audio::BuiltinFx::typeName(fx->type()), state.builtinFxCh + 1);
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin(title, &open);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)
    const audio::BuiltinFx::ParamDesc* pd = audio::BuiltinFx::paramDescs(fx->type());
    bool changed = false;
    for (int i = 0; i < audio::BuiltinFx::kNumParams; ++i) {
        if (!pd[i].label) continue;
        float v = fx->param(i);
        if (ImGui::SliderFloat(pd[i].label, &v, pd[i].min, pd[i].max, pd[i].fmt,
                               pd[i].log ? ImGuiSliderFlags_Logarithmic : 0)) {
            fx->setParam(i, v);
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            fx->setParam(i, pd[i].def); // 더블클릭 = 기본값
            changed = true;
        }
    }
    if (changed) {
        if (auto* pl = trackFxPlugin(state, state.builtinFxCh, state.builtinFxIdx))
            pl->path = builtinFxPathString(*fx);
    }
    // 컴프레서: 사이드체인 키 트랙 선택 (킥으로 베이스 덕킹 등)
    if (fx->type() == audio::BuiltinFx::kCompressor && state.vst) {
        ImGui::Separator();
        const int cur =
            state.vst->trackEffectSidechain(state.builtinFxCh, state.builtinFxIdx);
        std::string prev = "없음 (자기 입력)";
        if (cur >= 0) {
            prev = "버스 " + std::to_string(cur + 1);
            for (const auto& t : state.song.tracks)
                if ((t.channel & 0x0F) == cur) {
                    prev = t.name;
                    break;
                }
        }
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("사이드체인", prev.c_str())) {
            const auto pick = [&](int bus) {
                state.vst->setTrackEffectSidechain(state.builtinFxCh, state.builtinFxIdx,
                                                   bus);
                if (auto* pl = trackFxPlugin(state, state.builtinFxCh, state.builtinFxIdx))
                    pl->classIndex = bus; // 내장 이펙트는 classIndex를 키 버스로 재활용
            };
            if (ImGui::Selectable("없음 (자기 입력)", cur < 0)) pick(-1);
            for (int ti = 0; ti < (int)state.song.tracks.size(); ++ti) {
                const auto& t = state.song.tracks[(std::size_t)ti];
                const int bus = t.channel & 0x0F;
                if (bus == state.builtinFxCh) continue; // 자기 버스는 의미 없다
                ImGui::PushID(ti);
                if (ImGui::Selectable(t.name.c_str(), cur == bus)) pick(bus);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("선택한 트랙의 소리로 이 트랙을 눌러줍니다.\n"
                              "예: 베이스 트랙 컴프레서에 킥 트랙을 걸면\n"
                              "킥이 칠 때마다 베이스가 숙여집니다 (덕킹).");
    }
    ImGui::End();
    if (!open) state.builtinFxCh = state.builtinFxIdx = -1;
}

// ---------------------------------------------------------
// 채널 창 (컴팩트): 마스터 + "선택된 트랙"만. 전체 스트립 믹서와 별개로
// 왼쪽에 상시 붙여두고 쓰는 이전 방식의 창이다.
// ---------------------------------------------------------
void drawMixerCompact(AppState& state) {
    if (!state.showMixerCompact) return;
    // 세션마다 처음엔 믹서 창과 같은 도크(탭)에 붙인다. 이후 옮기면 그대로.
    if (ImGuiWindow* mx = ImGui::FindWindowByName("믹서"))
        if (mx->DockId != 0) ImGui::SetNextWindowDockID(mx->DockId, ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2(10.0f, 60.0f), ImGuiCond_FirstUseEver); // 왼쪽에 배치
    ImGui::SetNextWindowSize(ImVec2(270, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("채널 (마스터/선택 트랙)", &state.showMixerCompact);
    drawPendingWindowBackground(); // 창별 배경 이미지 (예약이 있으면)

    constexpr float kColW = 112.0f;
    constexpr float kFaderH = 230.0f;
    constexpr float kFaderW = 28.0f;
    constexpr float kKnobR = 19.0f;
    constexpr float kMeterGap = 6.0f;
    constexpr float kMeterW2 = 16.0f;
    constexpr float kMeterW1 = 9.0f;

    if (ImGui::BeginTable("mixerc_cols", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(kColW * 2.0f + 24.0f, 0))) {
        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, kColW);
        ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, kColW);
        ImGui::TableNextRow();
        char pct[16];
        float masterRaw[2] = {state.masterPeakCache[0], state.masterPeakCache[1]};

        // ── 마스터 ──
        ImGui::TableSetColumnIndex(0);
        centeredText("마스터");
        ImGui::Spacing();
        centerNextItem(kFaderW + kMeterGap + kMeterW2);
        ImGui::BeginGroup();
        float mv = state.song.masterVolume;
        const bool mvChg =
            ImGui::VSliderFloat("##cmaster", ImVec2(kFaderW, kFaderH), &mv, 0.0f, 1.5f, "");
        if (ImGui::IsItemActivated()) state.snapshot();
        if (mvChg) state.song.masterVolume = mv;
        ImGui::SameLine(0.0f, kMeterGap);
        levelMeterWidget("##cmmeter", state.meterMasterC, masterRaw, 2, kMeterW2, kFaderH);
        ImGui::EndGroup();
        std::snprintf(pct, sizeof(pct), "%.0f%%", state.song.masterVolume * 100);
        centeredText(pct);
        ImGui::Spacing();
        float prevKnob = state.song.masterPan;
        rotaryKnob("##cmpan", "Pan", &state.song.masterPan, -1.0f, 1.0f, 0.0f, kKnobR);
        snapshotKnobEdit(state, state.song.masterPan, prevKnob);
        ImGui::Spacing();
        prevKnob = state.song.masterGain;
        rotaryKnob("##cmgain", "Gain", &state.song.masterGain, 0.0f, 2.0f, 1.0f, kKnobR);
        snapshotKnobEdit(state, state.song.masterGain, prevKnob);
        ImGui::Separator();
        drawMasterLimiterControls(state);

        // ── 선택 트랙 ──
        ImGui::TableSetColumnIndex(1);
        if (state.selectedTrack < (int)state.song.tracks.size()) {
            auto& t = state.song.tracks[(std::size_t)state.selectedTrack];
            const float trackRaw = state.busPeakCache[t.channel & 0x0F];
            if (ImGui::CalcTextSize(t.name.c_str()).x <= kColW - 8.0f)
                centeredText(t.name.c_str());
            else
                ImGui::TextUnformatted(t.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.name.c_str());
            ImGui::Spacing();
            centerNextItem(kFaderW + kMeterGap + kMeterW1);
            ImGui::BeginGroup();
            float v = t.volume;
            const bool tvChg =
                ImGui::VSliderFloat("##ctrkvol", ImVec2(kFaderW, kFaderH), &v, 0.0f, 1.5f, "");
            if (ImGui::IsItemActivated()) state.snapshot();
            if (tvChg) t.volume = v;
            ImGui::SameLine(0.0f, kMeterGap);
            float trackRawCopy = trackRaw;
            levelMeterWidget("##ctmeter", &state.meterTrackC, &trackRawCopy, 1, kMeterW1,
                             kFaderH);
            ImGui::EndGroup();
            std::snprintf(pct, sizeof(pct), "%.0f%%", t.volume * 100);
            centeredText(pct);
            ImGui::Spacing();
            float prevT = t.pan;
            rotaryKnob("##ctpan", "Pan", &t.pan, -1.0f, 1.0f, 0.0f, kKnobR);
            snapshotKnobEdit(state, t.pan, prevT);
            ImGui::Spacing();
            prevT = t.gain;
            rotaryKnob("##ctgain", "Gain", &t.gain, 0.0f, 2.0f, 1.0f, kKnobR);
            snapshotKnobEdit(state, t.gain, prevT);
            ImGui::Spacing();
            prevT = t.sendLevel; // 리턴 버스(공용 리버브)로 보내는 양
            rotaryKnob("##ctsend", "Send", &t.sendLevel, 0.0f, 1.0f, 0.0f, kKnobR);
            snapshotKnobEdit(state, t.sendLevel, prevT);
            ImGui::Spacing();
            centerNextItem(52.0f);
            bool mm = t.muted;
            if (ImGui::Checkbox("뮤트##cmx", &mm)) {
                state.snapshot();
                t.muted = mm;
            }

            // ── 기본 EQ (Bass/Middle/Treble) ── 뮤트와 마커 사이를 경계선으로 구분
            ImGui::Separator();
            drawTrackEqInline(state, t);

            // ── 구간 마커 리스트 ──
            // 클릭하면 재생 위치를 그 마커로 옮기고, 트랙 뷰/피아노 롤이
            // 따라 스크롤한다 (seekTo의 scrollToPlayhead).
            ImGui::Separator();
            ImGui::TextDisabled("마커");
            if (ImGui::BeginChild("##cmarkers", ImVec2(kColW - 4.0f, 150.0f),
                                  ImGuiChildFlags_Borders)) {
                if (state.song.markers.empty()) {
                    ImGui::TextDisabled("(없음)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("트랙 뷰 우클릭 →\n\"여기에 구간 마커 추가\"");
                } else {
                    // 틱 순으로 정렬해 보여준다 (드래그로 순서가 섞였을 수 있다)
                    std::vector<int> order((std::size_t)state.song.markers.size());
                    std::iota(order.begin(), order.end(), 0);
                    std::sort(order.begin(), order.end(), [&](int a, int b) {
                        return state.song.markers[(std::size_t)a].tick <
                               state.song.markers[(std::size_t)b].tick;
                    });
                    const uint32_t tpb = songTicksPerBar(state);
                    for (int mi : order) {
                        const auto& mk = state.song.markers[(std::size_t)mi];
                        char lbl[96];
                        std::snprintf(lbl, sizeof(lbl), "%u %s##mk%d", mk.tick / tpb + 1,
                                      mk.name.c_str(), mi);
                        const bool cur = (mi == state.selectedMarker);
                        if (ImGui::Selectable(lbl, cur)) {
                            state.selectedMarker = mi;
                            seekTo(state, mk.tick); // 뷰가 따라 이동
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("마디 %u — 클릭하면 이 위치로 이동",
                                              mk.tick / tpb + 1);
                    }
                }
            }
            ImGui::EndChild();
        } else {
            centeredText("(트랙 없음)");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
} // namespace midipro::gui