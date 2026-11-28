#pragma once

#include <algorithm>
#include <cstddef>

#include "somike/dsp/frame_pacer.h"

namespace somike::dsp {

struct PumpConfig {
    // After the last sound ended, the session keeps streaming silence this
    // long: the end of the sound drains from the ring, and a quick follow-up
    // sound needs no new lead-in.
    double tail_ms = 1500.0;
    // The capture thread counts as the clock while its last block is this recent.
    double capture_grace_ms = 30.0;
    // A pause this long means the APO's reader has run dry and will re-prime.
    double gap_ms = 60.0;
    // Silence written (in real time) before a sound that starts after a pause,
    // so the re-priming reader skips silence rather than the start of the sound.
    std::size_t lead_in_frames = 1440;  // 30 ms at 48 kHz
    // Never more than this at once (catching up after a stalled thread).
    std::size_t max_burst_frames = 4800;  // 100 ms
    double frames_per_ms = 48.0;
};

// What the pump thread does on one tick.
struct PumpStep {
    enum class Action {
        Sleep,  // nothing to play: wait for the next kick
        Tick,   // keep ticking, nothing to write right now
        Write,  // write `silence_frames` of silence, then `sound_frames` of the mix
    };
    Action action = Action::Sleep;
    bool begin_session = false;  // open a ring session first
    bool end_session = false;    // Sleep only: close the ring session
    std::size_t silence_frames = 0;
    std::size_t sound_frames = 0;

    [[nodiscard]] std::size_t frames() const noexcept { return silence_frames + sound_frames; }
};

// The timing decisions of the soundboard pump, without I/O or threads so they
// can be tested with a fake clock. The ring has one writer at a time: the
// capture thread while app audio flows (it reports each block here), else the
// pump, paced by the system clock. The caller serialises all calls.
class PumpScheduler {
public:
    explicit PumpScheduler(PumpConfig config = {}) noexcept
        : config_(config), pacer_(config.frames_per_ms) {}

    // A sound was started: stream at least until the tail after `now_ms`.
    void Kick(double now_ms) noexcept { sound_until_ms_ = std::max(sound_until_ms_, now_ms + config_.tail_ms); }

    // Capture started: give its first block the grace period before the pump
    // takes over.
    void OnCaptureStart(double now_ms) noexcept { last_capture_ms_ = now_ms; }

    // The capture thread wrote a block (with the sounds mixed in). If the app
    // falls silent, the pump continues seamlessly from here.
    void OnCaptureBlock(double now_ms) noexcept {
        last_capture_ms_ = now_ms;
        last_write_ms_ = now_ms;
        pacer_.Reset(now_ms);
        lead_in_left_ = 0;
    }

    // True until the tail after the last sound has passed.
    [[nodiscard]] bool Sounding(double now_ms) const noexcept { return now_ms < sound_until_ms_; }

    // One pump tick. `sounding`: voices are playing (or fading); the router's
    // `capture_running` and `session_open` state.
    [[nodiscard]] PumpStep Next(double now_ms, bool sounding, bool capture_running, bool session_open) noexcept {
        if (sounding) {
            Kick(now_ms);
        }
        PumpStep step;
        if (!Sounding(now_ms)) {
            step.action = PumpStep::Action::Sleep;
            step.end_session = session_open && !capture_running;
            return step;
        }
        step.action = PumpStep::Action::Tick;
        if (capture_running && now_ms - last_capture_ms_ < config_.capture_grace_ms) {
            return step;  // the capture thread is the clock and mixes the sounds in
        }
        if (!session_open) {
            step.begin_session = true;
            StartLeadIn(now_ms);
        } else if (now_ms - last_write_ms_ > config_.gap_ms) {
            StartLeadIn(now_ms);
        }
        const std::size_t due = pacer_.Due(now_ms, config_.max_burst_frames);
        if (due == 0) {
            return step;
        }
        step.action = PumpStep::Action::Write;
        step.silence_frames = std::min(due, lead_in_left_);
        step.sound_frames = due - step.silence_frames;
        return step;
    }

    // The caller executed a Write step.
    void Wrote(double now_ms, const PumpStep& step) noexcept {
        pacer_.Produced(step.frames());
        lead_in_left_ -= std::min(lead_in_left_, step.silence_frames);
        last_write_ms_ = now_ms;
    }

private:
    void StartLeadIn(double now_ms) noexcept {
        pacer_.Reset(now_ms);
        lead_in_left_ = config_.lead_in_frames;
        // The new timeline starts now: without this the gap check would fire
        // again on every tick until the first write, re-anchoring the pacer
        // each time so that nothing ever became due.
        last_write_ms_ = now_ms;
    }

    PumpConfig config_;
    FramePacer pacer_;
    double last_write_ms_ = -1e12;
    double last_capture_ms_ = -1e12;
    double sound_until_ms_ = -1e12;
    std::size_t lead_in_left_ = 0;
};

}  // namespace somike::dsp
