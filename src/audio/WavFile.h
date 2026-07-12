#pragma once
// =============================================================
// MidiPro - audio/WavFile.h
// AudioClip <-> WAV(32비트 float PCM) 파일 변환.
//
// 왜 WAV인가 (Rule 1, 6):
//   프로젝트(.midipro)는 텍스트라 오디오 PCM을 담기엔 부적합하다.
//   클립은 프로젝트 옆 사이드카 폴더에 WAV로 저장하고, 프로젝트
//   텍스트는 파일명만 참조한다. 인코딩/디코딩은 순수 로직이라
//   메모리 버퍼 단위로 왕복 테스트할 수 있다.
// =============================================================

#include "audio/AudioClip.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace midipro::audio {

// 클립의 PCM 전체를 32비트 float WAV 바이트로 인코딩한다 (순수 함수).
std::vector<uint8_t> encodeWav(const AudioClip& clip);

// WAV 바이트를 클립으로 디코딩한다. 실패 시 nullptr.
// (16/24/32비트 정수 PCM과 32비트 float PCM을 지원)
std::shared_ptr<AudioClip> decodeWav(const uint8_t* data, std::size_t size,
                                     const std::string& name);

// 파일 입출력 (경로는 유니코드 안전)
bool writeWavFile(const AudioClip& clip, const std::filesystem::path& path);
std::shared_ptr<AudioClip> readWavFile(const std::filesystem::path& path,
                                       const std::string& name);

} // namespace midipro::audio
