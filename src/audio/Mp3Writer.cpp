// =============================================================
// MidiPro - audio/Mp3Writer.cpp
// Media Foundation SinkWriter로 float PCM을 MP3로 인코딩한다.
// =============================================================

#include "audio/Mp3Writer.h"

#define NOMINMAX // windows.h의 min/max 매크로가 std::min과 충돌하지 않게
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>

namespace midipro::audio {

bool writeMp3File(const AudioClip& clip, const std::filesystem::path& path, int bitrateKbps) {
    if (clip.pcm.empty() || clip.sampleRate <= 0) return false;
    const UINT32 ch = (UINT32)(clip.channels == 1 ? 1 : 2);
    const UINT32 sr = (UINT32)clip.sampleRate;
    // MPEG-1 Layer III가 지원하는 샘플레이트만 (엔진은 44.1k/48k라 충분)
    if (sr != 32000 && sr != 44100 && sr != 48000) return false;
    if (bitrateKbps < 64) bitrateKbps = 64;
    if (bitrateKbps > 320) bitrateKbps = 320;

    // COM: 이미 초기화돼 있으면(S_FALSE/RPC_E_CHANGED_MODE) 그대로 쓴다.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needCoUninit = SUCCEEDED(coHr);

    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        IMFSinkWriter* writer = nullptr;
        if (SUCCEEDED(
                MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, nullptr, &writer))) {
            // 출력: MP3
            IMFMediaType* out = nullptr;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
            out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
            out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           (UINT32)(bitrateKbps * 1000 / 8));
            DWORD stream = 0;
            HRESULT hr = writer->AddStream(out, &stream);

            // 입력: float32 PCM (필요하면 MF가 변환기를 끼워 준다)
            IMFMediaType* in = nullptr;
            MFCreateMediaType(&in);
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 4);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sr * ch * 4);
            in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, in, nullptr);

            if (SUCCEEDED(hr) && SUCCEEDED(writer->BeginWriting())) {
                // 0.5초 청크로 나눠 공급한다
                const std::size_t framesTotal = clip.pcm.size() / ch;
                const std::size_t chunkFrames = sr / 2;
                LONGLONG t100ns = 0;
                ok = true;
                for (std::size_t f = 0; f < framesTotal && ok; f += chunkFrames) {
                    const std::size_t n = std::min(chunkFrames, framesTotal - f);
                    const DWORD bytes = (DWORD)(n * ch * sizeof(float));
                    IMFMediaBuffer* buf = nullptr;
                    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) {
                        ok = false;
                        break;
                    }
                    BYTE* dst = nullptr;
                    buf->Lock(&dst, nullptr, nullptr);
                    std::memcpy(dst, clip.pcm.data() + f * ch, bytes);
                    buf->Unlock();
                    buf->SetCurrentLength(bytes);

                    IMFSample* smp = nullptr;
                    MFCreateSample(&smp);
                    smp->AddBuffer(buf);
                    const LONGLONG dur = (LONGLONG)n * 10000000LL / sr;
                    smp->SetSampleTime(t100ns);
                    smp->SetSampleDuration(dur);
                    if (FAILED(writer->WriteSample(stream, smp))) ok = false;
                    t100ns += dur;
                    smp->Release();
                    buf->Release();
                }
                if (ok) ok = SUCCEEDED(writer->Finalize());
            }
            if (in) in->Release();
            if (out) out->Release();
            writer->Release();
        }
        MFShutdown();
    }
    if (needCoUninit) CoUninitialize();
    return ok;
}

} // namespace midipro::audio
