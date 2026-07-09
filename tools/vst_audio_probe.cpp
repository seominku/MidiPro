// Vst3Host 실오디오 검증: 악기를 인스턴스화하고 노트를 넣어 소리(음량)가
// 나는지, 이펙트가 입력을 통과/처리하는지 측정한다.
#include "vst/Vst3Host.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace midipro::vst;

static double rms(const std::vector<float>& L, const std::vector<float>& R, int frames) {
    double s = 0.0;
    for (int i = 0; i < frames; ++i) s += (double)L[i] * L[i] + (double)R[i] * R[i];
    return std::sqrt(s / (2.0 * frames));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("사용법: vst_audio_probe <악기.vst3> [이펙트.vst3]\n");
        return 2;
    }
    const int SR = 44100, BLK = 512;
    std::vector<float> L(BLK, 0.f), R(BLK, 0.f);
    float* planar[2] = {L.data(), R.data()};

    // ---- 악기 ----
    {
        Vst3Host host;
        std::string err;
        if (!host.loadModule(argv[1], err)) { std::printf("악기 로드 실패: %s\n", err.c_str()); return 1; }
        if (!host.instantiate(0, SR, BLK, err)) { std::printf("악기 인스턴스화 실패: %s\n", err.c_str()); return 1; }
        std::printf("악기 인스턴스화 OK: %s (isInstrument=%d)\n", host.activeName().c_str(),
                    host.isInstrument() ? 1 : 0);

        // 노트 없이 몇 블록 (무음 기대)
        for (int b = 0; b < 4; ++b) host.process(planar, 2, BLK);
        const double silent = rms(L, R, BLK);

        // C4 노트 온 후 여러 블록 렌더 (어택/로딩 여유)
        host.addNoteOn(0, 60, 110);
        double loud = 0.0;
        for (int b = 0; b < 40; ++b) {
            host.process(planar, 2, BLK);
            loud = std::max(loud, rms(L, R, BLK));
        }
        std::printf("  무음 RMS=%.6f, 노트온 후 최대 RMS=%.6f -> %s\n", silent, loud,
                    loud > 0.0005 ? "소리남 [OK]" : "무음 [FAIL]");
        host.addNoteOff(0, 60);
        host.process(planar, 2, BLK);
    }

    // ---- 이펙트 ----
    if (argc >= 3) {
        Vst3Host fx;
        std::string err;
        if (!fx.loadModule(argv[2], err)) { std::printf("이펙트 로드 실패: %s\n", err.c_str()); return 1; }
        if (!fx.instantiate(0, SR, BLK, err)) { std::printf("이펙트 인스턴스화 실패: %s\n", err.c_str()); return 1; }
        std::printf("이펙트 인스턴스화 OK: %s (isInstrument=%d)\n", fx.activeName().c_str(),
                    fx.isInstrument() ? 1 : 0);
        // 사인파 입력을 넣고 통과시켜 출력이 나오는지
        double outMax = 0.0;
        for (int b = 0; b < 20; ++b) {
            for (int i = 0; i < BLK; ++i) {
                float s = 0.3f * std::sin(2.0 * 3.14159 * 220.0 * (b * BLK + i) / SR);
                L[i] = R[i] = s;
            }
            fx.process(planar, 2, BLK, planar);
            outMax = std::max(outMax, rms(L, R, BLK));
        }
        std::printf("  이펙트 처리 후 출력 RMS=%.6f -> %s\n", outMax,
                    outMax > 0.0005 ? "출력 있음 [OK]" : "무음 [FAIL]");
    }
    std::printf("검증 완료.\n");
    return 0;
}
