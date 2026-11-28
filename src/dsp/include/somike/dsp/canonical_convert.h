#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace somike::dsp {

// Folds any channel layout down (or up) to stereo, interleaved.
//   mono -> both sides; stereo -> as is; 3+ channels (WAVE order FL FR FC
//   LFE ...) -> front left/right plus the centre at -3 dB; the rest dropped.
[[nodiscard]] std::vector<float> ToStereo(std::span<const float> interleaved, std::uint32_t channels);

// High-quality offline sample-rate conversion of interleaved stereo audio
// (windowed sinc, polyphase table). Meant for whole decoded files, not for
// real-time streams: it sees the entire input at once, so there is no state.
[[nodiscard]] std::vector<float> ResampleStereo(std::span<const float> stereo, std::uint32_t from_rate,
                                                std::uint32_t to_rate);

// Decoded file -> canonical ring format (48 kHz, stereo, float32, interleaved).
// Non-finite samples become silence. Empty input or a zero rate/channel count
// yields an empty result.
[[nodiscard]] std::vector<float> ConvertToCanonical(std::span<const float> interleaved,
                                                    std::uint32_t channels, std::uint32_t sample_rate);

}  // namespace somike::dsp
