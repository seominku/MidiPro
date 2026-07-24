// =============================================================
// MidiPro - vst/Vst3Host.cpp
// VST3 SDK를 이용한 최소 호스트 구현. SDK 의존은 전부 여기 안에.
// =============================================================

#include "vst/Vst3Host.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <windows.h>

#include <algorithm>
#include <array>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace midipro::vst {

namespace {
// 스테레오 스피커 배치 (L|R). SpeakerArr 상수 대신 명시적으로 둔다.
constexpr uint64 kStereoArr = 0x3;
constexpr wchar_t kEditorWndClass[] = L"MidiProVstEditor";
} // namespace

// 플러그인이 에디터 리사이즈를 요청할 때 호스트 창을 맞춰주는 최소 프레임.
class HostPlugFrame : public U::Implements<U::Directly<IPlugFrame>> {
public:
    explicit HostPlugFrame(HWND wnd) : m_wnd(wnd) {}
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* rect) override {
        if (!rect || !m_wnd) return kResultFalse;
        RECT r = {0, 0, rect->getWidth(), rect->getHeight()};
        AdjustWindowRect(&r, GetWindowLong(m_wnd, GWL_STYLE), FALSE);
        SetWindowPos(m_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        if (view) view->onSize(rect);
        return kResultTrue;
    }

private:
    HWND m_wnd = nullptr;
};

// 최소 컴포넌트 핸들러.
// 규격상 호스트는 컨트롤러에 이걸 반드시 물려 줘야 한다. 큰 플러그인은 노브를
// 잡는 순간(beginEdit) 호스트를 부르는데, 핸들러가 없으면 그대로 죽는 것들이 있다.
// 우리는 오토메이션을 기록하지 않으므로 받기만 하고 성공만 돌려준다.
class HostComponentHandler : public U::Implements<U::Directly<IComponentHandler>> {
public:
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

struct Vst3Host::Impl {
    VST3::Hosting::Module::Ptr module;
    std::string path;
    std::vector<PluginClass> classes;

    IPtr<IHostApplication> hostContext;
    IPtr<IComponentHandler> componentHandler;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    IPtr<IMidiMapping> midiMapping;

    // 채널별(0~15) 표현 컨트롤러 -> 파라미터 ID 캐시.
    // 0=피치벤드, 1=애프터터치, 2=음색(CC74). 매핑 없으면 kNoParamId.
    static constexpr int kBend = 0, kAfter = 1, kTimbre = 2;
    ParamID exprParam[16][3];
    // 일반 MIDI CC(0~127) -> 파라미터 매핑 캐시 (채널별). 로드 시 한 번 조회해
    // 오디오 스레드에서 플러그인 컨트롤러를 재호출하지 않는다.
    ParamID ccParam[16][128];

    bool loaded = false;
    bool instrument = false;
    bool hasAudioInput = false;
    // 눌린 노트 추적 (오디오 스레드 전용). 정지 시 addAllNotesOff가
    // 눌린 것들만 note-off로 큐잉해 스턱 노트를 막는다.
    bool activeNote[16][128] = {};
    std::string name;
    double sampleRate = 44100.0;
    int maxBlock = 512;

    // 오디오 스레드 처리용 (사전 준비)
    EventList eventList{256};
    ParameterChanges inParams;
    ParameterChanges outParams;
    ProcessData data;
    AudioBusBuffers inBus;
    AudioBusBuffers outBus;
    ProcessContext ctx{};
    std::array<float*, 2> inPtrs{nullptr, nullptr};
    std::array<float*, 2> outPtrs{nullptr, nullptr};

    // ---- 다중 버스 대응 ----
    // Omnisphere·Trilian·Keyscape처럼 멀티아웃 악기는 출력 버스가 여러 개다
    // (메인 + 파트별 aux). VST3 규격상 호스트는 "플러그인이 가진 모든 버스"에
    // 대해 AudioBusBuffers를 줘야 하고, 플러그인은 그 개수만큼 배열을 훑는다.
    // 버스 하나만 넘기면 플러그인이 없는 배열 원소를 읽어 그대로 죽는다.
    // 그래서 버스 개수만큼 자리를 만들고, 0번만 진짜 출력에 연결한다.
    std::vector<AudioBusBuffers> outBuses;
    std::vector<AudioBusBuffers> inBuses;
    std::vector<std::vector<float*>> outBusPtrs; // 버스별 채널 포인터 배열
    std::vector<std::vector<float*>> inBusPtrs;
    std::vector<float> scratch;   // 쓰지 않는 버스가 뱉는 소리를 받아 버릴 곳
    std::vector<float> silence;   // 쓰지 않는 입력 버스에 물릴 무음
    int mainOutChannels = 2;

