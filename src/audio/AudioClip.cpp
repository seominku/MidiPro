// =============================================================
// MidiPro - audio/AudioClip.cpp
// dr_mp3/dr_flac으로 MP3/FLAC을 float PCM으로 디코드한다 (WAV는 WavFile).
// =============================================================

#include "audio/AudioClip.h"

#include "audio/WavFile.h" // decodeWav (자동 판별용)

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO // 파일 경로 대신 메모리에서 디코드 (한글 경로 문제 회피)
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

#include <cstring>

namespace midipro::audio {

std::shared_ptr<AudioClip> decodeMp3(const uint8_t* data, std::size_t size,
                                     const std::string& name) {
    if (!data || size == 0) return nullptr;

    drmp3_config config{};
    drmp3_uint64 frameCount = 0;
    float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &frameCount,
                                                           nullptr);
    if (!pcm || frameCount == 0) {
        if (pcm) drmp3_free(pcm, nullptr);
        return nullptr;
    }

    auto clip = std::make_shared<AudioClip>();
    clip->name = name;
    clip->channels = (int)config.channels;
    clip->sampleRate = (int)config.sampleRate;
    clip->pcm.assign(pcm, pcm + (std::size_t)frameCount * config.channels);
    drmp3_free(pcm, nullptr);
    clip->trimStart = 0;
    clip->trimLen = (int64_t)clip->frames(); // 처음엔 전체 재생
    clip->buildPeaks();                       // 파형 표시용 피크 미리 계산
    return clip;
}

std::shared_ptr<AudioClip> decodeFlac(const uint8_t* data, std::size_t size,
                                      const std::string& name) {
    if (!data || size == 0) return nullptr;
    unsigned int ch = 0, rate = 0;
    drflac_uint64 frameCount = 0;
    float* pcm =
        drflac_open_memory_and_read_pcm_frames_f32(data, size, &ch, &rate, &frameCount, nullptr);
    if (!pcm || frameCount == 0 || ch == 0) {
        if (pcm) drflac_free(pcm, nullptr);
        return nullptr;
    }
    auto clip = std::make_shared<AudioClip>();
    clip->name = name;
    clip->channels = (int)ch;
    clip->sampleRate = (int)rate;
    clip->pcm.assign(pcm, pcm + (std::size_t)frameCount * ch);
    drflac_free(pcm, nullptr);
    clip->trimStart = 0;
    clip->trimLen = (int64_t)clip->frames();
    clip->buildPeaks();
    return clip;
}

std::shared_ptr<AudioClip> decodeAudioAuto(const uint8_t* data, std::size_t size,
                                           const std::string& name) {
    if (!data || size < 4) return nullptr;
    if (std::memcmp(data, "RIFF", 4) == 0) { // WAV
        auto clip = decodeWav(data, size, name);
        if (clip) {
            if (clip->trimLen == 0) clip->trimLen = (int64_t)clip->frames();
            if (clip->peakMax.empty()) clip->buildPeaks();
        }
        return clip;
    }
    if (std::memcmp(data, "fLaC", 4) == 0) return decodeFlac(data, size, name);
    return decodeMp3(data, size, name); // MP3는 헤더가 다양해 마지막 폴백
}

std::shared_ptr<AudioClip> splitClipAt(AudioClip& left, double atSec) {
    if (left.sampleRate <= 0 || left.speed <= 0.0) return nullptr;
    const double totalSec = left.durationSeconds();
    // 너무 끝(10ms 이내)이면 자르지 않는다 (조각이 무의미)
    if (atSec <= 0.01 || atSec >= totalSec - 0.01) return nullptr;

    const int64_t srcOff = (int64_t)(atSec * left.sampleRate * left.speed); // 분할점(소스 프레임)
    if (srcOff <= 0 || srcOff >= left.playLen()) return nullptr;

    const std::size_t ch = (std::size_t)(left.channels > 0 ? left.channels : 1);
    const std::size_t beginFrame = (std::size_t)(left.trimStart + srcOff);
    if (beginFrame * ch >= left.pcm.size()) return nullptr;

    auto right = std::make_shared<AudioClip>();
    right->name = left.name;
    right->channels = left.channels;
    right->sampleRate = left.sampleRate;
    right->speed = left.speed;
    right->gain = left.gain; // 클립 게인은 양쪽 조각이 그대로 유지
    // 분할점부터 소스 "끝까지" 복사해 두면 오른쪽 조각의 끝도 나중에 늘릴 수 있다
    right->pcm.assign(left.pcm.begin() + (std::ptrdiff_t)(beginFrame * ch), left.pcm.end());
    right->trimStart = 0;
    right->trimLen = left.playLen() - srcOff;
    right->fadeInSec = 0.0;
    right->fadeOutSec = left.fadeOutSec; // 끝 페이드는 오른쪽 조각이 가져간다
    right->buildPeaks();

    left.trimLen = srcOff; // 왼쪽은 분할점까지 (pcm은 유지 -> 다시 늘릴 수 있음)
    left.fadeOutSec = 0.0;
    return right;
}

