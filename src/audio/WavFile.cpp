// =============================================================
// MidiPro - audio/WavFile.cpp
// =============================================================

#include "audio/WavFile.h"

#include <cstring>
#include <fstream>

namespace midipro::audio {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF));
    b.push_back((uint8_t)((v >> 24) & 0xFF));
}
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}
void putTag(std::vector<uint8_t>& b, const char* tag) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)tag[i]);
}

uint32_t readU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t readU16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

} // namespace

std::vector<uint8_t> encodeWav(const AudioClip& clip) {
    std::vector<uint8_t> b;
    const uint16_t channels = (uint16_t)(clip.channels > 0 ? clip.channels : 1);
    const uint32_t sampleRate = (uint32_t)(clip.sampleRate > 0 ? clip.sampleRate : 44100);
    const uint16_t bits = 32;
    const uint32_t dataBytes = (uint32_t)(clip.pcm.size() * sizeof(float));
    const uint32_t byteRate = sampleRate * channels * (bits / 8);
    const uint16_t blockAlign = (uint16_t)(channels * (bits / 8));

    b.reserve(44 + dataBytes);
    putTag(b, "RIFF");
    putU32(b, 36 + dataBytes); // 이후 크기
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    putU32(b, 16);
    putU16(b, kFormatFloat);
    putU16(b, channels);
    putU32(b, sampleRate);
    putU32(b, byteRate);
    putU16(b, blockAlign);
    putU16(b, bits);
    putTag(b, "data");
    putU32(b, dataBytes);

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(clip.pcm.data());
    b.insert(b.end(), raw, raw + dataBytes);
    return b;
}

std::shared_ptr<AudioClip> decodeWav(const uint8_t* data, std::size_t size,
                                     const std::string& name) {
    if (!data || size < 44) return nullptr;
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return nullptr;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataChunk = nullptr;
    uint32_t dataBytes = 0;

    // 청크 순회 (fmt/data 위치는 가변)
    std::size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* id = data + pos;
        const uint32_t chunkSize = readU32(data + pos + 4);
        const std::size_t body = pos + 8;
        if (body + chunkSize > size) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
            format = readU16(data + body + 0);
            channels = readU16(data + body + 2);
            sampleRate = readU32(data + body + 4);
            bits = readU16(data + body + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataChunk = data + body;
            dataBytes = chunkSize;
        }
        pos = body + chunkSize + (chunkSize & 1); // 청크는 짝수 정렬
    }
    if (!dataChunk || channels == 0 || sampleRate == 0) return nullptr;

    auto clip = std::make_shared<AudioClip>();
    clip->name = name;
    clip->channels = (int)channels;
    clip->sampleRate = (int)sampleRate;

    if (format == kFormatFloat && bits == 32) {
        const std::size_t n = dataBytes / sizeof(float);
        clip->pcm.resize(n);
        std::memcpy(clip->pcm.data(), dataChunk, n * sizeof(float));
    } else if (format == kFormatPcm && bits == 16) {
        const std::size_t n = dataBytes / 2;
        clip->pcm.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const int16_t s = (int16_t)readU16(dataChunk + i * 2);
            clip->pcm[i] = (float)s / 32768.0f;
        }
    } else if (format == kFormatPcm && bits == 24) {
        const std::size_t n = dataBytes / 3;
        clip->pcm.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const uint8_t* p = dataChunk + i * 3;
            int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            clip->pcm[i] = (float)(s >> 8) / 8388608.0f;
        }
    } else if (format == kFormatPcm && bits == 32) {
        const std::size_t n = dataBytes / 4;
        clip->pcm.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const int32_t s = (int32_t)readU32(dataChunk + i * 4);
            clip->pcm[i] = (float)s / 2147483648.0f;
        }
    } else {
        return nullptr; // 지원하지 않는 포맷
    }

    clip->trimStart = 0;
    clip->trimLen = (int64_t)clip->frames();
    clip->buildPeaks();
    return clip;
}

bool writeWavFile(const AudioClip& clip, const std::filesystem::path& path) {
    const std::vector<uint8_t> bytes = encodeWav(clip);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return out.good();
}

std::shared_ptr<AudioClip> readWavFile(const std::filesystem::path& path,
                                       const std::string& name) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return nullptr;
    const std::streamsize size = in.tellg();
    if (size <= 0) return nullptr;
    in.seekg(0);
    std::vector<uint8_t> bytes((std::size_t)size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
    return decodeWav(bytes.data(), bytes.size(), name);
}

} // namespace midipro::audio
