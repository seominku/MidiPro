#pragma once
// =============================================================
// MidiPro - audio/IAudioInput.h
// 오디오 입력(마이크/오디오 인터페이스) 캡처 인터페이스 (Rule 1의 경계).
//
// RtAudioEngine이 별도 입력 스트림으로 구현한다. GUI는 이 인터페이스로만
// 모니터링(실시간 듣기)과 트랙 녹음을 제어한다.
// =============================================================

#include "audio/AudioClip.h"

#include <memory>
#include <string>
#include <vector>

namespace midipro::audio {

class IAudioInput {
public:
    virtual ~IAudioInput() = default;

    // 입력 장치 열거/선택 (GUI 스레드)
    virtual std::vector<std::string> listInputDevices() = 0;
    virtual int inputDevice() const = 0;        // 현재 선택 인덱스(-1=없음)
    virtual void setInputDevice(int index) = 0;

    // 입력 채널 모드: 0=1+2 합침(스테레오 다운믹스), 1=입력1만, 2=입력2만
    virtual void setInputChannelMode(int mode) = 0;
    virtual int inputChannelMode() const = 0;

    // 레이턴시 튜닝용 버퍼 프레임 수(작을수록 지연↓, 끊김 위험↑). 스트림 재시작.
    virtual void setBufferFrames(unsigned frames) = 0;
    virtual unsigned bufferFrames() const = 0;

    // 출력 샘플레이트 지정(WASAPI). 장치가 지원하는 값만 반영, ASIO 중엔 무시.
    virtual void setPreferredSampleRate(unsigned hz) = 0;
    virtual unsigned preferredSampleRate() const = 0; // 0 = 장치 기본값
    virtual std::vector<unsigned> supportedSampleRates() const = 0;

    // 캡처 스트림 시작/정지
    virtual bool startInput() = 0;
    virtual void stopInput() = 0;
    virtual bool inputActive() const = 0;

    // 모니터링: 입력을 출력으로 실시간 통과(연주를 바로 듣기)
    virtual void setMonitor(bool on) = 0;
    virtual bool monitorOn() const = 0;
    // 모니터 입력에 적용할 게인(모니터 중인 트랙 볼륨). 오디오 스레드 안전.
    virtual void setMonitorGain(float gain) = 0;

    // 녹음: 시작하면 입력을 버퍼에 쌓고, 정지하면 캡처된 클립을 돌려준다.
    // 길이 제한은 사실상 없다: GUI가 pumpRecording()을 매 프레임 불러
    // 다음 버퍼 청크를 미리 할당해 준다 (오디오 스레드는 할당하지 않음).
    virtual void startRecording() = 0;
    virtual void pumpRecording() = 0; // 녹음 중 매 프레임 호출 (아니면 무동작)
    virtual std::shared_ptr<AudioClip> stopRecording() = 0; // 없으면 null
    virtual bool isRecording() const = 0;

    // 입력 레벨(0~1 최근 피크) — 화면 미터 표시용
    virtual float inputLevel() const = 0;

    // ---- ASIO (저지연 듀플렉스) ----
    virtual bool asioAvailable() const = 0;              // ASIO 드라이버가 하나라도 있나
    virtual std::vector<std::string> listAsioDevices() = 0;
    // ASIO 듀플렉스 시작: 입출력을 한 장치로 열어 저지연 모니터. 채널모드 0/1/2.
    virtual bool startAsio(int deviceIndex, int channelMode) = 0;
    virtual void stopAsio() = 0;
    virtual bool asioActive() const = 0;
};

} // namespace midipro::audio
