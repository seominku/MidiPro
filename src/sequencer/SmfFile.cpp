// =============================================================
// MidiPro - sequencer/SmfFile.cpp
// =============================================================

#include "sequencer/SmfFile.h"

#include "midi/MidiConstants.h"

#include <cstring>
#include <fstream>

namespace midipro::seq::smf {

namespace {

// ---- 메타 이벤트 타입 (매직 넘버 금지, Rule 5) ----
constexpr uint8_t kMetaPrefix = 0xFF;
constexpr uint8_t kMetaTrackName = 0x03;
constexpr uint8_t kMetaEndOfTrack = 0x2F;
constexpr uint8_t kMetaSetTempo = 0x51;
constexpr uint8_t kMetaTimeSignature = 0x58;
constexpr double kMicrosecondsPerMinute = 60000000.0;

void writeU16Be(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v & 0xFF));
}

void writeU32Be(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v & 0xFF));
}

uint32_t readU32Be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint16_t readU16Be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// 트랙 데이터(chunk 내용)를 MTrk 청크로 감싸 파일 버퍼에 붙인다
void appendTrackChunk(std::vector<uint8_t>& file, const std::vector<uint8_t>& body) {
    file.push_back('M');
    file.push_back('T');
    file.push_back('r');
    file.push_back('k');
    writeU32Be(file, (uint32_t)body.size());
    file.insert(file.end(), body.begin(), body.end());
}

void appendEndOfTrack(std::vector<uint8_t>& body) {
    body.push_back(0x00); // delta
    body.push_back(kMetaPrefix);
    body.push_back(kMetaEndOfTrack);
    body.push_back(0x00); // 길이
}

} // namespace

// ---------------------------------------------------------
// VLQ
// ---------------------------------------------------------
void writeVlq(std::vector<uint8_t>& out, uint32_t value) {
    // 7비트씩 상위부터, 마지막 바이트만 최상위 비트 0
    uint8_t bytes[5];
    int count = 0;
    do {
        bytes[count++] = (uint8_t)(value & 0x7F);
        value >>= 7;
    } while (value > 0);
    for (int i = count - 1; i > 0; --i)
        out.push_back((uint8_t)(bytes[i] | 0x80));
    out.push_back(bytes[0]);
}

std::size_t readVlq(const uint8_t* data, std::size_t size, std::size_t offset, uint32_t& value) {
    value = 0;
    std::size_t consumed = 0;
    while (offset + consumed < size && consumed < 5) {
        const uint8_t byte = data[offset + consumed];
        ++consumed;
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) return consumed;
    }
    return 0; // 잘렸거나 5바이트 초과 = 손상된 파일
}

