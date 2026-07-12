#pragma once
// =============================================================
// MidiPro - audio/BuiltinFx.h
// 내장 트랙 이펙트: EQ(3밴드) / 딜레이 / 리버브.
//
// 왜 내장인가: VST 플러그인이 없어도 기본 믹싱이 가능해야 해서.
// 엔진의 트랙 이펙트 체인(TrackEffect)에 VST와 나란히 끼워지며,
// 처리 순서·바이패스·순서 변경 규칙을 그대로 따른다.
//
// 스레딩 (Rule 3):
//   파라미터는 atomic — GUI 스레드가 setParam, 오디오 스레드가 process에서
//   읽는다. 버퍼는 생성 시 최대 샘플레이트(192kHz) 기준으로 선할당하므로
//   오디오 스레드에서는 할당이 없다.
// =============================================================

#include <atomic>
#include <cstdint>
#include <vector>

namespace midipro::audio {

class BuiltinFx {
public:
    // 종류 (프로젝트 저장 토큰과 1:1 — 순서를 바꾸면 안 된다)
    static constexpr int kEq = 0;
    static constexpr int kDelay = 1;
    static constexpr int kReverb = 2;
    static constexpr int kLimiter = 3;    // 피크 리미터 (마스터 클리핑 방지용)
    static constexpr int kCompressor = 4; // 컴프레서 (다이내믹 정리)
    static constexpr int kTypes = 5;

    static constexpr int kNumParams = 5;

    // 파라미터 정의 (GUI 슬라이더 라벨/범위). label이 nullptr면 미사용 슬롯.
    struct ParamDesc {
        const char* label;
        float min, max, def;
        bool log;        // 로그 스케일 슬라이더 (주파수 등)
        const char* fmt; // "%.1f dB" 등
    };
    static const char* typeName(int type);            // "EQ" / "딜레이" / "리버브"
    static const char* typeToken(int type);           // "eq" / "delay" / "reverb" (저장용)
    static int typeFromToken(const char* token);      // 못 찾으면 -1
    static const ParamDesc* paramDescs(int type);     // kNumParams개 배열

    explicit BuiltinFx(int type);

    int type() const { return m_type; }
    float param(int i) const;
    void setParam(int i, float v); // 범위로 클램프

    // 제자리 스테레오 처리. ch2 = {L, R} (frames 샘플씩).
    // 샘플레이트가 바뀌면 내부 상태를 (할당 없이) 다시 맞춘다.
    void process(float* const* ch2, int frames, double sampleRate);

    // 사이드체인 컴프레션: 게인 감소량을 자기 입력이 아니라 key 신호(L/R)의
    // 피크로 계산한다 (킥으로 베이스 덕킹 등). 컴프레서 타입 전용 —
    // 다른 타입이면 일반 process와 동일하게 동작한다.
    void processSidechain(float* const* ch2, const float* keyL, const float* keyR,
                          int frames, double sampleRate);

    // 리미터/컴프레서: 마지막 블록의 최대 게인 감소량(dB, 0=감소 없음). GUI 미터용.
    float gainReductionDb() const { return m_grDb.load(std::memory_order_relaxed); }

    // EQ용 RBJ 바이쿼드 (계수 계산 헬퍼가 파일 밖에 있어 public)
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0; // Direct Form II transposed 상태
        float run(float x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float)y;
        }
    };

private:
    void refresh(double sr); // 파라미터/샘플레이트 변경 반영 (계수·길이 재계산)

    int m_type = kEq;
    std::atomic<float> m_params[kNumParams];
    std::atomic<bool> m_dirty{true};
    double m_sr = 0.0;

    Biquad m_eq[3][2]; // [밴드][채널]

    // ---- 딜레이: 채널별 링 버퍼 (최대 2초 @192kHz 선할당) ----
    std::vector<float> m_dbuf[2];
    std::size_t m_dpos = 0;
    int m_dlen = 1; // 현재 딜레이 샘플 수

    // ---- 리버브: Freeverb 축약형 (콤 4개 + 올패스 2개) x 좌우 ----
    static constexpr int kCombs = 4;
    static constexpr int kAllpasses = 2;
    std::vector<float> m_comb[kCombs][2];
    int m_combLen[kCombs][2] = {};
    int m_combPos[kCombs][2] = {};
    float m_combLp[kCombs][2] = {}; // 댐핑 로우패스 상태
    std::vector<float> m_ap[kAllpasses][2];
    int m_apLen[kAllpasses][2] = {};
    int m_apPos[kAllpasses][2] = {};
    float m_combFeedback = 0.84f;
    float m_damp = 0.4f;

    // ---- 리미터 ----
    float m_limEnv = 0.0f;      // 피크 엔벨로프 (즉시 어택, 릴리스는 지수 감쇠)
    float m_limRelCoef = 0.0f;  // 릴리스 계수 (refresh에서 계산)
    float m_limGain = 1.0f;     // 입력 게인 (선형)
    float m_limCeil = 1.0f;     // 실링 (선형)
    std::atomic<float> m_grDb{0.0f}; // 블록 최대 감소량 (GUI 표시)

    // ---- 컴프레서 ----
    float m_cmpEnv = 0.0f;     // 피크 엔벨로프 (어택/릴리스 평활)
    float m_cmpAttCoef = 1.0f; // 어택 계수
    float m_cmpRelCoef = 0.0f; // 릴리스 계수
    float m_cmpThrDb = -18.0f; // 스레숄드 (dB)
    float m_cmpSlope = 0.75f;  // 1 - 1/비율
    float m_cmpMakeup = 1.0f;  // 메이크업 게인 (선형)
};

} // namespace midipro::audio