    // 에디터
    IPtr<IPlugView> view;
    std::unique_ptr<HostPlugFrame> frame;
    HWND editorWnd = nullptr;

    void addExpression(int ch, int which, float value01);

    void teardownEditor() {
        if (view) {
            view->setFrame(nullptr);
            view->removed();
            view = nullptr;
        }
        frame.reset();
        if (editorWnd) {
            DestroyWindow(editorWnd);
            editorWnd = nullptr;
        }
    }

    void teardownPlugin() {
        teardownEditor();
        if (processor) processor->setProcessing(false);
        if (component) component->setActive(false);
        // 컴포넌트/컨트롤러 연결 해제
        if (component && controller) {
            if (auto compICP = U::cast<IConnectionPoint>(component))
                if (auto ctrlICP = U::cast<IConnectionPoint>(controller)) {
                    compICP->disconnect(ctrlICP);
                    ctrlICP->disconnect(compICP);
                }
        }
        midiMapping = nullptr;
        if (controller) {
            controller->setComponentHandler(nullptr); // 먼저 끊고 끝낸다
            controller->terminate();
            controller = nullptr;
        }
        if (component) {
            component->terminate();
            component = nullptr;
        }
        processor = nullptr;
        loaded = false;
    }
};

Vst3Host::Vst3Host() : m_impl(std::make_unique<Impl>()) {}
Vst3Host::~Vst3Host() { unload(); }

bool Vst3Host::loadModule(const std::string& path, std::string& err) {
    unload();
    auto module = VST3::Hosting::Module::create(path, err);
    if (!module) return false;

    m_impl->module = module;
    m_impl->path = path;
    m_impl->classes.clear();

    for (const auto& ci : module->getFactory().classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue; // "Audio Module Class"
        PluginClass pc;
        pc.name = ci.name();
        const auto sub = ci.subCategoriesString();
        pc.isInstrument = sub.find("Instrument") != std::string::npos;
        pc.isEffect = !pc.isInstrument;
        m_impl->classes.push_back(pc);
    }
    return true;
}

const std::vector<PluginClass>& Vst3Host::classes() const { return m_impl->classes; }
std::string Vst3Host::modulePath() const { return m_impl->path; }

bool Vst3Host::instantiate(int classIndex, double sampleRate, int maxBlockSize, std::string& err) {
    if (!m_impl->module) {
        err = "모듈이 로드되지 않음";
        return false;
    }
    m_impl->teardownPlugin();

    const auto& factory = m_impl->module->getFactory();
    const auto infos = factory.classInfos();
    // classes[] 는 오디오 모듈만 걸러낸 목록이므로 인덱스를 원본으로 환산
    std::vector<VST3::Hosting::ClassInfo> audioInfos;
    for (const auto& ci : infos)
        if (ci.category() == kVstAudioEffectClass) audioInfos.push_back(ci);
    if (classIndex < 0 || classIndex >= (int)audioInfos.size()) {
        err = "잘못된 클래스 인덱스";
        return false;
    }
    const auto& info = audioInfos[classIndex];

    if (!m_impl->hostContext)
        m_impl->hostContext = owned(new HostApplication());

    m_impl->component = factory.createInstance<IComponent>(info.ID());
    if (!m_impl->component) {
        err = "컴포넌트 생성 실패";
        return false;
    }
    if (m_impl->component->initialize(m_impl->hostContext) != kResultOk) {
        err = "컴포넌트 초기화 실패";
        m_impl->teardownPlugin();
        return false;
    }

    m_impl->processor = U::cast<IAudioProcessor>(m_impl->component);
    if (!m_impl->processor) {
        err = "IAudioProcessor 없음";
        m_impl->teardownPlugin();
        return false;
    }

    // 컨트롤러: 컴포넌트가 겸하거나 별도 클래스
    TUID cid;
    if (m_impl->component->getControllerClassId(cid) == kResultOk) {
        m_impl->controller = factory.createInstance<IEditController>(VST3::UID(cid));
    }
    if (!m_impl->controller)
        m_impl->controller = U::cast<IEditController>(m_impl->component);
    if (m_impl->controller && m_impl->controller.get() != (IEditController*)m_impl->component.get())
        m_impl->controller->initialize(m_impl->hostContext);

    // 컴포넌트 <-> 컨트롤러 연결 + 상태 전달
    if (m_impl->controller) {
        // 핸들러부터 물려 준다 (연결/상태 전달 중에 부르는 플러그인이 있다)
        if (!m_impl->componentHandler)
            m_impl->componentHandler = owned(new HostComponentHandler());
        m_impl->controller->setComponentHandler(m_impl->componentHandler);

        if (auto compICP = U::cast<IConnectionPoint>(m_impl->component))
            if (auto ctrlICP = U::cast<IConnectionPoint>(m_impl->controller)) {
                compICP->connect(ctrlICP);
                ctrlICP->connect(compICP);
            }
        MemoryStream stream;
        if (m_impl->component->getState(&stream) == kResultOk) {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            m_impl->controller->setComponentState(&stream);
        }
    }

    // MPE 표현 매핑 캐시: 채널별로 피치벤드/애프터터치/CC74가 어떤
    // 파라미터에 연결되는지 미리 조회해 둔다 (오디오 스레드에서 재조회 방지).
    for (int ch = 0; ch < 16; ++ch)
        for (int k = 0; k < 3; ++k) m_impl->exprParam[ch][k] = kNoParamId;
    for (int ch = 0; ch < 16; ++ch)
        for (int c = 0; c < 128; ++c) m_impl->ccParam[ch][c] = kNoParamId;
    m_impl->midiMapping = U::cast<IMidiMapping>(m_impl->controller);
    if (m_impl->midiMapping) {
        const int16 ctrlNum[3] = {kPitchBend, kAfterTouch, kCtrlFilterResonance /*CC74*/};
        for (int ch = 0; ch < 16; ++ch)
            for (int k = 0; k < 3; ++k) {
                ParamID id = kNoParamId;
                if (m_impl->midiMapping->getMidiControllerAssignment(
                        0, (int16)ch, (CtrlNumber)ctrlNum[k], id) == kResultTrue)
                    m_impl->exprParam[ch][k] = id;
            }
        // 일반 CC 전체 (모듈레이션/서스테인/익스프레션 등 CC 레인이 쓴다)
        for (int ch = 0; ch < 16; ++ch)
            for (int c = 0; c < 128; ++c) {
                ParamID id = kNoParamId;
                if (m_impl->midiMapping->getMidiControllerAssignment(0, (int16)ch,
                                                                     (CtrlNumber)c, id) ==
                    kResultTrue)
                    m_impl->ccParam[ch][c] = id;
            }
    }

    // 버스 구성.
    // 멀티아웃 악기(Omnisphere/Trilian/Keyscape 등)는 출력 버스가 여러 개다.
    // 우리는 0번(메인)만 실제로 쓰지만, 규격상 나머지 버스에도 버퍼를 줘야 한다.
    const int nInBus = m_impl->component->getBusCount(kAudio, kInput);
    const int nOutBus = m_impl->component->getBusCount(kAudio, kOutput);
    const bool hasAudioIn = nInBus > 0;
    const bool hasAudioOut = nOutBus > 0;
    const bool hasEventIn = m_impl->component->getBusCount(kEvent, kInput) > 0;
    m_impl->hasAudioInput = hasAudioIn;
    m_impl->instrument = hasEventIn; // 이벤트 입력이 있으면 악기로 취급

    if (!hasAudioOut) {
        err = "오디오 출력 버스가 없는 플러그인입니다";
        m_impl->teardownPlugin();
        return false;
    }

    // 메인(0번)만 켜고 나머지 보조 출력은 끈다 — 끈 버스도 버퍼는 줘야 한다.
    if (hasAudioIn) m_impl->component->activateBus(kAudio, kInput, 0, true);
    for (int b = 1; b < nInBus; ++b) m_impl->component->activateBus(kAudio, kInput, b, false);
    m_impl->component->activateBus(kAudio, kOutput, 0, true);
    for (int b = 1; b < nOutBus; ++b) m_impl->component->activateBus(kAudio, kOutput, b, false);
    if (hasEventIn) m_impl->component->activateBus(kEvent, kInput, 0, true);

    // 스피커 배치는 버스 개수만큼 넘겨야 한다 (개수가 다르면 플러그인이 거부한다).
    {
        std::vector<SpeakerArrangement> ins((std::size_t)(nInBus > 0 ? nInBus : 0), kStereoArr);
        std::vector<SpeakerArrangement> outs((std::size_t)nOutBus, kStereoArr);
        m_impl->processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), nInBus,
                                              outs.data(), nOutBus);
        // 결과는 참고만 한다. 플러그인이 거부했어도 아래에서 "실제" 채널 수를
        // 다시 물어보고 그 값에 맞춰 버퍼를 준비하므로 어긋날 일이 없다.
    }

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;
    if (m_impl->processor->setupProcessing(setup) != kResultOk) {
        err = "setupProcessing 실패";
        m_impl->teardownPlugin();
        return false;
    }

    m_impl->component->setActive(true);
    m_impl->processor->setProcessing(true);

    // ProcessData 사전 구성 (Rule 3: 콜백에서 재할당 없음)
    m_impl->sampleRate = sampleRate;
    m_impl->maxBlock = maxBlockSize;
    // 표현 파라미터 변경을 담을 여유 (채널×컨트롤러)
    m_impl->inParams.setMaxParameters(64);
    m_impl->outParams.setMaxParameters(0);
    m_impl->ctx = ProcessContext{};
    m_impl->ctx.sampleRate = sampleRate;

    // 버스별 채널 수는 플러그인에게 직접 물어본다 (setBusArrangements가 거부됐을
    // 수도 있으므로 "우리가 요청한 값"이 아니라 "실제 값"을 써야 안전하다).
    auto busChannels = [&](BusDirection dir, int index) {
        BusInfo bi{};
        if (m_impl->component->getBusInfo(kAudio, dir, index, bi) == kResultOk)
            return (int)bi.channelCount;
        return 2;
    };

    // 안 쓰는 버스가 뱉는 소리를 받아 버릴 스크래치 / 물려 줄 무음 버퍼.
    // 채널을 공유해도 된다 — 어차피 내용을 읽지 않는다.
    m_impl->scratch.assign((std::size_t)maxBlockSize, 0.0f);
    m_impl->silence.assign((std::size_t)maxBlockSize, 0.0f);

    m_impl->outBusPtrs.clear();
    m_impl->outBuses.clear();
    m_impl->outBusPtrs.resize((std::size_t)nOutBus);
    m_impl->outBuses.resize((std::size_t)nOutBus);
    for (int b = 0; b < nOutBus; ++b) {
        const int ch = busChannels(kOutput, b);
        m_impl->outBusPtrs[(std::size_t)b].assign((std::size_t)(ch > 0 ? ch : 1), nullptr);
        auto& bus = m_impl->outBuses[(std::size_t)b];
        bus.numChannels = ch;
        bus.silenceFlags = 0;
        bus.channelBuffers32 = m_impl->outBusPtrs[(std::size_t)b].data();
    }
    m_impl->mainOutChannels = nOutBus > 0 ? m_impl->outBuses[0].numChannels : 2;

    m_impl->inBusPtrs.clear();
    m_impl->inBuses.clear();
    if (nInBus > 0) {
        m_impl->inBusPtrs.resize((std::size_t)nInBus);
        m_impl->inBuses.resize((std::size_t)nInBus);
        for (int b = 0; b < nInBus; ++b) {
            const int ch = busChannels(kInput, b);
            m_impl->inBusPtrs[(std::size_t)b].assign((std::size_t)(ch > 0 ? ch : 1), nullptr);
            auto& bus = m_impl->inBuses[(std::size_t)b];
            bus.numChannels = ch;
            bus.silenceFlags = 0;
            bus.channelBuffers32 = m_impl->inBusPtrs[(std::size_t)b].data();
        }
    }

    m_impl->data.processMode = kRealtime;
    m_impl->data.symbolicSampleSize = kSample32;
    m_impl->data.numSamples = 0;
    m_impl->data.numInputs = nInBus;
    m_impl->data.numOutputs = nOutBus;
    m_impl->data.inputs = nInBus > 0 ? m_impl->inBuses.data() : nullptr;
    m_impl->data.outputs = m_impl->outBuses.data();
    m_impl->data.inputParameterChanges = &m_impl->inParams;
    m_impl->data.outputParameterChanges = &m_impl->outParams;
    m_impl->data.inputEvents = &m_impl->eventList;
    m_impl->data.outputEvents = nullptr;
    m_impl->data.processContext = &m_impl->ctx;

    m_impl->name = info.name();
    m_impl->loaded = true;
    return true;
}

