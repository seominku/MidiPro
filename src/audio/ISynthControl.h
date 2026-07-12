#pragma once
// =============================================================
// MidiPro - audio/ISynthControl.h
// 신스 파라미터 제어 인터페이스.
//
// 왜 인터페이스인가 (Rule 1):
//   GUI(신스 패널)는 파라미터를 바꾸고 상태를 읽기만 하면 되지,
//   RtAudio 구현을 알 필요가 없다. RtAudioEngine이 이걸 구현한다.
// =============================================================

#include "audio/Synth.h"

#include <string>
#include <vector>

namespace midipro::audio {

class ISynthControl {
public:
    virtual ~ISynthControl() = default;

    // 음색 파라미터 / 상태
    virtual void setParams(const SynthParams& params) = 0;
    virtual int activeVoiceCount() const = 0;

    // 채널(=트랙) 볼륨/팬. 믹서의 트랙 볼륨·팬·뮤트가 MIDI 트랙에도 걸리게 한다.
    // 값이 바뀔 때만 오디오 스레드로 전달된다.
    virtual void setChannelMix(int channel, float gain, float pan) = 0;

    // 오디오 출력 장치 선택 (예: 포커스라이트 등 오디오 인터페이스)
    virtual std::vector<std::string> listOutputDevices() = 0;
    virtual int outputDevice() const = 0;         // 현재 장치의 목록 내 인덱스 (-1=기본/미선택)
    virtual void setOutputDevice(int index) = 0;  // 스트림이 열려 있으면 재시작
    virtual double currentSampleRate() const = 0; // 현재 스트림 샘플레이트

    // MPE (노트별 피치벤드/압력/음색) 모드 토글
    virtual void setMpeMode(bool enabled) = 0;
    virtual bool mpeMode() const = 0;
};

} // namespace midipro::audio