std::shared_ptr<AudioClip> reverseClip(const AudioClip& src) {
    if (src.sampleRate <= 0 || src.channels < 1) return nullptr;
    const int ch = src.channels;
    const int64_t total = (int64_t)src.frames();
    const int64_t begin = std::clamp<int64_t>(src.trimStart, 0, total);
    const int64_t len = std::min<int64_t>(src.playLen(), total - begin);
    if (len < 2) return nullptr;

    auto out = std::make_shared<AudioClip>();
    out->name = src.name;
    out->channels = ch;
    out->sampleRate = src.sampleRate;
    out->startTick = src.startTick;
    out->speed = src.speed;
    out->gain = src.gain;
    out->fadeInSec = src.fadeOutSec; // 뒤집으면 페이드도 자리가 바뀐다
    out->fadeOutSec = src.fadeInSec;
    out->freezeBounce = false;
    out->pcm.resize((std::size_t)len * (std::size_t)ch);
    for (int64_t f = 0; f < len; ++f)
        for (int c = 0; c < ch; ++c)
            out->pcm[(std::size_t)f * ch + c] =
                src.pcm[(std::size_t)(begin + (len - 1 - f)) * ch + c];
    out->trimStart = 0;
    out->trimLen = len;
    out->buildPeaks();
    return out;
}

std::shared_ptr<AudioClip> mergeClips(const std::vector<MergeItem>& items, int outRate) {
    if (items.empty() || outRate <= 0) return nullptr;
    double base = 1e300, end = 0.0;
    for (const auto& it : items) {
        if (!it.clip || it.clip->sampleRate <= 0) continue;
        base = std::min(base, it.startSec);
        end = std::max(end, it.startSec + it.clip->durationSeconds());
    }
    if (!(end > base)) return nullptr;
    const int64_t outFrames = (int64_t)((end - base) * outRate + 0.5);
    if (outFrames < 1) return nullptr;

    auto out = std::make_shared<AudioClip>();
    out->channels = 2;
    out->sampleRate = outRate;
    out->speed = 1.0;
    out->pcm.assign((std::size_t)outFrames * 2, 0.0f);

    for (const auto& it : items) {
        if (!it.clip || it.clip->sampleRate <= 0) continue;
        const AudioClip& c = *it.clip;
        const double durSec = c.durationSeconds();
        const int64_t startF = (int64_t)((it.startSec - base) * outRate + 0.5);
        const int64_t lenF = (int64_t)(durSec * outRate);
        // 페이드는 엔진 재생과 같게 사용자 값과 3ms 디클릭 중 큰 쪽을 굽는다
        const double fi = std::max(c.fadeInSec, 0.003);
        const double fo = std::max(c.fadeOutSec, 0.003);
        for (int64_t f = 0; f < lenF && startF + f < outFrames; ++f) {
            if (startF + f < 0) continue;
            const double tSec = (double)f / outRate;
            const double srcF = (double)c.trimStart + tSec * c.sampleRate * c.speed;
            float l = 0.0f, r = 0.0f;
            c.sampleAt(srcF, l, r);
            float g = c.gain;
            if (tSec < fi) g *= (float)(tSec / fi);
            const double rem = durSec - tSec;
            if (rem < fo) g *= (float)(std::max(0.0, rem) / fo);
            out->pcm[(std::size_t)(startF + f) * 2 + 0] += l * g;
            out->pcm[(std::size_t)(startF + f) * 2 + 1] += r * g;
        }
    }

    out->trimStart = 0;
    out->trimLen = (int64_t)out->frames();
    out->buildPeaks();
    return out;
}

