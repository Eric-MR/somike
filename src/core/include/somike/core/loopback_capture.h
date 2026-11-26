#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <thread>

#include "somike/common/unique_handle.h"

namespace somike::core {

// Which audio to capture, using the WASAPI process-loopback virtual device
// (Windows 10 build 20348+ / Windows 11).
struct CaptureTarget {
    enum class Mode {
        IncludeProcessTree,  // only this process and its children
        ExcludeProcessTree,  // the audio of all other processes
    };

    DWORD process_id = 0;
    Mode mode = Mode::IncludeProcessTree;

    [[nodiscard]] static CaptureTarget Application(DWORD pid) noexcept {
        return {pid, Mode::IncludeProcessTree};
    }
    [[nodiscard]] static CaptureTarget EverythingExcept(DWORD pid) noexcept {
        return {pid, Mode::ExcludeProcessTree};
    }
};

// Receives captured audio on the capture thread.
class ICaptureSink {
public:
    // Canonical frames: 48 kHz, stereo, float32, interleaved.
    virtual void OnAudio(const float* frames, std::size_t frame_count) noexcept = 0;
    // Called at least every ~100 ms, with or without audio (liveness).
    virtual void OnTick() noexcept = 0;
    // The stream failed (e.g. device removed); the capture thread exits.
    virtual void OnCaptureError(HRESULT hr) noexcept = 0;

protected:
    ~ICaptureSink() = default;
};

// Captures one CaptureTarget on a dedicated MMCSS ("Audio") thread and
// delivers canonical frames to an ICaptureSink. The audio engine converts
// whatever the source plays to the canonical format for us.
class LoopbackCapture {
public:
    LoopbackCapture() = default;
    ~LoopbackCapture() { Stop(); }
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

    // Blocks until the stream is running (S_OK) or failed to start.
    // `sink` must outlive the capture (until Stop() returns).
    [[nodiscard]] HRESULT Start(const CaptureTarget& target, ICaptureSink& sink);
    void Stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    win32::UniqueHandle stop_event_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace somike::core
