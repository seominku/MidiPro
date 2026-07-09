// =============================================================
// MidiPro - gui/Panels.cpp
// =============================================================

#include "gui/Panels.h"

#include "audio/SynthPreset.h"
#include "guitar/Fretboard.h"
#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"
#include "midi2/Ump.h"
#include "sequencer/TimeBase.h"
#include "sequencer/Track.h"
#include "vst/PluginScanner.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace midipro::gui {

namespace {

// 노트 번호 -> "C4" 문자열 (짧은 래퍼)
std::string noteName(uint8_t note) {
    return midi::MidiMessage::noteName(note);
}

// 재생 중이면 곡의 현재 틱, 아니면 0
uint32_t playheadTick(const AppState& state) {
    return state.player ? state.player->currentTick() : 0;
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
                open.active = false;
            }
        }
        track.sortEvents();
    }
    state.recording = false;
}

// 정지: 녹음 중이면 먼저 마무리한 뒤 트랜스포트를 멈춘다.
void stopTransport(AppState& state) {
    finalizeRecording(state);
    if (state.player) state.player->stop();
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
    if (state.history.undo(state.song)) {
        clampSelection(state);
        state.statusMessage = "실행취소";
    }
}

void doRedo(AppState& state) {
    if (!state.history.canRedo()) return;
    stopTransport(state);
    if (state.history.redo(state.song)) {
        clampSelection(state);
        state.statusMessage = "다시실행";
    }
}

// 재생 시작 (재생 버튼과 스페이스바가 공유)
void startPlayback(AppState& state) {
    if (!state.output || !state.player) return;
    if (state.song.tracks.empty()) {
        state.statusMessage = "재생할 트랙이 없습니다";
        return;
    }
    if (!state.output->isOpen()) {
        const auto ports = state.output->listPorts();
        if (!ports.empty()) state.output->openPort((unsigned)state.selectedOutputPort);
    }
    const uint32_t tpb = (uint32_t)state.song.ppqn * seq::kBeatsPerBar;
    const uint32_t start = state.loopEnabled ? (uint32_t)(state.loopStartBar - 1) * tpb : 0;
    state.player->play(state.song, start);
    state.statusMessage = "재생 중";
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

} // namespace

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
                    track.sortEvents();
                    open.active = false;
                }
            }
        }
    }
}

// ---------------------------------------------------------
// 루프/메트로놈을 매 프레임 플레이어에 반영 (트랜스포트 창을 닫아도 동작)
// ---------------------------------------------------------
void applyTransportState(AppState& state) {
    if (!state.player) return;
    const uint32_t tpb = (uint32_t)state.song.ppqn * seq::kBeatsPerBar;
    state.player->setLoop(state.loopEnabled, (uint32_t)(state.loopStartBar - 1) * tpb,
                          (uint32_t)state.loopEndBar * tpb);
    state.player->setMetronome(state.metronome || (state.recording && state.countIn));
}

// ---------------------------------------------------------
// 메뉴 바
// ---------------------------------------------------------
void drawMenuBar(AppState& state, bool& openRequested, bool& saveRequested) {
    // 키보드 단축키 (텍스트 입력 중이 아닐 때만): Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            io.KeyShift ? doRedo(state) : doUndo(state);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            doRedo(state);
        }
    }
    // 스페이스바: 재생/정지 토글. 텍스트 입력이나 위젯 조작 중에는 무시
    // (버튼 활성화용 스페이스와 충돌 방지).
    if (!io.WantTextInput && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        togglePlayback(state);
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
            if (ImGui::MenuItem("열기 (.mid)")) openRequested = true;
            if (ImGui::MenuItem("저장 (.mid)")) saveRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem("프로젝트 열기 (.midipro)")) state.projectLoadRequested = true;
            if (ImGui::MenuItem("프로젝트 저장 (.midipro)")) state.projectSaveRequested = true;
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
            ImGui::MenuItem("신디사이저", nullptr, &state.showSynth); // 팝업 창 토글
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tool")) {
            // 각 창을 체크박스로 켜고 끈다 (별도 팝업 창으로 뜬다)
            ImGui::MenuItem("트랜스포트", nullptr, &state.showTransport);
            ImGui::MenuItem("MIDI 장치", nullptr, &state.showDevices);
            ImGui::MenuItem("트랙", nullptr, &state.showTracks);
            ImGui::MenuItem("피아노 롤", nullptr, &state.showPianoRoll);
            ImGui::MenuItem("신디사이저", nullptr, &state.showSynth);
            ImGui::MenuItem("VST3 플러그인", nullptr, &state.showVst);
            ImGui::MenuItem("기타 도우미", nullptr, &state.showGuitar);
            ImGui::MenuItem("입력 모니터", nullptr, &state.showMonitor);
            ImGui::MenuItem("상태", nullptr, &state.showStatus);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("MidiPro - Phase 6 (Undo/Redo)");
        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------
