#include "somike/dsp/linear_resampler.h"

#include <algorithm>
#include <cmath>

namespace somike::dsp {

LinearResampler::LinearResampler(std::uint32_t channels) noexcept
    : channels_(std::clamp<std::uint32_t>(channels, 1, kMaxChannels)) {}

void LinearResampler::Reset() noexcept {
    position_ = 0.0;
    older_.fill(0.0f);
    newer_.fill(0.0f);
}

std::size_t LinearResampler::InputFramesRequired(double step,
                                                 std::size_t output_frames) const noexcept {
    if (output_frames == 0 || !(step > 0.0)) {
        return 0;
    }
    // Output j interpolates extended frames floor(p_j) and floor(p_j) + 1 with
    // p_j = position_ + j * step, where extended frames 0 and 1 are the two
    // history frames and frame k >= 2 is input[k - 2]. The last output reads
    // at most extended frame floor(p_N) + 1, i.e. input[floor(p_N) - 1], and
    // afterwards exactly floor(p_N) input frames have been consumed.
    const double end = position_ + static_cast<double>(output_frames) * step;
    return static_cast<std::size_t>(std::floor(end));
}

void LinearResampler::Process(const float* input, std::size_t input_frames, float* output,
                              std::size_t output_frames, double step) noexcept {
    if (output_frames == 0 || output == nullptr) {
        return;
    }
    if (!(step > 0.0)) {
        step = 1.0;
    }
    if (input == nullptr) {
        input_frames = 0;
    }
    const std::uint32_t ch = channels_;

    // Extended frame k; indices beyond the supplied input clamp to the last
    // available frame, so a short input degrades gracefully.
    auto frame = [&](std::size_t k) noexcept -> const float* {
        if (k == 0) {
            return older_.data();
        }
        if (k == 1 || input_frames == 0) {
            return newer_.data();
        }
        return input + std::min(k - 2, input_frames - 1) * ch;
    };

    double pos = position_;
    for (std::size_t j = 0; j < output_frames; ++j) {
        const auto i = static_cast<std::size_t>(pos);
        const auto frac = static_cast<float>(pos - static_cast<double>(i));
        const float* a = frame(i);
        const float* b = frame(i + 1);
        float* out = output + j * ch;
        for (std::uint32_t c = 0; c < ch; ++c) {
            out[c] = a[c] + (b[c] - a[c]) * frac;
        }
        pos += step;
    }

    const auto consumed = std::min(static_cast<std::size_t>(pos), input_frames);

    // New history = extended frames `consumed` and `consumed + 1`. Copy via
    // temporaries: either may alias the current history.
    std::array<float, kMaxChannels> next_older{};
    std::array<float, kMaxChannels> next_newer{};
    std::copy_n(frame(consumed), ch, next_older.begin());
    std::copy_n(frame(consumed + 1), ch, next_newer.begin());
    older_ = next_older;
    newer_ = next_newer;

    position_ = pos - static_cast<double>(consumed);
    if (position_ >= 1.0) {
        // Only reachable with insufficient input (an upstream bug): keep the
        // state well-formed instead of drifting further.
        position_ -= std::floor(position_);
    }
}

}  // namespace somike::dsp
