#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace somike::dsp {

// Snapshot of the diagnostics block written by APO instances.
struct ConsumerSnapshot {
    std::uint64_t heartbeat_ms = 0;
    std::uint64_t frames_injected = 0;
    std::uint64_t underruns = 0;
    std::uint64_t resyncs = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

// Read-only view of the whole ring state, for diagnostics.
struct RingSnapshot {
    bool compatible = false;  // header matches this build's protocol
    bool producer_streaming = false;
    std::uint64_t producer_heartbeat_ms = 0;
    std::uint64_t write_frame = 0;
    std::uint32_t session = 0;
    ConsumerSnapshot consumer;
};

// Never writes; safe on a read-only mapping.
[[nodiscard]] RingSnapshot InspectRing(std::span<const std::byte> region) noexcept;

// Producer side of the shared ring (lives in the Somike app).
//
// Exactly one thread may call Write(); the other methods only touch
// independent atomic fields and may be called from any thread.
class RingWriter {
public:
    RingWriter() = default;

    // `region` must span at least protocol::kRegionBytes, be 64-byte aligned
    // and outlive the writer. An incompatible header is re-initialised.
    explicit RingWriter(std::span<std::byte> region) noexcept;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }

    // Starts a new session: consumers re-prime and begin mixing.
    void BeginSession(std::uint64_t now_ms) noexcept;

    // Stops mixing; consumers pass the microphone through untouched.
    void EndSession(std::uint64_t now_ms) noexcept;

    // Appends canonical frames (48 kHz, stereo, float32, interleaved).
    void Write(const float* interleaved, std::size_t frames, std::uint64_t now_ms) noexcept;

    // Signals liveness without audio (e.g. while the source is silent).
    void Heartbeat(std::uint64_t now_ms) noexcept;

    void SetGains(float inject_gain, float mic_gain) noexcept;

    [[nodiscard]] std::uint64_t write_frame() const noexcept;
    [[nodiscard]] ConsumerSnapshot ReadConsumer() const noexcept;

private:
    std::byte* base_ = nullptr;
};

}  // namespace somike::dsp
