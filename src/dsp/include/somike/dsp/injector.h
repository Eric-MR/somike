#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "somike/dsp/linear_resampler.h"
#include "somike/dsp/ring_reader.h"

namespace somike::dsp {

struct EndpointFormat {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

struct InjectorConfig {
    ReaderConfig reader{};
    // Drift controller: relative rate correction per second of excess backlog.
    // 10 ms too much backlog -> consume 0.1 % faster.
    double drift_gain = 0.1;
    double max_drift_correction = 0.005;  // +-0.5 %, inaudible pitch change
    // Exponential smoothing factor for the backlog estimate (per block).
    double backlog_smoothing = 0.02;
    // While idle, refresh the diagnostics heartbeat at most this often.
    std::uint64_t idle_diagnostics_interval_ms = 1000;
};

inline constexpr std::uint32_t kMaxBlockFrames = 1u << 16;

[[nodiscard]] bool IsSupportedFormat(const EndpointFormat& format) noexcept;

// The platform-independent brain of the capture APO: pulls canonical audio
// from the shared ring, converts it to the endpoint format (rate + drift via
// the resampler, channel layout via the mixer) and mixes it into the
// microphone buffer in place.
//
// Lifecycle (mirrors the APO): Configure()/Attach() outside the real-time
// thread, then Process() once per engine period on the real-time thread.
class Injector {
public:
    explicit Injector(InjectorConfig config = {});

    // Non-real-time; allocates scratch buffers. Returns false for unsupported
    // formats. May throw std::bad_alloc.
    bool Configure(const EndpointFormat& format, std::uint32_t max_frames);

    // Non-real-time. With `writable` the diagnostics block is updated.
    void Attach(std::span<std::byte> region, bool writable) noexcept;
    void Detach() noexcept;

    // Drops all stream state (keeps configuration).
    void Reset() noexcept;

    // Real-time safe. Returns true when `io` was modified.
    bool Process(float* io, std::uint32_t frames, bool input_silent,
                 std::uint64_t now_ms) noexcept;

    [[nodiscard]] bool configured() const noexcept { return max_frames_ != 0; }
    [[nodiscard]] bool attached() const noexcept { return reader_.attached(); }
    [[nodiscard]] bool active() const noexcept { return was_active_; }
    [[nodiscard]] double last_step() const noexcept { return last_step_; }
    [[nodiscard]] const EndpointFormat& format() const noexcept { return format_; }

private:
    [[nodiscard]] double ComputeStep() const noexcept;
    void PublishDiagnostics(std::uint64_t now_ms, std::uint32_t injected_frames) noexcept;

    InjectorConfig config_;
    RingReader reader_;
    LinearResampler resampler_{2};
    EndpointFormat format_{};
    std::uint32_t max_frames_ = 0;
    std::size_t max_source_frames_ = 0;
    std::vector<float> source_;     // canonical frames pulled from the ring
    std::vector<float> resampled_;  // canonical channel layout at endpoint rate

    std::byte* diagnostics_ = nullptr;  // region base when writable
    std::uint64_t last_diagnostics_ms_ = 0;
    std::uint64_t published_underruns_ = 0;
    std::uint64_t published_resyncs_ = 0;

    bool was_active_ = false;
    double smoothed_backlog_ = 0.0;
    double last_step_ = 1.0;
};

}  // namespace somike::dsp