bool Vst3Host::reconfigure(double sampleRate, int maxBlockSize) {
    if (!m_impl->loaded || !m_impl->processor || !m_impl->component) return false;
    if (maxBlockSize <= 0) return false;
    // 값이 같으면 건드리지 않는다 (플러그인을 껐다 켜는 건 비싸다)
    if (sampleRate == m_impl->sampleRate && maxBlockSize == m_impl->maxBlock) return true;

    // setupProcessing은 "비활성 상태"에서만 부를 수 있다 (VST3 규격).
    m_impl->processor->setProcessing(false);
    m_impl->component->setActive(false);

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;
    const bool ok = m_impl->processor->setupProcessing(setup) == kResultOk;

    m_impl->component->setActive(true);
    m_impl->processor->setProcessing(true);

    if (ok) {
        m_impl->sampleRate = sampleRate;
        m_impl->maxBlock = maxBlockSize;
        m_impl->ctx.sampleRate = sampleRate;
        // 보조 버스용 버퍼도 새 블록 크기에 맞춘다
        m_impl->scratch.assign((std::size_t)maxBlockSize, 0.0f);
        m_impl->silence.assign((std::size_t)maxBlockSize, 0.0f);
    }
    // 실패해도 이전 설정 그대로 계속 돌아간다 (블록 수는 process에서 잘라 준다)
    return ok;
}

