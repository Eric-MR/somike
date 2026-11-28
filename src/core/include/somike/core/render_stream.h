#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "somike/common/unique_handle.h"

namespace somike::core {

// Pull source for RenderStream, called on the render thread.
class IRenderSource {
public:
    // Adds `frames` canonical frames (48 kHz stereo float, interleaved) into
    // `out`, which arrives zeroed. Returns false while there is nothing to play.
    virtual bool Render(float* out, std::size_t frames) noexcept = 0;

protected:
    ~IRenderSource() = default;
};

// Plays a pull source on the default output device, i.e. what the user hears
// (the soundboard's "hear it myself"). Shared mode at the canonical format;
// the audio engine converts to whatever the device runs at.
//
// Start() opens the stream on a background thread and returns at once. The
// stream closes by itself after `idle_ms` without sound, so an idle
// soundboard keeps no device open, and the next Start() follows a changed
// default device.
class RenderStream {
public:
    explicit RenderStream(std::uint32_t idle_ms = 3000) noexcept : idle_ms_(idle_ms) {}
    ~RenderStream() { Stop(); }
    RenderStream(const RenderStream&) = delete;
    RenderStream& operator=(const RenderStream&) = delete;

    // `source` must stay valid until Stop() returns (or the destructor runs).
    void Start(IRenderSource& source);
    void Stop() noexcept;

private:
    void Run(IRenderSource* source, HANDLE stop_event);
    // Render thread: true when the idle stream may close (no Start() came in).
    bool FinishIfIdle();

    std::uint32_t idle_ms_;
    std::mutex mutex_;
    std::thread thread_;
    win32::UniqueHandle stop_event_;
    bool running_ = false;  // guarded by mutex_
    bool kicked_ = false;   // guarded by mutex_: Start() while running
};

}  // namespace somike::core
