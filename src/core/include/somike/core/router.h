#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "somike/common/mapped_file.h"
#include "somike/common/unique_handle.h"
#include "somike/core/loopback_capture.h"
#include "somike/dsp/pump_scheduler.h"
#include "somike/dsp/ring_writer.h"

namespace somike::dsp {
class VoiceMixer;
}

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
//
//   LoopbackCapture (source app) --+
//                                  +--> RingWriter (shared ring) --> APO in audiodg
//   sound source (soundboard) -----+
//
// The ring has exactly one writer at a time (write_mutex_):
//   * while the captured app delivers audio, the capture thread writes, and
//     mixes the sound source into every block: the app's clock drives the ring;
//   * otherwise a pump thread writes the sound source, paced by the system
//     clock, so sounds reach the microphone even when no app is routed.
// Gains are applied here, before the ring; the ring's own inject gain stays 1.
//
// Start/Stop/SetGains/SetSoundSource/KickSoundSource are thread-safe.
class Router final : private ICaptureSink {
public:
    Router();
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Fails with HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) if the injector has
    // never been installed (no shared ring).
    [[nodiscard]] HRESULT Start(const RouteConfig& config);
    void Stop() noexcept;
    void SetGains(float inject_gain, bool mute_microphone) noexcept;

    // The soundboard. nullptr detaches; once this returns, the router no
    // longer touches the previous source, which may then be destroyed.
    void SetSoundSource(dsp::VoiceMixer* source) noexcept;
    // Call after starting a sound: opens the ring if needed and makes sure the
    // sound reaches the microphone. Returns the ring error when Somike is not
    // set up yet; the sound then still runs (and ends) in real time, silently.
    HRESULT KickSoundSource();

    [[nodiscard]] RouterStatus status() const noexcept;

    // Highest absolute sample captured since the previous call (0..1+), for a
    // level meter. Resets on read.
    [[nodiscard]] float TakePeak() noexcept { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    [[nodiscard]] bool routing() const noexcept { return state_.load() == RouterState::Routing; }

    // Diagnostics published by the APO; available whenever the ring exists.
    [[nodiscard]] std::optional<dsp::ConsumerSnapshot> ConsumerStatus();

private:
    HRESULT EnsureRing();  // control_mutex_ held
    void StartPump();      // control_mutex_ held
    void StopPump() noexcept;
    void PumpThread();

    // The members below are guarded by write_mutex_.
    bool PumpOnce(double now_ms);  // returns true while it needs to keep ticking
    void WriteBlock(const float* frames, std::size_t count);
    void UpdateRingGains();
    void EndSessionUnlessSounding(double now_ms);

    void OnAudio(const float* frames, std::size_t frame_count) noexcept override;
    void OnTick() noexcept override;
    void OnCaptureError(HRESULT hr) noexcept override;

    std::mutex control_mutex_;  // serialises Start/Stop/EnsureRing; taken before write_mutex_
    win32::MappedFile ring_file_;
    LoopbackCapture capture_;

    std::mutex write_mutex_;
    dsp::RingWriter writer_;
    dsp::VoiceMixer* sound_ = nullptr;
    dsp::PumpScheduler schedule_;     // when the pump writes, and how much
    std::vector<float> capture_mix_;  // capture thread's block buffer
    std::vector<float> pump_mix_;     // pump thread's block buffer (capacity reserved)
    bool session_open_ = false;
    bool capture_running_ = false;
    float app_gain_ = 1.0f;
    bool mute_microphone_ = false;

    std::thread pump_;
    const win32::UniqueHandle pump_wake_;  // auto-reset; created once, never replaced
    const win32::UniqueHandle pump_stop_;  // manual-reset

    std::atomic<RouterState> state_{RouterState::Stopped};
    std::atomic<HRESULT> error_{S_OK};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<float> peak_{0.0f};
};

}  // namespace somike::core
