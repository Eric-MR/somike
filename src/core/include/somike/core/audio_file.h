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
// files Windows cannot read.
[[nodiscard]] HRESULT DecodeAudioFile(const std::wstring& path, double max_seconds, DecodedAudio& out);

}  // namespace somike::core