void Vst3Host::unload() {
    if (!m_impl) return;
    m_impl->teardownPlugin();
    m_impl->module = nullptr;
    m_impl->classes.clear();
    m_impl->path.clear();
}

bool Vst3Host::isLoaded() const { return m_impl && m_impl->loaded; }
bool Vst3Host::isInstrument() const { return m_impl && m_impl->instrument; }
bool Vst3Host::hasAudioInput() const { return m_impl && m_impl->hasAudioInput; }
std::string Vst3Host::activeName() const { return m_impl ? m_impl->name : std::string(); }

void Vst3Host::addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!m_impl->loaded) return;
    Event e{};
    e.busIndex = 0;
    e.sampleOffset = 0;
    e.flags = Event::kIsLive;
    e.type = Event::kNoteOnEvent;
    e.noteOn.channel = channel;
    e.noteOn.pitch = note;
    e.noteOn.tuning = 0.0f;
    e.noteOn.velocity = (float)velocity / 127.0f;
    e.noteOn.length = 0;
    e.noteOn.noteId = -1;
    m_impl->eventList.addEvent(e);
    m_impl->activeNote[channel & 0x0F][note & 0x7F] = true;
}

void Vst3Host::addNoteOff(uint8_t channel, uint8_t note) {
    if (!m_impl->loaded) return;
    Event e{};
    e.busIndex = 0;
    e.sampleOffset = 0;
    e.flags = Event::kIsLive;
    e.type = Event::kNoteOffEvent;
    e.noteOff.channel = channel;
    e.noteOff.pitch = note;
    e.noteOff.velocity = 0.0f;
    e.noteOff.noteId = -1;
    m_impl->eventList.addEvent(e);
    m_impl->activeNote[channel & 0x0F][note & 0x7F] = false;
}

