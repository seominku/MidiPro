#pragma once
// =============================================================
// MidiPro - audio/AudioClip.h
// 트랙에 붙는 오디오 클립(디코드된 PCM) + 오디오 스레드용 믹스 스냅샷.
//
// 왜 이렇게 나눴는가 (Rule 1, 3):
//   AudioClip은 GUI/시퀀서가 다루는 순수 데이터(PCM + 배치 정보)다.
//   오디오 스레드는 AudioMixClip 스냅샷(수명을 shared_ptr로 잡은 뷰)만
//   읽는다. 콜백에서 할당/락 없이 믹싱한다.
// =============================================================

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midipro::audio {

// 디코드된 오디오 한 조각 (인터리브 PCM).
struct AudioClip {
    std::string name;
    std::vector<float> pcm; // 인터리브 (channels로 나눔)
    int channels = 2;
    int sampleRate = 44100;
    uint32_t startTick = 0; // 타임라인 상 시작 위치(틱)
    double speed = 1.0;     // 재생 배속(>1 빠름·짧음, <1 느림·김). 2단계 스트레치용.

    // 트림(재생 구간): 소스 프레임 오프셋 + 타임라인상 길이(소스 프레임 단위).
    // trimLen이 소스 길이보다 크면 뒤에 공백(무음)이 붙는다. 속도는 그대로.
    int64_t trimStart = 0;
    int64_t trimLen = 0; // 0이면 전체 길이로 간주 (decode 시 frames()로 설정)

    // 페이드 인/아웃 (초, 타임라인 기준). 0이면 없음. 클릭음 제거·자연스러운 시작/끝.
    double fadeInSec = 0.0;
    double fadeOutSec = 0.0;

    // 클립 게인 (트랙 페이더와 별개, 곱해진다). 녹음 볼륨이 들쭉날쭉한
    // 클립들을 트랙 볼륨을 건드리지 않고 개별로 맞출 때 쓴다.
    float gain = 1.0f;

    // 트랙 프리즈로 구워진 클립 표시. 프리즈 해제 때 이 클립들만 지운다.
    bool freezeBounce = false;

    int64_t playLen() const { return trimLen > 0 ? trimLen : (int64_t)frames(); }

    // 타임라인상 재생 길이(초). 배속이 빠를수록 짧아진다.
    double durationSeconds() const {
        if (sampleRate <= 0 || speed <= 0.0) return 0.0;
        return (double)playLen() / ((double)sampleRate * speed);
    }

    // durationSec 만큼 재생되게 하려면 필요한 배속. 타임스트레치 드래그에 쓴다.
    // (durationSeconds()의 역함수) 유효하지 않으면 현재 speed를 그대로 돌려준다.
    double speedForDuration(double durationSec) const {
        if (sampleRate <= 0 || durationSec <= 1e-9) return speed;
        return (double)playLen() / ((double)sampleRate * durationSec);
    }

    // 파형 표시용 피크 엔벨로프 (peakStride 소스 프레임마다 min/max 한 쌍).
    // 임포트 시 한 번 계산해 두어, 그릴 때 원본을 다 훑지 않게 한다.
    std::vector<float> peakMin;
    std::vector<float> peakMax;
    int peakStride = 512;

    std::size_t frames() const { return channels > 0 ? pcm.size() / (std::size_t)channels : 0; }

    void buildPeaks() {
        const std::size_t n = frames();
        peakMin.clear();
        peakMax.clear();
        for (std::size_t start = 0; start < n; start += (std::size_t)peakStride) {
            float lo = 1.0f, hi = -1.0f;
            const std::size_t end = std::min(n, start + (std::size_t)peakStride);
            for (std::size_t f = start; f < end; ++f) {
                const float s = channels == 2 ? 0.5f * (pcm[f * 2] + pcm[f * 2 + 1]) : pcm[f];
                if (s < lo) lo = s;
                if (s > hi) hi = s;
            }
            peakMin.push_back(lo);
            peakMax.push_back(hi);
        }
    }

    // 소스 프레임 위치 f(소수)의 좌/우 샘플 (선형보간, 모노는 양쪽 동일).
    void sampleAt(double f, float& l, float& r) const {
        const std::size_t n = frames();
        if (f < 0.0 || n == 0) { l = r = 0.0f; return; }
        const std::size_t i = (std::size_t)f;
        if (i + 1 >= n) { l = r = 0.0f; return; }
        const float frac = (float)(f - (double)i);
        if (channels == 2) {
            l = pcm[i * 2] * (1 - frac) + pcm[(i + 1) * 2] * frac;
            r = pcm[i * 2 + 1] * (1 - frac) + pcm[(i + 1) * 2 + 1] * frac;
        } else {
            const float s = pcm[i] * (1 - frac) + pcm[i + 1] * frac;
            l = r = s;
        }
    }
};

// 오디오 스레드가 읽는 믹스 항목 (스냅샷). shared_ptr로 PCM 수명 보장.
struct AudioMixClip {
    std::shared_ptr<const AudioClip> clip;
    int64_t startFrame = 0;       // 엔진 샘플 기준 타임라인 위치
    double srcPerEngine = 1.0;    // clip.sampleRate/engineRate * speed (엔진 1프레임당 소스 프레임)
    int64_t sourceOffset = 0;     // 소스 읽기 시작 프레임(trimStart)
    int64_t lengthSrcFrames = 0;  // 타임라인상 길이(소스 프레임). 넘으면 클립 끝(공백)
    float gainL = 1.0f;
    float gainR = 1.0f;
    bool muted = false;
    int bus = 0; // 트랙 버스(= 트랙의 MIDI 채널 0~15). 트랙별 이펙트 체인이 걸린다.
    // 페이드 (엔진 프레임 단위). 시작 fadeInFrames 동안 0->1, 끝 fadeOutFrames 동안 1->0.
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;
    int64_t lengthEngFrames = 0; // 타임라인상 길이(엔진 프레임) — 페이드아웃 기준점
};

// MP3 바이트를 디코드해 AudioClip을 만든다. 실패 시 nullptr.
std::shared_ptr<AudioClip> decodeMp3(const uint8_t* data, std::size_t size, const std::string& name);

// FLAC 바이트 디코드 (dr_flac). 실패 시 nullptr.
std::shared_ptr<AudioClip> decodeFlac(const uint8_t* data, std::size_t size,
                                      const std::string& name);

// 매직 넘버로 형식을 자동 판별해 디코드: RIFF=WAV, fLaC=FLAC, 그 외 MP3 시도.
std::shared_ptr<AudioClip> decodeAudioAuto(const uint8_t* data, std::size_t size,
                                           const std::string& name);

// 클립을 타임라인 기준 atSec 지점에서 둘로 자른다 (가위 도구).
// left(인자)는 그 지점까지로 줄고, 오른쪽 조각을 새 클립으로 돌려준다.
// 왼쪽은 pcm을 그대로 갖고 있어 나중에 오른쪽 끝을 다시 늘릴 수 있고,
// 오른쪽은 분할점부터 소스 끝까지 pcm을 복사해 독립적으로 편집된다.
// startTick은 호출자가 설정한다. 못 자르는 지점이면 nullptr.
std::shared_ptr<AudioClip> splitClipAt(AudioClip& left, double atSec);

// 클립 병합: 여러 클립을 타임라인 배치 그대로 하나로 굽는다.
// 사이 공백은 무음, 겹치면 더해진다. 각 클립의 트림/배속/게인/페이드가
// 결과 PCM에 구워지고, 결과 클립은 게인 1/배속 1/페이드 0의 평범한 클립이다.
// startSec = 각 클립의 절대 초(템포 맵 반영, 호출자가 계산). 실패 시 nullptr.
struct MergeItem {
    const AudioClip* clip = nullptr;
    double startSec = 0.0;
};
std::shared_ptr<AudioClip> mergeClips(const std::vector<MergeItem>& items, int outRate);

// 리버스: 재생 구간(트림 반영)을 뒤집은 "새 클립"을 돌려준다 (심벌 스웰 효과 등).
// 배속/게인은 유지되고, 페이드 인/아웃은 서로 자리를 바꾼다. 실패 시 nullptr.
std::shared_ptr<AudioClip> reverseClip(const AudioClip& src);

// 음정 유지 타임스트레치 (WSOLA). ratio = 출력길이/입력길이 (0.5=절반, 2=두 배).
// 재생 구간(트림 반영)을 늘이거나 줄인 "새 클립"을 돌려준다 (원본 불변 ->
// 언두는 트랙의 클립 포인터 교체로 복원된다). 너무 짧으면 nullptr.
// 배속(speed)과 달리 음높이가 변하지 않는다. 적당한 범위(0.5~2)에서 자연스럽다.
std::shared_ptr<AudioClip> stretchClipPitchPreserve(const AudioClip& src, double ratio);

// 정규화: 재생 구간(트림 반영)의 샘플 피크가 targetPeak가 되는 게인을 돌려준다.
// 무음이면 1.0. 기본 목표는 -1dB(≈0.891) — 0dB로 붙이면 재생 중 보간/속도
// 변화로 순간 클리핑이 날 수 있어 여유를 둔다.
float normalizeGainFor(const AudioClip& clip, float targetPeak = 0.891f);

} // namespace midipro::audio
