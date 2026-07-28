#pragma once
// =============================================================
// MidiPro - vst/VstPreset.h
// 플러그인 음색(프리셋) 파일 <-> 앱의 상태 블롭 변환.
//
// 앱은 플러그인 상태를 "MPST" 블롭으로 다룬다 (Vst3Host::saveState/loadState):
//   'M''P''S''T' | u32 compSize | comp bytes | u32 ctrlSize | ctrl bytes   (LE)
//
// 여기서 하는 일:
//   1) 그 블롭을 .mppreset 파일로 그대로 쓰고 읽는다 (앱이 만든 음색 저장/적용)
//   2) 표준 .vstpreset 파일을 위 블롭으로 바꾼다 (다른 프로그램에서 받은 프리셋)
//
// 왜 파일 파싱을 여기 두나 (Rule 1, 6):
//   순수 바이트 처리라 플러그인 없이 단위 테스트할 수 있다. Vst3Host는
//   실제 플러그인이 있어야 돌아가서 테스트가 어렵다.
//
// .vstpreset 형식 (VST3 SDK, 전부 리틀엔디언):
//   'V''S''T''3' | i32 version | char[32] classId(16진 ASCII) | i64 listOffset
//   ... 덩어리 데이터 ...
//   listOffset 위치: 'L''i''s''t' | i32 count | count번 { char[4] id, i64 offset, i64 size }
//   덩어리 id: "Comp"(컴포넌트 상태), "Cont"(컨트롤러 상태), "Info"(xml 설명)
// =============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace midipro::vst {

// comp/ctrl 바이트를 앱의 MPST 블롭으로 묶는다.
std::vector<uint8_t> packState(const uint8_t* comp, std::size_t compSize, const uint8_t* ctrl,
                               std::size_t ctrlSize);

// MPST 블롭인가 (앞 4바이트 확인)
bool isAppState(const uint8_t* data, std::size_t size);

// .vstpreset 인가
bool isVstPreset(const uint8_t* data, std::size_t size);

// .vstpreset -> MPST 블롭. classIdHex가 있으면 프리셋이 어느 플러그인 것인지 채워 준다
// (32자 16진 ASCII). 형식이 어긋나면 false.
bool vstPresetToState(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out,
                      std::string* classIdHex = nullptr);

// 어떤 형식이든 받아서 MPST 블롭으로. 이미 MPST면 그대로 복사한다.
bool anyPresetToState(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out,
                      std::string* classIdHex = nullptr);

} // namespace midipro::vst
