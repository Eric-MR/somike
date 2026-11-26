#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace somike::dsp {

// Streaming linear-interpolation resampler with a per-call step, used both
// for sample-rate conversion and for clock-drift compensation.
//
// `step` is the number of input frames advanced per output frame
// (input_rate / output_rate, optionally nudged by the drift controller).
//
// Two frames of history are kept, so the number of input frames a block
// needs is exactly the number it consumes: callers can pull precisely
// InputFramesRequired() frames from a stream without ever losing or
// re-reading one. The price is a fixed two-frame delay (~42 us at 48 kHz).
//
// Quality note: linear interpolation without an anti-aliasing filter is
// adequate for voice-chat paths at ratios near 1.0. A polyphase/windowed-sinc
// implementation can replace this class without touching its callers.
class LinearResampler {
public:
    static constexpr std::uint32_t kMaxChannels = 8;

    explicit LinearResampler(std::uint32_t channels = 2) noexcept;

    void Reset() noexcept;

    // Input frames Process() consumes to produce `output_frames` frames.
    [[nodiscard]] std::size_t InputFramesRequired(double step,
                                                  std::size_t output_frames) const noexcept;

    // Produces exactly `output_frames` frames. `input` should hold
    // InputFramesRequired(step, output_frames) frames; if it holds fewer, the
    // last available frame is repeated (never reads out of bounds).
    void Process(const float* input, std::size_t input_frames, float* output,
                 std::size_t output_frames, double step) noexcept;

    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

private:
    std::uint32_t channels_;
    double position_ = 0.0;                     // next output, relative to older_, in [0, 1)
    std::array<float, kMaxChannels> older_{};   // extended frame 0
    std::array<float, kMaxChannels> newer_{};   // extended frame 1
};

}  // namespace somike::dsp
