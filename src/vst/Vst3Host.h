#pragma once
// =============================================================
// MidiPro - vst/Vst3Host.h
// VST3 플러그인 호스트 (악기 VSTi / 이펙트 겸용).
//
// 계층 (Rule 1):
//   VST3 SDK 세부는 이 모듈 안에만 가둔다. 상위(오디오 엔진/GUI)는
//   이 얇은 인터페이스만 본다.
//
// 스레딩 (Rule 3):
//   - load/instantiate/unload/에디터: 제어(GUI) 스레드에서.
//   - 노트 이벤트 적재 + process(): 오디오 스레드에서. process 전에
//     같은 스레드에서 addNoteOn/Off로 이벤트를 쌓고 process가 소비한다.
//   - 로드/언로드는 오디오 스트림을 멈춘 상태에서 한다(엔진이 보장).
//
// SDK 헤더를 이 헤더에 포함하지 않으려고 impl은 Pimpl로 감춘다.
// =============================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct HWND__; // <windows.h> 없이 HWND 전방선언
using HWND = HWND__*;

namespace midipro::vst {

struct PluginClass {
    std::string name;
    bool isInstrument = false; // subCategory에 "Instrument"
    bool isEffect = false;     // 오디오 이펙트(입출력 있음)
};

class Vst3Host {
public:
    Vst3Host();
    ~Vst3Host();

    Vst3Host(const Vst3Host&) = delete;
    Vst3Host& operator=(const Vst3Host&) = delete;

    // .vst3 로드 후 내부 클래스 목록을 채운다. 실패 시 err에 사유.
    bool loadModule(const std::string& path, std::string& err);
    const std::vector<PluginClass>& classes() const;
    std::string modulePath() const;

    // 선택 클래스를 인스턴스화하고 오디오 처리를 준비한다.
    bool instantiate(int classIndex, double sampleRate, int maxBlockSize, std::string& err);
    // 오디오 스트림이 바뀌어(장치 변경·버퍼 크기 변경) 샘플레이트나 블록 크기가
    // 달라졌을 때 플러그인을 다시 준비한다. 값이 그대로면 아무 일도 하지 않는다.
    // 반드시 오디오가 멈춘 상태에서 부를 것. 음색(상태)은 유지된다.
    bool reconfigure(double sampleRate, int maxBlockSize);
    void unload(); // 에디터 닫고 플러그인/모듈 해제

    bool isLoaded() const;      // 인스턴스화까지 완료됨
    bool isInstrument() const;  // 악기(이벤트 입력)로 동작
    // 오디오 입력 버스가 있는가. 없으면 인서트 이펙트로 쓸 수 없다
    // (process()가 입력을 무시하고 출력 버퍼를 덮어써 무음이 된다).
    bool hasAudioInput() const;
    std::string activeName() const;

    // ---- 오디오 스레드 ----
    // process 호출 전에 이 블록에서 발생한 노트를 쌓는다.
    void addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void addNoteOff(uint8_t channel, uint8_t note);
    // 눌린 모든 노트에 note-off를 큐잉한다 (정지/시크 시 스턱 노트 방지).
    void addAllNotesOff();

    // MPE 표현: IMidiMapping으로 채널별 파라미터에 매핑해 전달한다.
    // (플러그인이 해당 컨트롤러를 매핑하지 않으면 조용히 무시)
    void addPitchBend(uint8_t channel, float bendNorm); // -1~1
    void addPressure(uint8_t channel, float value01);   // 채널 애프터터치
    void addTimbre(uint8_t channel, float value01);     // CC74
    // 일반 MIDI CC (모듈레이션 CC1, 서스테인 CC64 등). IMidiMapping으로
    // 플러그인이 그 CC에 매핑한 파라미터에 전달한다 (매핑 없으면 무시).
    void addControlChange(uint8_t channel, uint8_t ccNumber, float value01);
    // outputs: 채널별 planar 버퍼 [numChannels][frames].
    // 이펙트면 inputs(입력 오디오)를 받는다. 악기면 inputs=nullptr.
    void process(float** outputs, int numChannels, int frames, float** inputs = nullptr);

    // ---- 상태(패치) 저장/복원 (GUI 스레드, 로드된 상태에서) ----
    // VST3 표준 getState/setState로 플러그인 내부 상태(음색/노브 전부)를
    // 바이트로 직렬화한다. 컴포넌트 + 컨트롤러 상태를 한 블록에 담는다.
    bool saveState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t* data, std::size_t size);

    // ---- 에디터 (GUI 스레드) ----
    bool openEditor();   // 플러그인 GUI를 별도 창으로 연다
    void closeEditor();
    bool editorOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace midipro::vst
