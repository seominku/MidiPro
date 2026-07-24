#pragma once
// =============================================================
// MidiPro - audio/IVstHostControl.h
// VST3 호스트 제어 인터페이스 (GUI가 의존하는 얇은 경계, Rule 1).
//
// RtAudioEngine이 구현한다. GUI는 이 인터페이스로만 플러그인을
// 불러오거나 해제하고, 클래스 열거/에디터는 Vst3Host를 통해 한다.
// =============================================================

#include "vst/Vst3Host.h"

#include <string>

namespace midipro::audio {

class BuiltinFx; // 내장 이펙트 (audio/BuiltinFx.h)

class IVstHostControl {
public:
    virtual ~IVstHostControl() = default;

    // 스트림을 잠깐 멈추고 안전하게 로드/해제한다 (엔진이 처리).
    virtual bool loadInstrument(const std::string& path, int classIndex, std::string& err) = 0;
    virtual bool loadEffect(const std::string& path, int classIndex, std::string& err) = 0;
    virtual void clearInstrument() = 0;
    virtual void clearEffect() = 0;
    virtual bool instrumentActive() const = 0;
    virtual bool effectActive() const = 0;

    // 전역 이펙트를 실시간으로 바이패스(off)/활성(on). 오디오 스레드에서 안전.
    virtual void setEffectBypass(bool bypass) = 0;
    virtual bool effectBypassed() const = 0;

    // VSTi 출력 라우팅: 지정한 트랙 버스(0~15 = MIDI 채널)로 보내 그 트랙의
    // 볼륨/팬/이펙트 체인이 걸리게 한다. -1 = 마스터 직행(기존 동작). 실시간 안전.
    virtual void setInstrumentBus(int bus) = 0;
    virtual int instrumentBus() const = 0;

    // 클래스 목록/에디터 접근용 (GUI 스레드)
    virtual vst::Vst3Host& instrumentHost() = 0;
    virtual vst::Vst3Host& effectHost() = 0;

    // ---- 트랙별 이펙트 체인 (트랙 = MIDI 채널 0~15) ----
    // 각 트랙 버스에 순서대로 걸린다. 로드/해제는 스트림을 잠시 멈추고 안전하게.
    virtual bool loadTrackEffect(int channel, const std::string& path, int classIndex,
                                 std::string& err) = 0;
    virtual void removeTrackEffect(int channel, int index) = 0;
    // 체인 순서 변경 (from 위치의 이펙트를 to 위치로). 스트림을 잠시 멈추고 바꾼다.
    virtual void moveTrackEffect(int channel, int from, int to) = 0;
    virtual void clearTrackEffects(int channel) = 0;
    virtual int trackEffectCount(int channel) const = 0;
    virtual std::string trackEffectName(int channel, int index) const = 0;
    virtual bool trackEffectEnabled(int channel, int index) const = 0;
    virtual void setTrackEffectEnabled(int channel, int index, bool on) = 0; // 실시간 바이패스
    virtual vst::Vst3Host* trackEffectHost(int channel, int index) = 0;      // 에디터 열기용

    // ---- 내장 이펙트 (EQ/딜레이/리버브) — VST와 같은 체인에 끼워진다 ----
    // type = BuiltinFx::kEq/kDelay/kReverb. 성공하면 체인 끝에 추가된다.
    virtual bool addBuiltinTrackEffect(int channel, int type) { (void)channel; (void)type;
        return false; }
    // 그 슬롯이 내장 이펙트면 객체를 (아니면 nullptr) 돌려준다 — 파라미터 창용.
    // 파라미터는 atomic이라 GUI에서 바로 읽고 써도 안전하다.
    virtual BuiltinFx* trackEffectBuiltin(int channel, int index) { (void)channel; (void)index;
        return nullptr; }
    // 내장 컴프레서의 사이드체인 키 버스 (-1 = 자기 입력). 실시간 안전.
    virtual void setTrackEffectSidechain(int channel, int index, int bus) {
        (void)channel; (void)index; (void)bus;
    }
    virtual int trackEffectSidechain(int channel, int index) const {
        (void)channel; (void)index;
        return -1;
    }

    // 이 번들에 인서트 이펙트로 쓸 수 있는 클래스가 있는가 (악기 전용이면 false).
    // GUI가 트랙 이펙트/악기 목록을 거르는 데 쓴다.
    // 판정하려면 플러그인을 실제로 열어 봐야 해서 느리다(큰 악기는 몇 초씩).
    // 그래서 결과를 디스크에 기억해 두고, 두 번째 실행부터는 즉시 답한다.
    virtual bool pluginHasEffectClass(const std::string& path) = 0;
    virtual bool pluginHasInstrumentClass(const std::string& path) = 0;
    // 기억해 둔 조사 결과를 버린다 (다음 조회 때 다시 열어 본다).
    virtual void clearPluginCache() = 0;

    // ---- 트랙별 악기 (트랙 = MIDI 채널 0~15, 슬롯 1개) ----
    // 로드하면 그 채널의 노트가 이 악기로 라우팅되고, 출력은 그 트랙 버스를
    // 타서 트랙 볼륨/팬/이펙트 체인이 걸린다. 여러 트랙에 서로 다른 악기 가능.
    virtual bool loadTrackInstrument(int channel, const std::string& path, int classIndex,
                                     std::string& err) = 0;
    virtual void clearTrackInstrument(int channel) = 0;
    virtual bool trackInstrumentActive(int channel) const = 0;
    virtual std::string trackInstrumentName(int channel) const = 0;
    virtual vst::Vst3Host* trackInstrumentHost(int channel) = 0; // 에디터 열기용
};

} // namespace midipro::audio