// ---- 상태(패치) 저장/복원 ----
// 포맷: "MPST" + u32 컴포넌트크기 + 컴포넌트바이트 + u32 컨트롤러크기 + 컨트롤러바이트
bool Vst3Host::saveState(std::vector<uint8_t>& out) const {
    out.clear();
    if (!m_impl->loaded || !m_impl->component) return false;

    MemoryStream comp;
    if (m_impl->component->getState(&comp) != kResultOk) return false;
    MemoryStream ctrl;
    const bool hasCtrl =
        m_impl->controller && m_impl->controller->getState(&ctrl) == kResultOk;

    auto putU32 = [&out](uint32_t v) {
        out.push_back((uint8_t)(v & 0xFF));
        out.push_back((uint8_t)((v >> 8) & 0xFF));
        out.push_back((uint8_t)((v >> 16) & 0xFF));
        out.push_back((uint8_t)((v >> 24) & 0xFF));
    };
    out.push_back('M');
    out.push_back('P');
    out.push_back('S');
    out.push_back('T');
    const uint32_t compSize = (uint32_t)comp.getSize();
    putU32(compSize);
    const uint8_t* cd = (const uint8_t*)comp.getData();
    out.insert(out.end(), cd, cd + compSize);
    const uint32_t ctrlSize = hasCtrl ? (uint32_t)ctrl.getSize() : 0;
    putU32(ctrlSize);
    if (ctrlSize > 0) {
        const uint8_t* td = (const uint8_t*)ctrl.getData();
        out.insert(out.end(), td, td + ctrlSize);
    }
    return true;
}

