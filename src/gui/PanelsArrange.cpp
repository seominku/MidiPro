// =============================================================
// MidiPro - gui/PanelsArrange.cpp
// 어레인지 뷰: 구간 마커(Intro/Verse/...)를 블록으로 늘어놓고
// 드래그로 순서를 바꾸면 곡 내용(모든 트랙의 노트/CC/클립/오토메이션/
// 템포/마커)이 통째로 재배열된다. 블록에서 점프/복제/삭제도 된다.
//
// 재배열 방식: "새 순서대로 전체 재조립". 각 구간의 내용을 원본에서
// 읽어 새 오프셋에 다시 놓는다 — 자리 이동/삭제가 한 함수로 끝나고
// 경계에 걸친 노트도 스팬 단위로 안전하다.
// =============================================================

#include "gui/Panels.h"
#include "gui/PanelsInternal.h"

#include "midi/MidiConstants.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace midipro::gui {

namespace {
struct Section {
    uint32_t a = 0, b = 0; // [a, b)
    std::string name;
};

// 현재 마커들로 구간 목록을 만든다 (마지막 구간은 내용 끝을 마디로 올림)
std::vector<Section> buildSections(AppState& state) {
    std::vector<Section> out;
    const auto& mks = state.song.markers;
    if (mks.empty()) return out;
    const uint32_t tpb = songTicksPerBar(state);
    uint32_t endAll = contentTicksWithAudio(state);
    const uint32_t lastMk = mks.back().tick;
    if (endAll <= lastMk) endAll = lastMk + tpb * 4;
    endAll = (endAll + tpb - 1) / tpb * tpb; // 마디로 올림
    for (std::size_t k = 0; k < mks.size(); ++k) {
        Section s;
        s.a = mks[k].tick;
        s.b = k + 1 < mks.size() ? mks[k + 1].tick : endAll;
        s.name = mks[k].name;
        if (s.b > s.a) out.push_back(std::move(s));
    }
    return out;
}

// 구간들을 newOrder 순서로 재조립한다 (빠진 인덱스 = 그 구간 삭제).
// 첫 마커 이전(프렐류드)은 그대로 맨 앞에 남는다.
void applyArrangement(AppState& state, const std::vector<Section>& secs,
                      const std::vector<int>& newOrder) {
    if (secs.empty()) return;
    stopTransport(state);
    state.snapshot();
    seq::Song& s = state.song;
    const uint32_t prelude = secs.front().a;

    // 새 순서에서 각 구간의 목적지 오프셋
    std::vector<std::pair<Section, uint32_t>> plan; // (구간, 새 시작)
    uint32_t off = prelude;
    for (int idx : newOrder) {
        if (idx < 0 || idx >= (int)secs.size()) continue;
        plan.push_back({secs[(std::size_t)idx], off});
        off += secs[(std::size_t)idx].b - secs[(std::size_t)idx].a;
    }

    for (auto& t : s.tracks) {
        std::vector<seq::MidiEvent> nev;
        std::vector<std::shared_ptr<audio::AudioClip>> nclips;
        std::vector<seq::Track::AutoPoint> nvol, npan;
        std::vector<seq::MidiClip> nmc;
        const auto notes = seq::extractNotes(t);
        const uint8_t onSt = (uint8_t)(midi::kStatusNoteOn | (t.channel & 0x0F));
        const uint8_t offSt = (uint8_t)(midi::kStatusNoteOff | (t.channel & 0x0F));

        // [a,b)의 내용을 dest로 복사 (노트는 스팬 단위, 클립은 포인터 이동)
        const auto emit = [&](uint32_t a, uint32_t b, uint32_t dest) {
            const long d = (long)dest - (long)a;
            for (const auto& n : notes)
                if (n.startTick >= a && n.startTick < b) {
                    const uint32_t ns = (uint32_t)((long)n.startTick + d);
                    nev.push_back({ns, onSt, n.note, n.velocity});
                    nev.push_back({(uint32_t)((long)n.endTick + d), offSt, n.note, 0});
                }
            for (const auto& e : t.events)
                if (!e.isNoteOn() && !e.isNoteOff() && e.tick >= a && e.tick < b) {
                    seq::MidiEvent c = e;
                    c.tick = (uint32_t)((long)c.tick + d);
                    nev.push_back(c);
                }
            for (auto& cp : t.clips)
                if (cp && cp->startTick >= a && cp->startTick < b) {
                    cp->startTick = (uint32_t)((long)cp->startTick + d);
                    nclips.push_back(cp);
                }
            for (const auto& p : t.volAuto)
                if (p.tick >= a && p.tick < b)
                    nvol.push_back({(uint32_t)((long)p.tick + d), p.value});
            for (const auto& p : t.panAuto)
                if (p.tick >= a && p.tick < b)
                    npan.push_back({(uint32_t)((long)p.tick + d), p.value});
            for (const auto& mc : t.midiClips)
                if (mc.startTick >= a && mc.startTick < b) {
                    seq::MidiClip c = mc;
                    c.startTick = (uint32_t)((long)c.startTick + d);
                    c.endTick = (uint32_t)((long)c.endTick + d);
                    for (auto& m : c.members) m.second = (uint32_t)((long)m.second + d);
                    nmc.push_back(std::move(c));
                }
        };
        emit(0, prelude, 0); // 프렐류드는 제자리
        for (const auto& pl : plan) emit(pl.first.a, pl.first.b, pl.second);

        t.events = std::move(nev);
        t.sortEvents();
        t.clips = std::move(nclips);
        t.volAuto = std::move(nvol);
        t.panAuto = std::move(npan);
        const auto byTick = [](const seq::Track::AutoPoint& x,
                               const seq::Track::AutoPoint& y) { return x.tick < y.tick; };
        std::sort(t.volAuto.begin(), t.volAuto.end(), byTick);
        std::sort(t.panAuto.begin(), t.panAuto.end(), byTick);
        std::stable_sort(nmc.begin(), nmc.end(),
                         [](const seq::MidiClip& x, const seq::MidiClip& y) {
                             return x.startTick < y.startTick;
                         });
        t.midiClips = std::move(nmc);
    }

    // 템포 지점: 프렐류드 것 + 각 구간 것 (같은 방식으로 재배치)
    {
        std::vector<seq::TempoChange> ntc;
        for (const auto& tc : s.tempoChanges)
            if (tc.tick < prelude) ntc.push_back(tc);
        for (const auto& pl : plan) {
            const long d = (long)pl.second - (long)pl.first.a;
            for (const auto& tc : s.tempoChanges)
                if (tc.tick >= pl.first.a && tc.tick < pl.first.b) {
                    seq::TempoChange c = tc;
                    c.tick = (uint32_t)((long)c.tick + d);
                    ntc.push_back(c);
                }
        }
        std::stable_sort(ntc.begin(), ntc.end(),
                         [](const seq::TempoChange& x, const seq::TempoChange& y) {
                             return x.tick < y.tick;
                         });
        s.tempoChanges = std::move(ntc);
    }
    // 마커: 각 구간(안의 부가 마커 포함)을 새 위치로
    {
        std::vector<seq::SectionMarker> nmk;
        for (const auto& pl : plan) {
            const long d = (long)pl.second - (long)pl.first.a;
            for (const auto& mk : s.markers)
                if (mk.tick >= pl.first.a && mk.tick < pl.first.b)
                    nmk.push_back({(uint32_t)((long)mk.tick + d), mk.name});
        }
        std::stable_sort(nmk.begin(), nmk.end(),
                         [](const seq::SectionMarker& x, const seq::SectionMarker& y) {
                             return x.tick < y.tick;
                         });
        s.markers = std::move(nmk);
    }

    // 인덱스 기반 선택은 전부 무효
    state.selectedNotes.clear();
    state.selClips.clear();
    state.selClipTrack = state.selClipIndex = -1;
    state.selMidiClipTrack = state.selMidiClipIndex = -1;
    state.clipRange = AppState::ClipRangeSel{};
    state.selectedMarker = state.selectedTempoMarker = -1;
    rebuildAudioMix(state);
    seekTo(state, 0);
}
} // namespace

