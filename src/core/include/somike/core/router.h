#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

#include "somike/common/mapped_file.h"
#include "somike/core/loopback_capture.h"
#include "somike/dsp/ring_writer.h"

namespace somike::core {

struct RouteConfig {
    CaptureTarget target;
    float inject_gain = 1.0f;      // linear gain of the routed audio
    bool mute_microphone = false;  // silence the real microphone while routing
};

enum class RouterState { Stopped, Routing, Failed };

struct RouterStatus {
    RouterState state = RouterState::Stopped;
    HRESULT error = S_OK;  // reason for RouterState::Failed
    std::uint64_t frames_written = 0;
};

// Producer side of the routing pipeline:
//   LoopbackCapture (source app) -> RingWriter (shared ring) -> APO in audiodg
//
// Start/Stop/SetGains are thread-safe; the capture thread only ever touches
// the ring writer and atomics.
class Router final : private ICaptureSink {
public:
    Router() = default;
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Fails with HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) if the injector has
    // never been installed (no shared ring).
    [[nodiscard]] HRESULT Start(const RouteConfig& config);
    void Stop() noexcept;
    void SetGains(float inject_gain, bool mute_microphone) noexcept;

    [[nodiscard]] RouterStatus status() const noexcept;

    // Highest absolute sample captured since the previous call (0..1+), for a
    // level meter. Resets on read.
    [[nodiscard]] float TakePeak() noexcept { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    [[nodiscard]] bool routing() const noexcept { return state_.load() == RouterState::Routing; }

    // Diagnostics published by the APO; available whenever the ring exists.
    [[nodiscard]] std::optional<dsp::ConsumerSnapshot> ConsumerStatus();

private:
    HRESULT EnsureRing();

    void OnAudio(const float* frames, std::size_t frame_count) noexcept override;
    void OnTick() noexcept override;
    void OnCaptureError(HRESULT hr) noexcept override;

    std::mutex control_mutex_;  // serialises Start/Stop/EnsureRing
    win32::MappedFile ring_file_;
    dsp::RingWriter writer_;
    LoopbackCapture capture_;

    std::atomic<RouterState> state_{RouterState::Stopped};
    std::atomic<HRESULT> error_{S_OK};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<float> peak_{0.0f};
};

}  // namespace somike::core