bool Vst3Host::loadState(const uint8_t* data, std::size_t size) {
    if (!m_impl->loaded || !m_impl->component) return false;
    if (!data || size < 12) return false;
    if (data[0] != 'M' || data[1] != 'P' || data[2] != 'S' || data[3] != 'T') return false;

    auto readU32 = [&](std::size_t at) {
        return (uint32_t)data[at] | ((uint32_t)data[at + 1] << 8) |
               ((uint32_t)data[at + 2] << 16) | ((uint32_t)data[at + 3] << 24);
    };
    const uint32_t compSize = readU32(4);
    if (8 + (std::size_t)compSize + 4 > size) return false;
    const uint8_t* compData = data + 8;
    const uint32_t ctrlSize = readU32(8 + compSize);
    if (8 + (std::size_t)compSize + 4 + ctrlSize > size) return false;
    const uint8_t* ctrlData = data + 8 + compSize + 4;

    // MemoryStream(비소유)으로 감싸 setState에 넘긴다. 각 사용 전 처음으로 되감기.
    {
        MemoryStream cs((void*)compData, (TSize)compSize);
        if (m_impl->component->setState(&cs) != kResultOk) return false;
    }
    if (m_impl->controller) {
        // 컨트롤러에게 컴포넌트 상태를 알리고, 자체 상태도 복원한다.
        MemoryStream cs2((void*)compData, (TSize)compSize);
        m_impl->controller->setComponentState(&cs2);
        if (ctrlSize > 0) {
            MemoryStream ts((void*)ctrlData, (TSize)ctrlSize);
            m_impl->controller->setState(&ts);
        }
    }
    return true;
}

void Vst3Host::addAllNotesOff() {
    if (!m_impl->loaded) return;
    // 눌린 노트에만 note-off를 보낸다 (정지/시크 시 스턱 노트 방지).
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (m_impl->activeNote[ch][n]) addNoteOff((uint8_t)ch, (uint8_t)n);
}

// 채널의 표현 컨트롤러를 매핑된 파라미터 변경으로 큐잉한다.
void Vst3Host::Impl::addExpression(int ch, int which, float value01) {
    if (!midiMapping || ch < 0 || ch > 15) return;
    const ParamID id = exprParam[ch][which];
    if (id == kNoParamId) return;
    int32 index = 0;
    if (IParamValueQueue* q = inParams.addParameterData(id, index)) {
        int32 pt = 0;
        q->addPoint(0, (ParamValue)(value01 < 0.f ? 0.f : (value01 > 1.f ? 1.f : value01)), pt);
    }
}

void Vst3Host::addPitchBend(uint8_t channel, float bendNorm) {
    if (!m_impl->loaded) return;
    m_impl->addExpression(channel, Impl::kBend, bendNorm * 0.5f + 0.5f); // -1~1 -> 0~1
}

void Vst3Host::addPressure(uint8_t channel, float value01) {
    if (!m_impl->loaded) return;
    m_impl->addExpression(channel, Impl::kAfter, value01);
}

void Vst3Host::addTimbre(uint8_t channel, float value01) {
    if (!m_impl->loaded) return;
    m_impl->addExpression(channel, Impl::kTimbre, value01);
}

