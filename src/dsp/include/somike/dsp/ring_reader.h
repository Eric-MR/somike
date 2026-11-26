#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace somike::dsp {

struct ReaderConfig {
    // Latency the reader aims for, in canonical (48 kHz) frames.
    std::uint32_t target_latency_frames = 1920;  // 40 ms
    // Beyond this backlog the reader jumps forward to the target latency.
    std::uint32_t max_latency_frames = 7200;  // 150 ms
    // Producer heartbeat older than this means "producer is gone".
    std::uint64_t producer_timeout_ms = 500;
};

struct ReadResult {
    std::size_t frames = 0;  // frames copied into the output buffer
    bool active = false;     // producer is streaming and alive
    float inject_gain = 0.0f;
    float mic_gain = 1.0f;  // 1.0 whenever inactive: never mute a mic by accident
};

// Consumer side of the shared ring (lives in the APO, one per stream).
//
// Every reader keeps its own read position, so any number of readers can
// consume the same ring independently and the shared memory never needs to
// be written for control purposes. All shared data is treated as untrusted:
// indices are masked with compile-time constants and gains are sanitised.
//
// Real-time safe: no allocation, no locks, no system calls.
class RingReader {
public:
    RingReader() = default;
    explicit RingReader(ReaderConfig config) noexcept : config_(config) {}

    // Returns false (and stays detached) if the region is too small or misaligned.
    bool Attach(std::span<std::byte> region) noexcept;
    void Detach() noexcept;
    [[nodiscard]] bool attached() const noexcept { return base_ != nullptr; }

    // Copies up to `frames` canonical frames into `out` (interleaved stereo).
    // Frames beyond ReadResult::frames are left untouched.
    ReadResult Read(float* out, std::size_t frames, std::uint64_t now_ms) noexcept;

    // Forget stream position; the next Read() re-primes.
    void Reset() noexcept;

    [[nodiscard]] bool running() const noexcept { return phase_ == Phase::Running; }
    [[nodiscard]] std::uint64_t backlog() const noexcept { return backlog_; }
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_; }
    [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }
    [[nodiscard]] const ReaderConfig& config() const noexcept { return config_; }

private:
    enum class Phase { Idle, Priming, Running };

    void CopyOut(float* out, std::uint64_t from_frame, std::size_t frames) const noexcept;

    ReaderConfig config_{};
    std::byte* base_ = nullptr;
    Phase phase_ = Phase::Idle;
    std::uint32_t session_ = 0;
    std::uint64_t read_ = 0;
    std::uint64_t backlog_ = 0;
    std::uint64_t underruns_ = 0;
    std::uint64_t resyncs_ = 0;
};

}  // namespace somike::dsp
