#pragma once
// =============================================================
// MidiPro - audio/Mp3Writer.h
// AudioClip -> MP3 파일 인코딩.
//
// Windows Media Foundation의 내장 MP3 인코더를 쓴다 (Windows 10+
// 기본 탑재, 외부 라이브러리 불필요). MP3가 지원하는 샘플레이트
// (32k/44.1k/48k)만 받는다 — 엔진 출력이 그 범위라 충분하다.
// =============================================================

#include "audio/AudioClip.h"

#include <filesystem>

namespace midipro::audio {

// clip(1~2채널 float PCM)을 MP3로 인코딩해 저장한다. 성공 시 true.
bool writeMp3File(const AudioClip& clip, const std::filesystem::path& path,
                  int bitrateKbps = 192);

} // namespace midipro::audio
