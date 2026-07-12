#pragma once
// =============================================================
// MidiPro - audio/IAudioClips.h
// 오디오 클립(임포트한 오디오) 재생 제어 인터페이스 (Rule 1).
//
// GUI가 트랜스포트에 맞춰 오디오 클립을 재생/정지/시크하고, 트랙별
// 볼륨/팬이 반영된 믹스 스냅샷을 엔진에 넘긴다. RtAudioEngine이 구현.
// =============================================================

#include "audio/AudioClip.h"

#include <memory>
#include <vector>

namespace midipro::audio {

class BuiltinFx; // 내장 이펙트 (audio/BuiltinFx.h) — 마스터 리미터 파라미터 접근용

class IAudioClips {
public:
    virtual ~IAudioClips() = default;

    // 믹싱할 클립 목록(스냅샷)을 원자적으로 교체한다. 오디오 스레드는
    // 다음 콜백부터 새 목록을 읽는다.
    virtual void setAudioMix(std::shared_ptr<const std::vector<AudioMixClip>> clips) = 0;

    // 트랜스포트 동기화: startFrame(엔진 샘플)에서 오디오 시작/정지/시크.
    virtual void startAudio(int64_t startFrame) = 0;
    virtual void stopAudio() = 0;
    virtual void seekAudio(int64_t frame) = 0;

    // 틱->샘플 환산에 필요한 현재 엔진 샘플레이트.
    virtual double engineSampleRate() const = 0;

    // 연습 모드: 오디오 입력(기타)의 최근 샘플을 읽는다 (모노, 최신 n개).
    // GUI 스레드가 피치 분석용으로 폴링한다. 입력이 없으면 0을 반환.
    virtual int readInputTap(float* dst, int n) {
        (void)dst;
        (void)n;
        return 0;
    }
    // 입력 장치의 버퍼 지연(초). 연습 판정에서 타격 시각을 이만큼 앞으로 당긴다.
    // (일반 WASAPI 입력은 100ms를 훌쩍 넘을 수 있다 — 보정 없으면 판정이 늦게 잡힌다)
    virtual double inputLatencySeconds() const { return 0.0; }
    // 입력 스트림의 누적 샘플 수 (쓰기 커서). GUI가 읽기 커서를 유지하며
    // "빠짐없이" 처리할 수 있게 한다 (프레임 드랍에도 어택을 놓치지 않는다).
    virtual uint32_t inputTapWritePos() const { return 0; }
    // [from, from+n) 구간을 읽는다. 아직 안 쓰였거나 이미 덮였으면 false.
    virtual bool readInputTapRange(uint32_t from, float* dst, int n) {
        (void)from;
        (void)dst;
        (void)n;
        return false;
    }

    // 최종 출력 마스터 게인(신스+VST+클립+모니터 전부에 적용). 오디오 스레드 안전.
    virtual void setMasterGain(float gain) = 0;
    // 최종 출력 마스터 팬 (-1 좌 ~ +1 우). 오디오 스레드 안전.
    virtual void setMasterPan(float pan) = 0;

    // ---- 마스터 리미터 (클리핑 방지, 볼륨/팬 뒤 최종 단계) ----
    virtual void setMasterLimiter(bool on) { (void)on; }
    virtual bool masterLimiterOn() const { return false; }
    // 파라미터(게인/실링/릴리스)와 게인 감소량 표시용. atomic이라 GUI에서 안전.
    virtual BuiltinFx* masterLimiter() { return nullptr; }

    // ASIO 모니터 입력을 이 트랙 버스로 (체인 앞) — 모니터에 트랙 FX가 걸린다.
    // -1 = 마스터 직행 (이펙트 없음). 녹음 파일은 언제나 드라이 원본.
    virtual void setMonitorBus(int bus) { (void)bus; }

    // ---- 센드/리턴 버스 (공용 리버브 1개, 트랙별 Send 노브) ----
    virtual void setBusSend(int bus, float level) { (void)bus; (void)level; }
    virtual void setReturnLevel(float level) { (void)level; }
    virtual float returnLevel() const { return 1.0f; }
    virtual BuiltinFx* returnReverb() { return nullptr; } // 공간/댐핑 파라미터

