#pragma once

#include <algorithm>
#include <cmath>

namespace somike::dsp {

// Upper bound for any gain read from shared memory (+12 dB).
inline constexpr float kMaxGain = 4.0f;

// Gains arrive through shared memory and are therefore untrusted. Non-finite
// values fall back to `fallback`; everything else is clamped to [0, kMaxGain].
[[nodiscard]] inline float SanitizeGain(float gain, float fallback) noexcept {
    if (!std::isfinite(gain)) {
        return fallback;
    }
    return std::clamp(gain, 0.0f, kMaxGain);
}

}  // namespace somike::dsp