void drawArrange(AppState& state) {
    if (!state.showArrange) return;
    ImGui::SetNextWindowSize(ImVec2(680, 180), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("어레인지", &state.showArrange)) {
        ImGui::End();
        return;
    }
    const auto secs = buildSections(state);
    if (secs.empty()) {
        ImGui::TextDisabled("구간 마커가 없습니다.");
        ImGui::TextDisabled("트랙 뷰 우클릭 → \"여기에 구간 마커 추가\"로 구간을 나눠 주세요.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("블록 드래그 = 순서 바꾸기 · 클릭 = 그 위치로 이동 · "
                        "우클릭 = 복제/삭제");
    const uint32_t tpb = songTicksPerBar(state);

    // 구간 블록들 (가로 나열, 드래그&드롭으로 재배열)
    for (int k = 0; k < (int)secs.size(); ++k) {
        const auto& sc = secs[(std::size_t)k];
        const int bars = (int)((sc.b - sc.a + tpb - 1) / tpb);
        char label[96];
        std::snprintf(label, sizeof(label), "%s\n%d마디###arr%d", sc.name.c_str(), bars, k);
        if (k > 0) ImGui::SameLine();
        ImGui::PushID(k);
        // 구간 이름으로 색을 정한다 — 같은 이름(복제된 Verse 등)은 같은 색이라
        // 순서를 바꿔도 구조가 한눈에 보인다.
        const float hue =
            (float)(std::hash<std::string>{}(sc.name) % 360u) / 360.0f;
        float cr, cg, cb;
        ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.45f, cr, cg, cb);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(cr, cg, cb, 1.0f));
        ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.58f, cr, cg, cb);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(cr, cg, cb, 1.0f));
        ImGui::ColorConvertHSVtoRGB(hue, 0.6f, 0.7f, cr, cg, cb);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(cr, cg, cb, 1.0f));
        // 재생 중인 구간은 테두리 강조
        const bool playingHere =
            state.playPosTick >= sc.a && state.playPosTick < sc.b;
        if (playingHere)
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.5f, 0.3f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, playingHere ? 2.0f : 1.0f);
        if (ImGui::Button(label, ImVec2(std::max(86.0f, 26.0f + 14.0f * (float)bars), 52.0f)))
            seekTo(state, sc.a); // 클릭 = 점프
        ImGui::PopStyleVar();
        if (playingHere) ImGui::PopStyleColor();
        ImGui::PopStyleColor(3);

        // 드래그 소스/타깃: 놓으면 그 자리 "앞"으로 이동
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ARR_SEC", &k, sizeof(int));
            ImGui::Text("%s (%d마디)", sc.name.c_str(), bars);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("ARR_SEC")) {
                const int src = *(const int*)pay->Data;
                if (src != k && src >= 0 && src < (int)secs.size()) {
                    std::vector<int> order;
                    for (int i2 = 0; i2 < (int)secs.size(); ++i2)
                        if (i2 != src) order.push_back(i2);
                    // src를 k 위치(현재 목록에서 k가 있는 곳) 앞에 끼운다
                    int insertAt = 0;
                    for (int i2 = 0; i2 < (int)order.size(); ++i2)
                        if (order[(std::size_t)i2] == k) insertAt = i2 + (src < k ? 1 : 0);
                    insertAt = std::clamp(insertAt, 0, (int)order.size());
                    order.insert(order.begin() + insertAt, src);
                    applyArrangement(state, secs, order);
                    ImGui::EndDragDropTarget();
                    ImGui::PopID();
                    break; // 구간 목록이 바뀌었으니 이번 프레임은 종료
                }
            }
            ImGui::EndDragDropTarget();
        }
        // 우클릭: 복제/삭제
        if (ImGui::BeginPopupContextItem("arrctx")) {
            ImGui::TextDisabled("%s (%d마디)", sc.name.c_str(), bars);
            ImGui::Separator();
            if (ImGui::MenuItem("복제 (바로 뒤에)")) {
                duplicateSection(state, sc.a, sc.b);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            if (ImGui::MenuItem("구간 삭제 (내용째)")) {
                std::vector<int> order;
                for (int i2 = 0; i2 < (int)secs.size(); ++i2)
                    if (i2 != k) order.push_back(i2);
                applyArrangement(state, secs, order);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace midipro::gui