// 트랜스포트
// ---------------------------------------------------------
void drawTransport(AppState& state) {
    if (!state.showTransport) return;
    ImGui::Begin("트랜스포트", &state.showTransport);

    const bool playing = state.player && state.player->isPlaying();

    // 기능을 4개 세트로 나눠 세로로 쌓고, 세로 구분선으로 가로 배치한다.
    if (ImGui::BeginTable("transport_sets", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();

        // ── 세트 1: 재생/녹음 ──
        ImGui::TableSetColumnIndex(0);
        ImGui::SeparatorText("재생");
        if (playing) {
            if (ImGui::Button("■ 정지", ImVec2(110, 0))) {
                stopTransport(state);
                state.statusMessage = state.recording ? "녹음 정지" : "정지";
            }
        } else {
            if (ImGui::Button("▶ 재생", ImVec2(110, 0))) startPlayback(state);
        }
        if (state.recording) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
            if (ImGui::Button("● 녹음중", ImVec2(110, 0))) {
                stopTransport(state);
                state.statusMessage = "녹음 정지";
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("● 녹음", ImVec2(110, 0))) {
                if (state.song.tracks.empty()) { // 기록할 트랙이 없으면 하나 만든다
                    seq::Track t;
                    t.name = "Track 1";
                    state.song.tracks.push_back(t);
                    state.selectedTrack = 0;
                }
                if (state.input == nullptr || !state.input->isOpen()) {
                    state.statusMessage = "먼저 MIDI 입력 포트를 여세요 (MIDI 장치 패널)";
                } else {
                    if (state.output && !state.output->isOpen())
                        state.output->openPort((unsigned)state.selectedOutputPort);
                    state.snapshot(); // 녹음 시작 전 상태를 남겨 한 번에 되돌리기
                    for (auto& o : state.openRecNotes) o.active = false;
                    state.recording = true;
                    // 카운트인이면 한 마디 프리롤 후 곡 시작(틱0)부터 기록
                    const uint32_t tpb = (uint32_t)state.song.ppqn * seq::kBeatsPerBar;
                    const uint32_t preRoll = state.countIn ? tpb : 0;
                    state.player->play(state.song, 0, /*keepAlive=*/true, preRoll);
                    state.statusMessage = state.countIn ? "카운트인 후 녹음" : "녹음 중 (연주하세요)";
                }
            }
        }
        if (ImGui::Button("⟲ 처음으로", ImVec2(110, 0))) stopTransport(state);

        // ── 세트 2: 템포·위치 ──
        ImGui::TableSetColumnIndex(1);
        ImGui::SeparatorText("템포·위치");
        float bpm = (float)state.song.bpm;
        ImGui::SetNextItemWidth(130);
        if (ImGui::SliderFloat("BPM", &bpm, 40.0f, 240.0f, "%.0f")) state.song.bpm = bpm;
        const uint32_t tick = playheadTick(state);
        const seq::BarBeatTick pos = seq::toBarBeatTick(tick, state.song.ppqn);
        ImGui::Text("위치  %d : %d : %03d", pos.bar, pos.beat, pos.tick);
        ImGui::Checkbox("재생 헤드 따라가기", &state.followPlayhead);

        // ── 세트 3: 루프 ──
        ImGui::TableSetColumnIndex(2);
        ImGui::SeparatorText("루프");
        ImGui::Checkbox("루프 켜기", &state.loopEnabled);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("시작 마디", &state.loopStartBar);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("끝 마디", &state.loopEndBar);
        if (state.loopStartBar < 1) state.loopStartBar = 1;
        if (state.loopEndBar < state.loopStartBar) state.loopEndBar = state.loopStartBar;

        // ── 세트 4: 옵션 ──
        ImGui::TableSetColumnIndex(3);
        ImGui::SeparatorText("옵션");
        ImGui::Checkbox("메트로놈", &state.metronome);
        ImGui::Checkbox("카운트인", &state.countIn);
        ImGui::Checkbox("소프트 스루", &state.softThru);

        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------
// 장치 선택
// ---------------------------------------------------------
void drawDevices(AppState& state) {
    if (!state.showDevices) return;
    ImGui::Begin("MIDI 장치", &state.showDevices);

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
    }

    ImGui::End();
}

// ---------------------------------------------------------
// 트랙 목록
// ---------------------------------------------------------
void drawTrackList(AppState& state) {
    if (!state.showTracks) return;
    ImGui::Begin("트랙", &state.showTracks);

    if (ImGui::Button("+ 트랙 추가")) {
        state.snapshot();
        seq::Track t;
        t.name = "Track " + std::to_string(state.song.tracks.size() + 1);
        t.channel = (uint8_t)(state.song.tracks.size() & 0x0F);
        state.song.tracks.push_back(t);
        state.selectedTrack = (int)state.song.tracks.size() - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("데모 채우기") && !state.song.tracks.empty()) {
        state.snapshot();
        // C 메이저 스케일 한 마디를 선택 트랙에 넣는다 (테스트용)
        auto& t = state.song.tracks[state.selectedTrack];
        t.events.clear();
        const uint8_t scale[8] = {60, 62, 64, 65, 67, 69, 71, 72};
        const uint32_t step = state.song.ppqn / 2; // 8분음표
        for (int i = 0; i < 8; ++i) t.addNote(i * step, step, scale[i], 100);
        t.sortEvents();
        state.statusMessage = "데모 스케일을 추가했습니다";
    }

    ImGui::Separator();

    for (int i = 0; i < (int)state.song.tracks.size(); ++i) {
        auto& track = state.song.tracks[i];
        ImGui::PushID(i);

        const bool selected = (i == state.selectedTrack);
        if (ImGui::Selectable(track.name.c_str(), selected, ImGuiSelectableFlags_AllowOverlap))
            state.selectedTrack = i;

        ImGui::SameLine(160);
        ImGui::Checkbox("뮤트", &track.muted);
        ImGui::SameLine();
        int ch = track.channel + 1;
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("ch", &ch)) {
            ch = std::clamp(ch, 1, 16);
            track.channel = (uint8_t)(ch - 1);
        }
        ImGui::PopID();
    }

    if (state.song.tracks.empty())
        ImGui::TextDisabled("트랙이 없습니다. '+ 트랙 추가'를 누르세요.");

    ImGui::End();
}

