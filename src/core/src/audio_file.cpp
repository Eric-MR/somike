#include "somike/core/audio_file.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "somike/dsp/canonical_convert.h"

namespace somike::core {

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

// MFStartup/MFShutdown are reference counted, so every decode can bracket itself.
class MediaFoundation {
public:
    MediaFoundation() noexcept : hr_(::MFStartup(MF_VERSION, MFSTARTUP_LITE)) {}
    ~MediaFoundation() {
        if (SUCCEEDED(hr_)) {
            ::MFShutdown();
        }
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;
    [[nodiscard]] HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

struct PcmFormat {
    bool is_float = false;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t bits = 0;
};

HRESULT RequestOutput(IMFSourceReader* reader, const GUID& subtype) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = ::MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kAudioStream, nullptr, type.Get());
    return hr;
}

HRESULT ReadFormat(IMFSourceReader* reader, PcmFormat& format) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader->GetCurrentMediaType(kAudioStream, &type);
    if (FAILED(hr)) {
        return hr;
    }
    GUID subtype{};
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    format.is_float = subtype == MFAudioFormat_Float;
    format.channels = ::MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    format.sample_rate = ::MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    format.bits = ::MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);

    const bool bits_ok = format.is_float ? format.bits == 32
                                         : (format.bits == 8 || format.bits == 16 || format.bits == 24 ||
                                            format.bits == 32);
    if ((subtype != MFAudioFormat_Float && subtype != MFAudioFormat_PCM) || !bits_ok ||
        format.channels == 0 || format.channels > 32 || format.sample_rate < 4000 ||
        format.sample_rate > 384000) {
        return MF_E_INVALIDMEDIATYPE;
    }
    return S_OK;
}

// Appends the samples of one decoded buffer as float.
void AppendSamples(const PcmFormat& format, const BYTE* data, DWORD bytes, std::vector<float>& out) {
    const std::size_t bytes_per_sample = format.bits / 8;
    const std::size_t count = bytes / bytes_per_sample;
    const std::size_t start = out.size();
    out.resize(start + count);
    float* dst = out.data() + start;
    if (format.is_float) {
        std::memcpy(dst, data, count * sizeof(float));
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const BYTE* s = data + i * bytes_per_sample;
        switch (format.bits) {
            case 8:
                dst[i] = (static_cast<float>(s[0]) - 128.0f) / 128.0f;
                break;
            case 16: {
                std::int16_t v;
                std::memcpy(&v, s, 2);
                dst[i] = static_cast<float>(v) / 32768.0f;
                break;
            }
            case 24: {
                const std::int32_t v = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(s[0]) << 8) | (static_cast<std::uint32_t>(s[1]) << 16) |
                    (static_cast<std::uint32_t>(s[2]) << 24));
                dst[i] = static_cast<float>(v) / 2147483648.0f;
                break;
            }
            default: {
                std::int32_t v;
                std::memcpy(&v, s, 4);
                dst[i] = static_cast<float>(v) / 2147483648.0f;
                break;
            }
        }
    }
}

double DurationSeconds(IMFSourceReader* reader) {
    PROPVARIANT value;
    ::PropVariantInit(&value);
    double seconds = 0.0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &value)) &&
        value.vt == VT_UI8) {
        seconds = static_cast<double>(value.uhVal.QuadPart) / 1e7;  // 100 ns units
    }
    ::PropVariantClear(&value);
    return seconds;
}

}  // namespace

bool MediaFoundationAvailable() noexcept {
    // Loaded once and kept: the delay-load helper then finds them by name.
    static const bool available =
        ::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr &&
        ::LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    return available;
}

HRESULT DecodeAudioFile(const std::wstring& path, double max_seconds, DecodedAudio& out) {
    out = {};
    if (!MediaFoundationAvailable()) {
        return kMediaFoundationMissing;  // a delay-loaded call would raise instead
    }
    const MediaFoundation mf;
    if (FAILED(mf.result())) {
        return mf.result();
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        return hr;
    }
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(kAudioStream, TRUE);
    if (FAILED(hr)) {
        return hr;  // no audio stream (e.g. a picture)
    }

    // Float straight from the decoder; uncompressed sources (WAV) may only
    // offer integer PCM, which is converted below.
    if (FAILED(RequestOutput(reader.Get(), MFAudioFormat_Float))) {
        hr = RequestOutput(reader.Get(), MFAudioFormat_PCM);
        if (FAILED(hr)) {
            return hr;
        }
    }
    PcmFormat format;
    hr = ReadFormat(reader.Get(), format);
    if (FAILED(hr)) {
        return hr;
    }

    out.source_seconds = DurationSeconds(reader.Get());
    const auto max_frames = static_cast<std::size_t>(max_seconds * format.sample_rate);
    std::vector<float> samples;
    if (out.source_seconds > 0.0) {
        const double frames = std::min(out.source_seconds, max_seconds) * format.sample_rate;
        samples.reserve(static_cast<std::size_t>(frames + format.sample_rate) * format.channels);
    }

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(kAudioStream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            return hr;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            PcmFormat changed;
            if (FAILED(ReadFormat(reader.Get(), changed)) || changed.channels != format.channels ||
                changed.sample_rate != format.sample_rate) {
                break;  // keep what was decoded so far
            }
            format = changed;
        }
        if (!sample) {
            continue;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            continue;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            AppendSamples(format, data, length, samples);
            buffer->Unlock();
        }
        if (samples.size() / format.channels >= max_frames) {
            samples.resize(max_frames * format.channels);
            out.truncated = true;
            break;
        }
    }

    auto clip = std::make_shared<dsp::Clip>();
    clip->samples = dsp::ConvertToCanonical(samples, format.channels, format.sample_rate);
    if (clip->frames() == 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);  // decodable, but no audio in it
    }
    if (out.source_seconds <= 0.0) {
        out.source_seconds = static_cast<double>(samples.size() / format.channels) / format.sample_rate;
    }
    out.clip = std::move(clip);
    return S_OK;
}

}  // namespace somike::core
