// VST MPE 표현 전달 검증: 악기에 노트를 넣고 피치벤드를 보냈을 때
// 출력의 기본 주파수(영교차 추정)가 실제로 바뀌는지 측정한다.
#include "vst/Vst3Host.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace midipro::vst;

// L 채널의 양의 방향 영교차 횟수로 대략적 주파수(Hz)를 추정
static double estimateHz(Vst3Host& host, float** planar, int blocks, int BLK, int SR,
                         float bendNorm, bool applyBend) {
    long crossings = 0;
    float prev = 0.f;
    long total = 0;
    for (int b = 0; b < blocks; ++b) {
        if (applyBend) host.addPitchBend(0, bendNorm); // 매 블록 재확인(파라미터 유지)
        host.process(planar, 2, BLK);
        for (int i = 0; i < BLK; ++i) {
            const float s = planar[0][i];
            if (prev <= 0.f && s > 0.f) ++crossings;
            prev = s;
            ++total;
        }
    }
    return (double)crossings * SR / (double)total;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("사용법: vst_mpe_probe <악기.vst3>\n"); return 2; }
    const int SR = 44100, BLK = 512;
    std::vector<float> L(BLK, 0.f), R(BLK, 0.f);
    float* planar[2] = {L.data(), R.data()};

    Vst3Host host;
    std::string err;
    if (!host.loadModule(argv[1], err) || !host.instantiate(0, SR, BLK, err)) {
        std::printf("로드/인스턴스화 실패: %s\n", err.c_str());
        return 1;
    }
    std::printf("악기: %s\n", host.activeName().c_str());

    host.addNoteOn(0, 60, 110); // C4 (261.6Hz)
    for (int b = 0; b < 30; ++b) host.process(planar, 2, BLK); // 어택/워밍업

    const double f0 = estimateHz(host, planar, 60, BLK, SR, 0.f, false);
    const double f1 = estimateHz(host, planar, 60, BLK, SR, 1.0f, true); // 최대 업벤드

    std::printf("피치벤드 전 ~%.1f Hz, 최대 업벤드 후 ~%.1f Hz\n", f0, f1);
    const bool changed = f1 > f0 * 1.03; // 3% 이상 상승이면 벤드 전달됨
    std::printf("표현(피치벤드) 전달: %s\n", changed ? "확인됨 [OK]" : "변화 없음 [?]");
    host.addNoteOff(0, 60);
    return changed ? 0 : 3;
}
