#pragma once
// =============================================================
// MidiPro - sequencer/SmfFile.h
// SMF(Standard MIDI File, .mid) 읽기/쓰기.
//
// 지원 범위 (Phase 2):
//   - 읽기: 포맷 0/1, PPQN 분해능(SMPTE는 거부), 러닝 스테이터스,
//     템포(첫 번째만)/트랙 이름 메타, 채널 보이스 메시지
//   - 쓰기: 포맷 1 (트랙 0 = 템포 트랙), 러닝 스테이터스 미사용
//
// VLQ 인코딩 함수는 유닛 테스트를 위해 공개한다 (Rule 6).
// =============================================================

#include "sequencer/Song.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace midipro::seq::smf {

bool save(const Song& song, const std::filesystem::path& path);
bool load(Song& out, const std::filesystem::path& path);

// ---- VLQ(Variable Length Quantity): SMF의 가변 길이 정수 ----
void writeVlq(std::vector<uint8_t>& out, uint32_t value);
// data[offset]부터 읽어 value에 저장, 소비한 바이트 수 반환 (실패 시 0)
std::size_t readVlq(const uint8_t* data, std::size_t size, std::size_t offset, uint32_t& value);

} // namespace midipro::seq::smf
