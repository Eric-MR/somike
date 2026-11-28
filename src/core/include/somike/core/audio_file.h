#pragma once

#include <windows.h>

#include <string>

#include "somike/dsp/voice_mixer.h"

namespace somike::core {

struct DecodedAudio {
    dsp::ClipPtr clip;            // canonical: 48 kHz, stereo, float32
    double source_seconds = 0.0;  // length of the whole file
    bool truncated = false;       // longer than the limit: only the start was decoded
};

// Decodes an audio file with Media Foundation: MP3, WAV, AAC/M4A/MP4, WMA,
// FLAC and ALAC out of the box; Ogg, Opus and WebM where the Microsoft Store
// codecs ("Web Media Extensions") are installed. Only the first `max_seconds`
// are decoded.
//
// Call on a thread with COM initialised (MTA recommended). Fails with
// MF_E_UNSUPPORTED_BYTESTREAM_TYPE / MF_E_INVALIDMEDIATYPE and friends for
// files Windows cannot read, and with kMediaFoundationMissing where Windows
// has no Media Foundation (N editions without the Media Feature Pack).
[[nodiscard]] HRESULT DecodeAudioFile(const std::wstring& path, double max_seconds, DecodedAudio& out);

inline constexpr HRESULT kMediaFoundationMissing = __HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

// True if mfplat.dll and mfreadwrite.dll are in System32. Executables that
// decode delay-load them (somike_delay_load_media_foundation in CMake), so
// this must hold before the first Media Foundation call.
[[nodiscard]] bool MediaFoundationAvailable() noexcept;

}  // namespace somike::core
