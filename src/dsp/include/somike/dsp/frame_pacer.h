#pragma once

#include <cstddef>
#include <cstdint>

namespace somike::dsp {

// Turns wall-clock time into a frame budget, for a producer without an audio
// clock of its own: the soundboard while no app audio drives the ring.
//
// The pacer remembers when it was last re-anchored and how many frames were
// produced since, and answers "how many frames are due now". Re-anchoring on
// every block another clock delivers (the loopback capture) makes the pacer
// continue seamlessly from there once that clock stops: the frames of the gap
// are due at once, so the ring timeline has no hole.
class FramePacer {
public:
    explicit FramePacer(double frames_per_ms = 48.0) noexcept : frames_per_ms_(frames_per_ms) {}

    void Reset(double now_ms) noexcept {
        origin_ms_ = now_ms;
        produced_ = 0;
    }

    // Frames due at `now_ms`, at most `max_frames`. When further behind than
    // that (a stalled thread), the excess is forgotten instead of caught up.
    [[nodiscard]] std::size_t Due(double now_ms, std::size_t max_frames) noexcept {
        const double expected = (now_ms - origin_ms_) * frames_per_ms_;
        const double due = expected - static_cast<double>(produced_);
        if (due <= 0.0) {
            return 0;
        }
        if (due > static_cast<double>(max_frames)) {
            // Move the origin so that exactly `max_frames` are due.
            origin_ms_ = now_ms - (static_cast<double>(produced_ + max_frames) / frames_per_ms_);
            return max_frames;
        }
        return static_cast<std::size_t>(due);
    }

    void Produced(std::size_t frames) noexcept { produced_ += frames; }

private:
    double frames_per_ms_;
    double origin_ms_ = 0.0;
    std::uint64_t produced_ = 0;
};

}  // namespace somike::dsp