void Vst3Host::addControlChange(uint8_t channel, uint8_t ccNumber, float value01) {
    if (!m_impl->loaded || !m_impl->midiMapping) return;
    const int ch = channel & 0x0F;
    if (ccNumber > 127) return;
    const ParamID id = m_impl->ccParam[ch][ccNumber];
    if (id == kNoParamId) return; // 플러그인이 이 CC를 매핑하지 않음
    int32 index = 0;
    if (IParamValueQueue* q = m_impl->inParams.addParameterData(id, index)) {
        int32 pt = 0;
        q->addPoint(0, (ParamValue)(value01 < 0.f ? 0.f : (value01 > 1.f ? 1.f : value01)), pt);
    }
}

void Vst3Host::process(float** outputs, int numChannels, int frames, float** inputs) {
    if (!m_impl->loaded || !m_impl->processor) return;
    if (frames > m_impl->maxBlock) frames = m_impl->maxBlock;

    // 0번(메인) 버스만 진짜 버퍼에 연결하고, 나머지 버스는 버리는 버퍼로 채운다.
    // 규격상 모든 버스의 모든 채널 포인터가 유효해야 한다 — 하나라도 비면
    // 플러그인이 그대로 죽는다 (멀티아웃 악기에서 실제로 터진 지점).
    float* const junk = m_impl->scratch.data();
    float* const quiet = m_impl->silence.data();

    for (std::size_t b = 0; b < m_impl->outBuses.size(); ++b) {
        auto& ptrs = m_impl->outBusPtrs[b];
        for (std::size_t c = 0; c < ptrs.size(); ++c) {
            if (b == 0)
                ptrs[c] = outputs[(int)c < numChannels ? (int)c : 0];
            else
                ptrs[c] = junk; // 보조 출력은 받아서 버린다
        }
        m_impl->outBuses[b].silenceFlags = 0;
    }
    for (std::size_t b = 0; b < m_impl->inBuses.size(); ++b) {
        auto& ptrs = m_impl->inBusPtrs[b];
        for (std::size_t c = 0; c < ptrs.size(); ++c) {
            if (b == 0 && inputs)
                ptrs[c] = inputs[(int)c < numChannels ? (int)c : 0];
            else
                ptrs[c] = quiet; // 입력이 없으면 무음을 물린다
        }
        m_impl->inBuses[b].silenceFlags = 0;
    }

    m_impl->data.numSamples = frames;
    m_impl->processor->process(m_impl->data);

    // 소비한 이벤트/파라미터 변경 비우기 (다음 블록 준비)
    m_impl->eventList.clear();
    m_impl->inParams.clearQueue();
}

// ---------------------------------------------------------
// 에디터 창
// ---------------------------------------------------------
namespace {
LRESULT CALLBACK editorWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE) {
        ShowWindow(hwnd, SW_HIDE); // 실제 정리는 closeEditor/unload에서
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace

bool Vst3Host::openEditor() {
    if (!m_impl->loaded || !m_impl->controller) return false;
    if (m_impl->view) {
        ShowWindow(m_impl->editorWnd, SW_SHOW);
        return true;
    }

    IPtr<IPlugView> view = owned(m_impl->controller->createView(ViewType::kEditor));
    if (!view || view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) return false;

    ViewRect rect{};
    view->getSize(&rect);
    int w = rect.getWidth() > 0 ? rect.getWidth() : 400;
    int h = rect.getHeight() > 0 ? rect.getHeight() : 300;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = editorWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = kEditorWndClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT wr = {0, 0, w, h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    std::wstring title = L"VST 에디터";
    HWND wnd = CreateWindowW(kEditorWndClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr,
                             GetModuleHandle(nullptr), nullptr);
    if (!wnd) return false;

    m_impl->frame = std::make_unique<HostPlugFrame>(wnd);
    view->setFrame(m_impl->frame.get());
    if (view->attached(wnd, kPlatformTypeHWND) != kResultTrue) {
        DestroyWindow(wnd);
        m_impl->frame.reset();
        return false;
    }
    m_impl->view = view;
    m_impl->editorWnd = wnd;
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    return true;
}

void Vst3Host::closeEditor() {
    if (m_impl) m_impl->teardownEditor();
}

bool Vst3Host::editorOpen() const {
    return m_impl && m_impl->view && m_impl->editorWnd && IsWindowVisible(m_impl->editorWnd);
}

} // namespace midipro::vst