    // ---- 메트로놈/카운트인 클릭 소리 (일반/강조 각각) ----
    // 클릭은 채널 10의 지정 노트로 온다. 그 노트에 샘플이 설정돼 있으면
    // 신스 대신 샘플(원샷)을 재생한다. null이면 신스 음으로 되돌린다.
    static constexpr int kClickMetro = 0;         // 메트로놈 일반
    static constexpr int kClickCountIn = 1;       // 카운트인 일반
    static constexpr int kClickAccent = 2;        // 메트로놈 강조(다운비트)
    static constexpr int kClickCountInAccent = 3; // 카운트인 강조(첫 클릭)
    static constexpr int kClickKinds = 4;
    virtual void setClickPitches(uint8_t metroNote, uint8_t countInNote, uint8_t accentNote,
                                 uint8_t countInAccentNote) = 0;
    virtual void setClickSample(int kind, std::shared_ptr<const AudioClip> clip) = 0;

    // ---- 드럼 샘플: 채널 10의 지정 노트를 WAV 원샷으로 소리낸다 ----
    // null = 내장 드럼 신스로 복귀. 트랙 버스를 타서 볼륨/EQ/FX가 걸린다.
    virtual void setDrumSample(uint8_t note, std::shared_ptr<const AudioClip> clip) {
        (void)note;
        (void)clip;
    }

    // ---- 오프라인 렌더 (내보내기: 재생 없이 GUI 스레드가 구간을 직접 렌더) ----
    // begin: 스트림을 멈추고 오디오 클립 재생 위치를 startFrame으로 둔다.
    // queueMidi: 렌더 블록 사이에 노트 이벤트를 밀어 넣는다.
    // renderOfflineBlock: 신스/VSTi/클립/체인/마스터를 거친 최종 스테레오 인터리브 출력.
    // end: 스트림을 원래대로 복구한다.
    virtual void beginOfflineRender(int64_t startFrame) = 0;
    virtual bool queueMidi(uint8_t status, uint8_t data1, uint8_t data2) = 0;
    virtual void renderOfflineBlock(float* interleavedOut, unsigned frames) = 0;
    // 한 버스의 출력만 뽑는다 (마스터 이펙트/게인 미적용).
    // preFx=true : 악기 출력만 (트랙 프리즈용 — FX는 재생 때 걸린다)
    // preFx=false: FX 체인까지 거친 소리 (스템 내보내기용)
    virtual void renderOfflineBlockBus(int bus, float* interleavedOut, unsigned frames,
                                       bool preFx) = 0;
    // 센드/리턴 버스(공용 리버브)의 출력만 뽑는다 (리버브 스템 내보내기용)
    virtual void renderOfflineBlockReturn(float* interleavedOut, unsigned frames) {
        for (unsigned i = 0; i < frames * 2; ++i) interleavedOut[i] = 0.0f;
    }
    virtual void endOfflineRender() = 0;

    // 트랙 프리즈: 이 버스의 VST 악기/이펙트 처리를 통째로 건너뛴다 (CPU 절약).
    // 구운 클립은 이미 FX가 반영돼 있어 체인을 다시 탈 필요가 없다.
    virtual void setBusFrozen(int bus, bool frozen) = 0;

    // ---- 레벨 미터 (믹서 표시용, GUI가 매 프레임 폴링) ----
    // 마지막 조회 이후의 샘플 피크(절댓값 최대)를 돌려주고 0으로 리셋한다.
    // 오디오 스레드는 max-hold로만 쓰므로 GUI가 프레임 사이 피크를 놓치지 않는다.
    // 마스터는 소프트 클립 "직전" 값이라 1.0 초과 = 클리핑을 뜻한다.
    virtual float pollBusPeak(int bus) = 0;                     // 트랙 버스(0~15), 포스트 FX·페이더
    virtual void pollMasterPeak(float& outL, float& outR) = 0;  // 최종 출력 L/R

    // ---- 성능 표시 ----
    // 오디오 콜백 부하: 처리 시간 / 버퍼 시간 (0~1, 1에 가까우면 끊김 위험).
    virtual float audioLoad() const = 0;
    virtual unsigned engineBufferFrames() const = 0; // 현재 스트림 버퍼 크기

    // ---- 마스터 캡처 (WAV 내보내기용) ----
    // 시작하면 최종 출력(마스터 볼륨/팬/이펙트 반영, 스피커로 나가는 그대로)을
    // 버퍼에 쌓는다. 정지하면 캡처된 스테레오 클립을 돌려준다 (없으면 null).
    virtual void startMasterCapture(double maxSeconds) = 0;
    virtual std::shared_ptr<AudioClip> stopMasterCapture() = 0;
    virtual bool masterCapturing() const = 0;
};

} // namespace midipro::audio
