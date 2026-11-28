#include "somike/dsp/canonical_convert.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "somike/protocol/shared_ring.h"

namespace somike::dsp {

namespace {

constexpr std::size_t kPhases = 512;  // sub-sample positions in the filter table

double Sinc(double x) {
    if (std::abs(x) < 1e-9) {
        return 1.0;
    }
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

double Blackman(double x, double half_width) {
    // x in [-half_width, half_width] -> window value; 0 outside.
    if (std::abs(x) >= half_width) {
        return 0.0;
    }
    const double n = (x + half_width) / (2.0 * half_width);  // 0..1
    return 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * n) + 0.08 * std::cos(4.0 * std::numbers::pi * n);
}

float Clean(float sample) { return std::isfinite(sample) ? sample : 0.0f; }

}  // namespace

std::vector<float> ToStereo(std::span<const float> interleaved, std::uint32_t channels) {
    if (channels == 0) {
        return {};
    }
    const std::size_t frames = interleaved.size() / channels;
    std::vector<float> out(frames * 2);
    constexpr float kCentre = 0.70710678f;
    for (std::size_t f = 0; f < frames; ++f) {
        const float* in = interleaved.data() + f * channels;
        float left = 0.0f;
        float right = 0.0f;
        if (channels == 1) {
            left = right = Clean(in[0]);
        } else {
            left = Clean(in[0]);
            right = Clean(in[1]);
            if (channels >= 3) {
                left += kCentre * Clean(in[2]);
                right += kCentre * Clean(in[2]);
            }
        }
        out[2 * f] = left;
        out[2 * f + 1] = right;
    }
    return out;
}

std::vector<float> ResampleStereo(std::span<const float> stereo, std::uint32_t from_rate, std::uint32_t to_rate) {
    const std::size_t in_frames = stereo.size() / 2;
    if (from_rate == 0 || to_rate == 0 || in_frames == 0) {
        return {};
    }
    if (from_rate == to_rate) {
        return {stereo.begin(), stereo.begin() + static_cast<std::ptrdiff_t>(in_frames * 2)};
    }

    const double ratio = static_cast<double>(to_rate) / from_rate;  // output per input frame
    // Low-pass at the lower Nyquist (slightly inside, for the transition band).
    const double cutoff = 0.5 * std::min(1.0, ratio) * 0.95;  // cycles per input frame
    // Kernel half-width in input frames: 16 zero crossings, widened when
    // downsampling so the filter keeps its shape relative to the cutoff.
    const int half = static_cast<int>(std::ceil(16.0 / std::min(1.0, ratio)));
    const int taps = 2 * half;

    // table[p][k]: weight of input frame (base - half + 1 + k) for an output
    // position `p / kPhases` input frames after `base`.
    std::vector<float> table((kPhases + 1) * static_cast<std::size_t>(taps));
    for (std::size_t p = 0; p <= kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        float* row = table.data() + p * static_cast<std::size_t>(taps);
        for (int k = 0; k < taps; ++k) {
            const double distance = frac - static_cast<double>(k - half + 1);
            const double w = 2.0 * cutoff * Sinc(2.0 * cutoff * distance) * Blackman(distance, half);
            row[k] = static_cast<float>(w);
            sum += w;
        }
        // Normalise each phase to unity DC gain (no level change, no ripple
        // between phases).
        if (sum != 0.0) {
            for (int k = 0; k < taps; ++k) {
                row[k] = static_cast<float>(row[k] / sum);
            }
        }
    }

    const auto out_frames = static_cast<std::size_t>(std::floor(static_cast<double>(in_frames) * ratio));
    std::vector<float> out(out_frames * 2);
    const auto last = static_cast<std::ptrdiff_t>(in_frames) - 1;
    for (std::size_t o = 0; o < out_frames; ++o) {
        const double position = static_cast<double>(o) / ratio;
        const auto base = static_cast<std::ptrdiff_t>(std::floor(position));
        const auto phase = static_cast<std::size_t>(std::lround((position - static_cast<double>(base)) * kPhases));
        const float* row = table.data() + phase * static_cast<std::size_t>(taps);
        float left = 0.0f;
        float right = 0.0f;
        for (int k = 0; k < taps; ++k) {
            const std::ptrdiff_t i = base - half + 1 + k;
            if (i < 0 || i > last) {
                continue;  // silence outside the clip
            }
            left += row[k] * stereo[static_cast<std::size_t>(i) * 2];
            right += row[k] * stereo[static_cast<std::size_t>(i) * 2 + 1];
        }
        out[2 * o] = left;
        out[2 * o + 1] = right;
    }
    return out;
}

std::vector<float> ConvertToCanonical(std::span<const float> interleaved, std::uint32_t channels,
                                      std::uint32_t sample_rate) {
    if (channels == 0 || sample_rate == 0 || interleaved.empty()) {
        return {};
    }
    std::vector<float> stereo = ToStereo(interleaved, channels);
    if (sample_rate == protocol::kSampleRate) {
        return stereo;
    }
    return ResampleStereo(stereo, sample_rate, protocol::kSampleRate);
}

}  // namespace somike::dsp