// ---------------------------------------------------------
// 저장 (포맷 1)
// ---------------------------------------------------------
bool save(const Song& song, const std::filesystem::path& path) {
    std::vector<uint8_t> file;

    // MThd: 포맷 1, 트랙 수 = 템포 트랙 + 곡 트랙
    file.push_back('M');
    file.push_back('T');
    file.push_back('h');
    file.push_back('d');
    writeU32Be(file, 6);
    writeU16Be(file, 1); // 포맷 1
    writeU16Be(file, (uint16_t)(song.tracks.size() + 1));
    writeU16Be(file, (uint16_t)song.ppqn);

    // ---- 트랙 0: 템포/박자 전용 (포맷 1 관례) ----
    {
        std::vector<uint8_t> body;
        // Set Tempo: 4분음표당 마이크로초
        const uint32_t usPerQuarter = (uint32_t)(kMicrosecondsPerMinute / song.bpm);
        body.push_back(0x00);
        body.push_back(kMetaPrefix);
        body.push_back(kMetaSetTempo);
        body.push_back(0x03);
        body.push_back((uint8_t)(usPerQuarter >> 16));
        body.push_back((uint8_t)(usPerQuarter >> 8));
        body.push_back((uint8_t)(usPerQuarter & 0xFF));
        // Time Signature 4/4 (분모는 2의 지수, 클럭 24, 32분음표 8)
        body.push_back(0x00);
        body.push_back(kMetaPrefix);
        body.push_back(kMetaTimeSignature);
        body.push_back(0x04);
        body.push_back(0x04);
        body.push_back(0x02);
        body.push_back(0x18);
        body.push_back(0x08);
        appendEndOfTrack(body);
        appendTrackChunk(file, body);
    }

    // ---- 곡 트랙들 ----
    for (const auto& track : song.tracks) {
        std::vector<uint8_t> body;

        if (!track.name.empty()) {
            body.push_back(0x00);
            body.push_back(kMetaPrefix);
            body.push_back(kMetaTrackName);
            writeVlq(body, (uint32_t)track.name.size());
            body.insert(body.end(), track.name.begin(), track.name.end());
        }

        // 이벤트는 정렬돼 있다고 가정하지 않고 복사 후 정렬한다
        Track sorted = track;
        sorted.sortEvents();

        uint32_t lastTick = 0;
        for (const auto& e : sorted.events) {
            writeVlq(body, e.tick - lastTick);
            lastTick = e.tick;
            // 러닝 스테이터스를 쓰지 않는 이유: 파일이 조금 커지는
            // 대신 디버깅(hex 덤프 확인)이 쉬워진다.
            body.push_back(e.status);
            body.push_back(e.data1);
            if (e.isThreeBytes()) body.push_back(e.data2);
        }
        appendEndOfTrack(body);
        appendTrackChunk(file, body);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write((const char*)file.data(), (std::streamsize)file.size());
    return out.good();
}

// ---------------------------------------------------------
// 불러오기
// ---------------------------------------------------------
bool load(Song& outSong, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize fileSize = in.tellg();
    if (fileSize < 14) return false; // MThd(14바이트)보다 작으면 손상
    in.seekg(0);
    std::vector<uint8_t> data((std::size_t)fileSize);
    in.read((char*)data.data(), fileSize);
    if (!in.good()) return false;

    // ---- 헤더 ----
    if (std::memcmp(data.data(), "MThd", 4) != 0) return false;
    if (readU32Be(&data[4]) != 6) return false;
    const uint16_t format = readU16Be(&data[8]);
    if (format > 1) return false; // 포맷 2는 미지원
    const uint16_t division = readU16Be(&data[12]);
    if (division & 0x8000) return false; // SMPTE 분해능 미지원

    Song song;
    song.ppqn = division;
    bool tempoFound = false;

    // ---- 트랙 청크 순회 ----
    std::size_t pos = 14;
    while (pos + 8 <= data.size()) {
        const bool isTrack = std::memcmp(&data[pos], "MTrk", 4) == 0;
        const uint32_t chunkLength = readU32Be(&data[pos + 4]);
        const std::size_t bodyStart = pos + 8;
        const std::size_t bodyEnd = bodyStart + chunkLength;
        if (bodyEnd > data.size()) return false;
        pos = bodyEnd; // 다음 청크 위치 (알 수 없는 청크는 건너뜀)
        if (!isTrack) continue;

        Track track;
        bool channelSet = false;
        uint32_t absTick = 0;
        uint8_t runningStatus = 0;
        std::size_t p = bodyStart;

        while (p < bodyEnd) {
            uint32_t delta = 0;
            const std::size_t deltaBytes = readVlq(data.data(), bodyEnd, p, delta);
            if (deltaBytes == 0) return false;
            p += deltaBytes;
            absTick += delta;
            if (p >= bodyEnd) return false;

            const uint8_t first = data[p];

            if (first == kMetaPrefix) {
                // 메타 이벤트: FF type len data
                if (p + 2 > bodyEnd) return false;
                const uint8_t metaType = data[p + 1];
                uint32_t length = 0;
                const std::size_t lengthBytes = readVlq(data.data(), bodyEnd, p + 2, length);
                if (lengthBytes == 0) return false;
                const std::size_t payload = p + 2 + lengthBytes;
                if (payload + length > bodyEnd) return false;

                if (metaType == kMetaSetTempo && length == 3 && !tempoFound) {
                    // 왜 첫 템포만 쓰는가: Phase 2는 단일 BPM 모델.
                    // 템포 맵은 확장 지점으로 남긴다.
                    const uint32_t usPerQuarter = ((uint32_t)data[payload] << 16) |
                                                  ((uint32_t)data[payload + 1] << 8) |
                                                  data[payload + 2];
                    if (usPerQuarter > 0) {
                        song.bpm = kMicrosecondsPerMinute / usPerQuarter;
                        tempoFound = true;
                    }
                } else if (metaType == kMetaTrackName && length > 0) {
                    track.name.assign((const char*)&data[payload], length);
                } else if (metaType == kMetaEndOfTrack) {
                    break;
                }
                p = payload + length;
                runningStatus = 0; // 메타는 러닝 스테이터스를 끊는다
            } else if (first == midi::kStatusSysExStart || first == 0xF7) {
                // SysEx: 길이만큼 건너뜀
                uint32_t length = 0;
                const std::size_t lengthBytes = readVlq(data.data(), bodyEnd, p + 1, length);
                if (lengthBytes == 0) return false;
                p += 1 + lengthBytes + length;
                runningStatus = 0;
            } else {
                // 채널 보이스 메시지 (러닝 스테이터스 지원)
                uint8_t status = first;
                if (status & 0x80) {
                    ++p;
                } else {
                    if (runningStatus == 0) return false; // 상태 없이 데이터 = 손상
                    status = runningStatus;
                }
                runningStatus = status;

                MidiEvent e;
                e.tick = absTick;
                e.status = status;
                if (p >= bodyEnd) return false;
                e.data1 = data[p++];
                if (e.isThreeBytes()) {
                    if (p >= bodyEnd) return false;
                    e.data2 = data[p++];
                }
                if (!channelSet) {
                    track.channel = e.channel();
                    channelSet = true;
                }
                track.events.push_back(e);
            }
        }

        // 템포 전용 트랙(이벤트 없음)은 곡 트랙으로 넣지 않는다
        if (!track.events.empty()) {
            track.sortEvents();
            song.tracks.push_back(std::move(track));
        }
    }

    outSong = std::move(song);
    return true;
}

} // namespace midipro::seq::smf
