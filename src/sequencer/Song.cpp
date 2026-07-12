// =============================================================
// MidiPro - sequencer/Song.cpp
// =============================================================

#include "sequencer/Song.h"

#include <cmath>

namespace midipro::seq {

uint32_t Song::lengthTicks() const {
    uint32_t longest = 0;
    for (const auto& t : tracks) {
        const uint32_t len = t.lengthTicks();
        if (len > longest) longest = len;
    }
    return longest;
}

// ---- 템포 맵 ----
// tempoChanges는 틱 오름차순이라 선형 탐색으로 충분하다 (지점 수가 적다).
// bpm이 0 이하로 저장돼 있어도 0 나눗셈이 없도록 최소값으로 방어한다.

namespace {
double safeBpm(double b) {
    return b > 1.0 ? b : 1.0;
}

// [0, dTick) 동안 bpm이 b0 -> b1로 선형 변화(램프)할 때 걸리는 초.
// dt = 60/(ppqn·bpm(t)), bpm(t) = b0 + k·t 의 적분: (60/(ppqn·k))·ln(b1/b0)
double rampSeconds(double dTick, double b0, double b1, int ppqn) {
    if (dTick <= 0.0) return 0.0;
    b0 = safeBpm(b0);
    b1 = safeBpm(b1);
    if (std::abs(b1 - b0) < 1e-9) return ticksToSeconds(dTick, b0, ppqn);
    const double k = (b1 - b0) / dTick;
    return 60.0 / ((double)ppqn * k) * std::log(b1 / b0);
}

// 역방향: 같은 램프에서 s초가 걸리는 틱 수
double rampTicks(double s, double dTickTotal, double b0, double b1, int ppqn) {
    b0 = safeBpm(b0);
    b1 = safeBpm(b1);
    if (std::abs(b1 - b0) < 1e-9 || dTickTotal <= 0.0)
        return secondsToTicks(s, b0, ppqn);
    const double k = (b1 - b0) / dTickTotal;
    const double c = 60.0 / ((double)ppqn * k);
    return b0 * (std::exp(s / c) - 1.0) / k;
}

// 지점 i가 유효한 램프인가 (다음 지점이 있고 그 틱이 더 뒤)
bool isRamp(const Song& song, std::size_t i) {
    return song.tempoChanges[i].ramp && i + 1 < song.tempoChanges.size() &&
           song.tempoChanges[i + 1].tick > song.tempoChanges[i].tick;
}
} // namespace

double bpmAtTick(const Song& song, double tick) {
    const auto& tcs = song.tempoChanges;
    double bpm = safeBpm(song.bpm);
    for (std::size_t i = 0; i < tcs.size(); ++i) {
        if ((double)tcs[i].tick > tick) break;
        if (isRamp(song, i) && tick < (double)tcs[i + 1].tick) {
            const double f = (tick - (double)tcs[i].tick) /
                             ((double)tcs[i + 1].tick - (double)tcs[i].tick);
            bpm = safeBpm(tcs[i].bpm) +
                  (safeBpm(tcs[i + 1].bpm) - safeBpm(tcs[i].bpm)) * f;
        } else {
            bpm = safeBpm(tcs[i].bpm);
        }
    }
    return safeBpm(bpm);
}

double songTickToSec(const Song& song, double tick) {
    const auto& tcs = song.tempoChanges;
    if (tick <= 0.0) return ticksToSeconds(tick, bpmAtTick(song, 0.0), song.ppqn);
    double sec = 0.0;
    double prevTick = 0.0;
    double bpm = safeBpm(song.bpm);
    for (std::size_t i = 0; i < tcs.size(); ++i) {
        const double ct = (double)tcs[i].tick;
        if (ct >= tick) break;
        if (ct > prevTick) { // 직전 일정 구간 [prevTick, ct) 마감
            sec += ticksToSeconds(ct - prevTick, bpm, song.ppqn);
            prevTick = ct;
        }
        if (isRamp(song, i)) { // 램프 구간 [ct, 다음 지점)을 해석적으로 적분
            const double nt = (double)tcs[i + 1].tick;
            const double b0 = safeBpm(tcs[i].bpm), b1 = safeBpm(tcs[i + 1].bpm);
            const double segEnd = nt < tick ? nt : tick;
            const double bEnd = b0 + (b1 - b0) * ((segEnd - ct) / (nt - ct));
            sec += rampSeconds(segEnd - ct, b0, bEnd, song.ppqn);
            prevTick = segEnd;
            bpm = b1;
            if (segEnd >= tick) return sec;
        } else {
            bpm = safeBpm(tcs[i].bpm);
        }
    }
    return sec + ticksToSeconds(tick - prevTick, bpm, song.ppqn);
}

double songSecToTick(const Song& song, double sec) {
    const auto& tcs = song.tempoChanges;
    if (sec <= 0.0) return secondsToTicks(sec, bpmAtTick(song, 0.0), song.ppqn);
    double accSec = 0.0;
    double prevTick = 0.0;
    double bpm = safeBpm(song.bpm);
    for (std::size_t i = 0; i < tcs.size(); ++i) {
        const double ct = (double)tcs[i].tick;
        if (ct > prevTick) { // 일정 구간 [prevTick, ct)
            const double segSec = ticksToSeconds(ct - prevTick, bpm, song.ppqn);
            if (accSec + segSec >= sec) // 목표 시각이 이 구간 안에 있다
                return prevTick + secondsToTicks(sec - accSec, bpm, song.ppqn);
            accSec += segSec;
            prevTick = ct;
        }
        if (isRamp(song, i)) { // 램프 구간
            const double nt = (double)tcs[i + 1].tick;
            const double b0 = safeBpm(tcs[i].bpm), b1 = safeBpm(tcs[i + 1].bpm);
            const double segSec = rampSeconds(nt - ct, b0, b1, song.ppqn);
            if (accSec + segSec >= sec)
                return ct + rampTicks(sec - accSec, nt - ct, b0, b1, song.ppqn);
            accSec += segSec;
            prevTick = nt;
            bpm = b1;
        } else {
            bpm = safeBpm(tcs[i].bpm);
        }
    }
    return prevTick + secondsToTicks(sec - accSec, bpm, song.ppqn);
}

} // namespace midipro::seq
