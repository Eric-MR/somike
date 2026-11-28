#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace somike::dsp {

// Decoded audio in the canonical ring format: 48 kHz, stereo, float32,
// interleaved. Immutable once shared, so several mixers can play one clip.
struct Clip {
    std::vector<float> samples;
    [[nodiscard]] std::size_t frames() const noexcept { return samples.size() / 2; }
    [[nodiscard]] std::size_t bytes() const noexcept { return samples.size() * sizeof(float); }
};
using ClipPtr = std::shared_ptr<const Clip>;

struct VoiceInfo {
    std::uint32_t sound_id = 0;
    std::size_t position = 0;  // frames already played
    std::size_t length = 0;    // frames in the clip
};

// The soundboard's mixer: any number of clips at once, one voice per sound.
//
// Control (Play/Stop/gains) comes from the UI thread, MixInto() from an
// audio thread; a short mutex serialises them. This runs in the app process,
// never inside audiodg.exe, so a lock on the producer side is acceptable.
class VoiceMixer {
public:
    static constexpr std::size_t kMaxVoices = 16;
    static constexpr std::size_t kFadeFrames = 240;  // 5 ms fade on stop: no click

    // Starts `clip` for `sound_id`, restarting it if it is already playing.
    // With kMaxVoices voices busy, the oldest one is dropped.
    void Play(std::uint32_t sound_id, ClipPtr clip, float gain);
    // Fades the sound out. Unknown ids are ignored.
    void Stop(std::uint32_t sound_id);
    void StopAll();
    void SetVoiceGain(std::uint32_t sound_id, float gain);
    // Overall volume; changes are ramped over one block (no zipper noise).
    void SetMasterGain(float gain);

    // Adds the next `frames` frames of every voice into `io` (interleaved
    // stereo) and advances them. Returns true if any voice contributed.
    bool MixInto(float* io, std::size_t frames);

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool IsPlaying(std::uint32_t sound_id) const;
    [[nodiscard]] std::vector<VoiceInfo> Snapshot() const;

private:
    struct Voice {
        std::uint32_t sound_id = 0;
        ClipPtr clip;
        std::size_t position = 0;
        float gain = 1.0f;
        std::size_t fade_left = 0;  // > 0: fading out, frames remaining
        std::uint64_t started = 0;  // order of Play() calls, to drop the oldest
    };

    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::uint64_t play_counter_ = 0;
    float master_target_ = 1.0f;
    float master_current_ = 1.0f;
};

}  // namespace somike::dsp
