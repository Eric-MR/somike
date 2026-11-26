#include "somike/dsp/mixer.h"

#include <algorithm>
#include <cmath>

namespace somike::dsp {

float SanitizeSample(float sample) noexcept {
    if (!std::isfinite(sample)) {
        return 0.0f;
    }
    return std::clamp(sample, -1.0f, 1.0f);
}

void MixInto(float* io, std::size_t frames, std::uint32_t channels, const float* inject_stereo,
             float inject_gain, float mic_gain, bool input_silent) noexcept {
    if (io == nullptr || inject_stereo == nullptr || channels == 0) {
        return;
    }

    for (std::size_t f = 0; f < frames; ++f) {
        const float left = inject_stereo[2 * f] * inject_gain;
        const float right = inject_stereo[2 * f + 1] * inject_gain;
        float* frame = io + f * channels;

        // When the engine marks the input silent its contents are undefined,
        // so the microphone contribution is exactly zero (not "garbage * 0",
        // which would propagate NaNs).
        auto mic = [&](std::uint32_t c) noexcept {
            return input_silent ? 0.0f : frame[c] * mic_gain;
        };

        if (channels == 2) {
            frame[0] = SanitizeSample(mic(0) + left);
            frame[1] = SanitizeSample(mic(1) + right);
        } else {
            const float mono = 0.5f * (left + right);
            for (std::uint32_t c = 0; c < channels; ++c) {
                frame[c] = SanitizeSample(mic(c) + mono);
            }
        }
    }
}

}  // namespace somike::dsp