std::shared_ptr<AudioClip> stretchClipPitchPreserve(const AudioClip& src, double ratio) {
    if (src.sampleRate <= 0 || src.channels < 1) return nullptr;
    ratio = std::clamp(ratio, 0.25, 4.0);
    const int ch = src.channels;
    const int64_t total = (int64_t)src.frames();
    const int64_t begin = std::clamp<int64_t>(src.trimStart, 0, total);
    const int64_t inLen = std::min<int64_t>(src.playLen(), total - begin);
    if (inLen < 2048) return nullptr; // 너무 짧으면 스트레치가 의미 없다

    // WSOLA 파라미터: ~60ms 프레임을 절반씩 겹쳐 붙이고, 이음새는 ±10ms
    // 탐색창에서 이전 꼬리와 파형이 가장 비슷한 지점을 골라 클릭음을 줄인다.
    const int N = std::max(1024, src.sampleRate * 6 / 100); // 프레임 길이
    const int L = N / 2;                                     // 겹침(크로스페이드)
    const int Hs = N - L;                                    // 출력 홉
    const double Ha = (double)Hs / ratio;                    // 입력 홉
    const int W = std::max(64, src.sampleRate / 100);        // 탐색 반경

    const int64_t outLen = (int64_t)((double)inLen * ratio);
    if (outLen < N) return nullptr;

    auto out = std::make_shared<AudioClip>();
    out->name = src.name;
    out->channels = ch;
    out->sampleRate = src.sampleRate;
    out->startTick = src.startTick;
    out->speed = 1.0; // 음정 유지: 배속이 아니라 파형 재배열
    out->gain = src.gain;
    out->fadeInSec = src.fadeInSec;
    out->fadeOutSec = src.fadeOutSec;
    out->freezeBounce = false;
    out->pcm.assign((std::size_t)outLen * (std::size_t)ch, 0.0f);

    // 탐색용 모노 신호 (채널 평균)
    std::vector<float> mono((std::size_t)inLen);
    for (int64_t f = 0; f < inLen; ++f) {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c) s += src.pcm[(std::size_t)(begin + f) * ch + c];
        mono[(std::size_t)f] = s / (float)ch;
    }
    const auto monoAt = [&](int64_t f) {
        return (f >= 0 && f < inLen) ? mono[(std::size_t)f] : 0.0f;
    };
    const auto inSample = [&](int64_t f, int c) {
        return (f >= 0 && f < inLen) ? src.pcm[(std::size_t)(begin + f) * ch + c] : 0.0f;
    };

    // 첫 프레임은 입력 시작을 그대로 복사
    for (int64_t f = 0; f < N && f < outLen; ++f)
        for (int c = 0; c < ch; ++c)
            out->pcm[(std::size_t)f * ch + c] = inSample(f, c);

    int64_t prevIn = 0;   // 직전 프레임의 입력 위치 (자연스러운 이어짐 기준)
    int64_t outPos = Hs;  // 다음 프레임을 쓸 출력 위치
    double inPosF = Ha;   // 다음 프레임의 명목 입력 위치
    while (outPos < outLen) {
        const int64_t target = (int64_t)inPosF;
        const int64_t natural = prevIn + Hs; // 그냥 이어 읽으면 여기가 나온다
        int64_t best = std::clamp<int64_t>(target, 0, std::max<int64_t>(0, inLen - N));
        // 정규화 교차상관: 에너지가 큰 후보가 무조건 이기지 않게 에너지로 나눈다
        // (그냥 내적이면 큰 소리 쪽으로 끌려가 위상이 어긋나 클릭이 생긴다)
        float bestScore = -1e30f;
        for (int64_t cand = target - W; cand <= target + W; cand += 2) {
            if (cand < 0 || cand + N > inLen) continue;
            float dot = 0.0f, eCand = 1e-9f;
            for (int k = 0; k < L; k += 2) {
                const float a = monoAt(natural + k);
                const float b = monoAt(cand + k);
                dot += a * b;
                eCand += b * b;
            }
            const float score = dot / std::sqrt(eCand);
            if (score > bestScore) {
                bestScore = score;
                best = cand;
            }
        }
        // 겹침 구간은 등파워(사인/코사인) 크로스페이드 — 선형보다 이음새가 매끄럽다
        for (int f = 0; f < N && outPos + f < outLen; ++f) {
            for (int c = 0; c < ch; ++c) {
                const float v = inSample(best + f, c);
                const std::size_t oi = (std::size_t)(outPos + f) * ch + c;
                if (f < L) {
                    const float t = (float)f / (float)L;
                    const float wIn = std::sin(t * 1.57079632679f);  // 새 프레임
                    const float wOut = std::cos(t * 1.57079632679f); // 이전 꼬리
                    out->pcm[oi] = out->pcm[oi] * wOut + v * wIn;
                } else {
                    out->pcm[oi] = v;
                }
            }
        }
        prevIn = best;
        outPos += Hs;
        inPosF += Ha;
    }

    out->trimStart = 0;
    out->trimLen = (int64_t)out->frames();
    out->buildPeaks();
    return out;
}

float normalizeGainFor(const AudioClip& clip, float targetPeak) {
    const int64_t n = (int64_t)clip.frames();
    const int ch = clip.channels > 0 ? clip.channels : 1;
    // 들리는 구간만 잰다: 트림 밖의 큰 소리 때문에 덜 키워지는 것을 막는다
    const int64_t begin = std::clamp<int64_t>(clip.trimStart, 0, n);
    const int64_t end = std::min<int64_t>(n, begin + clip.playLen());
    float peak = 0.0f;
    for (int64_t f = begin; f < end; ++f) {
        for (int c = 0; c < ch; ++c) {
            const float s = clip.pcm[(std::size_t)f * ch + (std::size_t)c];
            const float a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
    }
    if (peak <= 0.000001f || targetPeak <= 0.0f) return 1.0f; // 무음은 건드리지 않는다
    return targetPeak / peak;
}

} // namespace midipro::audio
