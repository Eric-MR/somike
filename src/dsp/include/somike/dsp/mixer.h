#pragma once

#include <cstddef>
#include <cstdint>

namespace somike::dsp {

// Replaces NaN/Inf with silence and hard-limits to [-1, 1].
[[nodiscard]] float SanitizeSample(float sample) noexcept;

// Mixes canonical stereo audio into an endpoint buffer in place:
//   io[c] = io[c] * mic_gain + inject[c'] * inject_gain
//
// Channel mapping from the stereo source:
//   1 channel  -> (L + R) / 2
//   2 channels -> L, R
//   N channels -> (L + R) / 2 on every channel (mic arrays are summed by apps)
//
// If `input_silent` is true the engine flagged the input buffer as silent and
// its contents are undefined, so the microphone part is treated as zero.
void MixInto(float* io, std::size_t frames, std::uint32_t channels, const float* inject_stereo,
             float inject_gain, float mic_gain, bool input_silent) noexcept;

}  // namespace somike::dsp
