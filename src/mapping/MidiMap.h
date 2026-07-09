#pragma once
// =============================================================
// MidiPro - mapping/MidiMap.h
// MIDI CC -> 신스 파라미터 매핑 (MIDI Learn).
//
// 왜 별도 계층인가 (Rule 1):
//   컨트롤러(노브/페이더)의 CC를 어떤 파라미터에 연결할지는
//   MIDI I/O나 오디오 엔진과 독립된 "매핑" 관심사다. 값 스케일링과
//   직렬화는 순수 함수라 GUI/장치 없이 테스트한다 (Rule 6).
//
//   이 계층은 audio::SynthParams(자료구조)만 참조하고 RtAudio 같은
//   구체 엔진은 모른다.
// =============================================================

#include "audio/Synth.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace midipro::mapping {

// 컨트롤러로 조절할 수 있는 신스 파라미터 목록
enum class ParamTarget {
    FilterCutoff,
    FilterResonance,
    Attack,
    Decay,
    Sustain,
    Release,
    LfoRate,
    LfoDepth,
    DelayMix,
    MasterVolume,
    Count
};

struct ParamInfo {
    const char* id;   // 직렬화용 ascii 키
    const char* name; // 화면 표시용
    float min;
    float max;
};

const ParamInfo& paramInfo(ParamTarget target);

// 정규화 값(0~1)을 파라미터 범위로 펴서 적용/역산 (CC 0~127 <-> 값)
void applyNormalized(audio::SynthParams& params, ParamTarget target, float norm01);
float readNormalized(const audio::SynthParams& params, ParamTarget target);

struct CcMapping {
    uint8_t cc;      // CC 번호
    int channel;     // 학습 당시 채널(표시용). 매칭은 CC 번호로만 한다.
    ParamTarget target;
};

class MidiMap {
public:
    // cc를 target에 연결한다. 같은 cc 또는 같은 target의 기존 매핑은 교체.
    // 왜 둘 다 교체하나: 한 노브는 한 파라미터로, 한 파라미터는 한 노브로
    //   두는 1:1이 학습 UX에서 헷갈리지 않는다.
    void bind(uint8_t cc, int channel, ParamTarget target);

    void clearTarget(ParamTarget target);
    void clearAll() { m_mappings.clear(); }

    bool findTarget(uint8_t cc, ParamTarget& out) const;
    int ccForTarget(ParamTarget target) const;      // 매핑 없으면 -1
    int channelForTarget(ParamTarget target) const;  // 매핑 없으면 -1

    const std::vector<CcMapping>& list() const { return m_mappings; }

    // 직렬화 (테스트/왕복 검증용 공개)
    std::string serialize() const;
    bool deserialize(const std::string& text);

    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);

private:
    std::vector<CcMapping> m_mappings;
};

} // namespace midipro::mapping