// ---------------------------------------------------------
// 피아노 롤
// ---------------------------------------------------------
void drawPianoRoll(AppState& state) {
    if (!state.showPianoRoll) return;
    ImGui::Begin("피아노 롤", &state.showPianoRoll);

    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("확대", &state.pianoRollZoom, 0.05f, 1.0f, "%.2f px/tick");
    ImGui::SameLine();
    ImGui::Checkbox("노트 편집", &state.editMode);
    if (state.editMode) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        const char* lens[] = {"4분음표", "8분음표", "16분음표"};
        int lenSel = (state.editNoteLenDiv == 1) ? 0 : (state.editNoteLenDiv == 2 ? 1 : 2);
        if (ImGui::Combo("길이", &lenSel, lens, IM_ARRAYSIZE(lens)))
            state.editNoteLenDiv = (lenSel == 0) ? 1 : (lenSel == 1 ? 2 : 4);
    }
    if (state.editMode)
        ImGui::TextDisabled("빈 칸 좌클릭: 추가 · 노트 드래그: 이동 · 오른쪽 끝 드래그: 길이 · 우클릭: 삭제");

    if (state.song.tracks.empty() || state.selectedTrack >= (int)state.song.tracks.size()) {
        ImGui::TextDisabled("표시할 트랙을 선택하세요.");
        ImGui::End();
        return;
    }

    seq::Track& track = state.song.tracks[state.selectedTrack];
    const auto notes = seq::extractNotes(track);

    // 그리기 영역
    ImGui::BeginChild("roll_canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    constexpr int kLowNote = 36;  // C2
    constexpr int kHighNote = 84; // C6
    constexpr float kRowHeight = 12.0f;
    const float zoom = state.pianoRollZoom;

    const int rows = kHighNote - kLowNote;
    const uint32_t songLen = std::max<uint32_t>(state.song.lengthTicks(),
                                                (uint32_t)(state.song.ppqn * 8));
    const float contentW = songLen * zoom + 40.0f;
    const float contentH = rows * kRowHeight;

    // 배경 행 (검은건반 줄 음영)
    for (int r = 0; r < rows; ++r) {
        const int note = kHighNote - r;
        const int pc = note % 12;
        const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        const float y = origin.y + r * kRowHeight;
        dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + contentW, y + kRowHeight),
                          black ? IM_COL32(40, 40, 46, 255) : IM_COL32(52, 52, 58, 255));
        // 옥타브 C 라벨
        if (pc == 0) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%s", noteName((uint8_t)note).c_str());
            dl->AddText(ImVec2(origin.x + 2, y), IM_COL32(150, 150, 160, 255), buf);
        }
    }

    // 마디 격자선
    const uint32_t ticksPerBar = (uint32_t)state.song.ppqn * seq::kBeatsPerBar;
    for (uint32_t t = 0; t <= songLen; t += ticksPerBar) {
        const float x = origin.x + t * zoom;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + contentH),
                    IM_COL32(80, 80, 90, 255));
    }

    // 루프 구간 음영 + 경계선
    if (state.loopEnabled) {
        const float lx0 = origin.x + (float)(state.loopStartBar - 1) * ticksPerBar * zoom;
        const float lx1 = origin.x + (float)state.loopEndBar * ticksPerBar * zoom;
        dl->AddRectFilled(ImVec2(lx0, origin.y), ImVec2(lx1, origin.y + contentH),
                          IM_COL32(120, 200, 120, 28));
        dl->AddLine(ImVec2(lx0, origin.y), ImVec2(lx0, origin.y + contentH),
                    IM_COL32(120, 220, 120, 200), 2.0f);
        dl->AddLine(ImVec2(lx1, origin.y), ImVec2(lx1, origin.y + contentH),
                    IM_COL32(120, 220, 120, 200), 2.0f);
    }

    // 노트 블록
    for (const auto& n : notes) {
        if (n.note < kLowNote || n.note >= kHighNote) continue;
        const int r = kHighNote - n.note;
        const float x0 = origin.x + n.startTick * zoom;
        const float x1 = origin.x + n.endTick * zoom;
        const float y0 = origin.y + r * kRowHeight;
        dl->AddRectFilled(ImVec2(x0, y0 + 1), ImVec2(std::max(x1, x0 + 2), y0 + kRowHeight - 1),
                          IM_COL32(90, 170, 250, 255), 2.0f);
        dl->AddRect(ImVec2(x0, y0 + 1), ImVec2(std::max(x1, x0 + 2), y0 + kRowHeight - 1),
                    IM_COL32(200, 220, 255, 255), 2.0f);
    }

    // 재생 헤드
    if (state.player && state.player->isPlaying()) {
        const float x = origin.x + playheadTick(state) * zoom;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + contentH),
                    IM_COL32(255, 90, 90, 255), 2.0f);
        if (state.followPlayhead) {
            const float scrollTarget = playheadTick(state) * zoom - ImGui::GetWindowWidth() * 0.5f;
            ImGui::SetScrollX(std::max(0.0f, scrollTarget));
        }
    }

    // 입력/스크롤 영역: 보이지 않는 버튼으로 클릭·호버를 잡는다.
    // (draw list는 이 위에 그려지므로 시각적 충돌 없음)
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("roll_input", ImVec2(contentW, contentH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    if (state.editMode) {
        // 마우스 위치 -> (틱, 노트). 드래그 중엔 아이템 밖이어도 전역 좌표 사용.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float relX = mouse.x - origin.x;
        const float relY = mouse.y - origin.y;
        const int hoverNote = kHighNote - (int)(relY / kRowHeight);
        const uint32_t hoverTick = relX > 0 ? (uint32_t)(relX / zoom) : 0;
        const uint32_t grid = std::max<uint32_t>(1, (uint32_t)state.song.ppqn / 4); // 16분음표
        auto snap = [grid](uint32_t t) { return (t / grid) * grid; };
        auto& drag = state.noteDrag;

        if (drag.active) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (drag.mode == AppState::NoteDrag::Move) {
                    long ns = (long)hoverTick - drag.grabTickOffset;
                    drag.curStart = snap((uint32_t)(ns < 0 ? 0 : ns));
                    drag.curNote = (uint8_t)std::clamp(hoverNote, 0, 127);
                    drag.curDuration = drag.durationTicks;
                } else { // Resize: 오른쪽 끝을 끈다
                    uint32_t endT = snap(hoverTick) + grid;
                    if (endT <= drag.startTick) endT = drag.startTick + grid;
                    drag.curStart = drag.startTick;
                    drag.curNote = drag.note;
                    drag.curDuration = endT - drag.startTick;
                }
            } else {
                // 놓음 -> 새 위치/길이로 확정 (undo 스냅샷은 드래그 시작 때 이미 남김)
                track.addNote(drag.curStart, std::max<uint32_t>(drag.curDuration, 1), drag.curNote,
                              drag.velocity);
                track.sortEvents();
                drag.active = false;
                state.statusMessage =
                    drag.mode == AppState::NoteDrag::Move ? "노트 이동" : "노트 길이 조절";
            }
        } else if (ImGui::IsItemHovered() && hoverNote >= kLowNote && hoverNote < kHighNote &&
                   relX >= 0) {
            bool found = false;
            const seq::NoteSpan hit = seq::noteSpanAt(track, (uint8_t)hoverNote, hoverTick, found);
            const bool leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            const bool rightClick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);

            if (rightClick && found) {
                state.snapshot();
                seq::removeNote(track, hit); // 우클릭 = 삭제
                state.statusMessage = "노트 삭제";
            } else if (leftClick && found) {
                // 좌클릭+드래그 = 이동/크기 조절. 오른쪽 끝 근처면 크기.
                state.snapshot();
                const float endX = origin.x + hit.endTick * zoom;
                const bool nearRight = (endX - mouse.x) >= -1.0f && (endX - mouse.x) <= 7.0f;
                seq::removeNote(track, hit);
                drag.active = true;
                drag.mode = nearRight ? AppState::NoteDrag::Resize : AppState::NoteDrag::Move;
                drag.note = hit.note;
                drag.velocity = hit.velocity;
                drag.startTick = hit.startTick;
                drag.durationTicks = hit.endTick - hit.startTick;
                drag.grabTickOffset = (int)hoverTick - (int)hit.startTick;
                drag.curNote = hit.note;
                drag.curStart = hit.startTick;
                drag.curDuration = drag.durationTicks;
            } else if (leftClick && !found) {
                // 빈 칸 좌클릭 -> 격자에 스냅해 노트 추가
                state.snapshot();
                const uint32_t len = (uint32_t)state.song.ppqn / (uint32_t)state.editNoteLenDiv;
                track.addNote(snap(hoverTick), len > 0 ? len : 1, (uint8_t)hoverNote, 100);
                track.sortEvents();
                state.statusMessage = "노트 추가";
            }
        }

        // 드래그 중 고스트 노트
        if (drag.active) {
            const int r = kHighNote - drag.curNote;
            if (r >= 0 && r < rows) {
                const float gx0 = origin.x + drag.curStart * zoom;
                const float gx1 = origin.x + (drag.curStart + drag.curDuration) * zoom;
                const float gy0 = origin.y + r * kRowHeight;
                dl->AddRectFilled(ImVec2(gx0, gy0 + 1),
                                  ImVec2(std::max(gx1, gx0 + 2), gy0 + kRowHeight - 1),
                                  IM_COL32(250, 210, 90, 220), 2.0f);
            }
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------
// 내장 신스 음색
// ---------------------------------------------------------
namespace {

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

} // namespace

void drawSynth(AppState& state) {
    if (!state.showSynth) return; // 메뉴(설정 > 신디사이저)로 열 때만 표시

    // 처음 열릴 때 화면 중앙에 팝업처럼 뜬다. X로 닫으면 showSynth=false.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(430, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("신디사이저", &state.showSynth);

    if (state.synth == nullptr) {
        ImGui::TextDisabled("신스를 사용할 수 없습니다.");
        ImGui::End();
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

    ImGui::End();
}

// ---------------------------------------------------------
// VST3 플러그인 (악기/이펙트)
// ---------------------------------------------------------
void drawVst(AppState& state) {
    if (!state.showVst) return;
    ImGui::Begin("VST3 플러그인", &state.showVst);

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
        state.vstPickInstrument = std::min(state.vstPickInstrument, (int)state.vstScanned.size() - 1);
        state.vstPickEffect = std::min(state.vstPickEffect, (int)state.vstScanned.size() - 1);
    }
    ImGui::SameLine();
    ImGui::Text("%d개 발견", (int)state.vstScanned.size());
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
