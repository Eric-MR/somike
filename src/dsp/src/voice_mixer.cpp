#include "somike/dsp/voice_mixer.h"

#include <algorithm>
#include <cmath>

namespace somike::dsp {

namespace {

constexpr float kMaxGain = 4.0f;

float CleanGain(float gain) noexcept {
    if (!std::isfinite(gain)) {
        return 0.0f;
    }
    return std::clamp(gain, 0.0f, kMaxGain);
}

}  // namespace

void VoiceMixer::Play(std::uint32_t sound_id, ClipPtr clip, float gain) {
    if (!clip || clip->frames() == 0) {
        return;
    }
    std::scoped_lock lock(mutex_);
    std::erase_if(voices_, [&](const Voice& v) { return v.sound_id == sound_id; });
    if (voices_.size() >= kMaxVoices) {
        const auto oldest = std::min_element(voices_.begin(), voices_.end(),
                                             [](const Voice& a, const Voice& b) { return a.started < b.started; });
        voices_.erase(oldest);
    }
    Voice voice;
    voice.sound_id = sound_id;
    voice.clip = std::move(clip);
    voice.gain = CleanGain(gain);
    voice.started = ++play_counter_;
    voices_.push_back(std::move(voice));
}

void VoiceMixer::Stop(std::uint32_t sound_id) {
    std::scoped_lock lock(mutex_);
    for (Voice& voice : voices_) {
        if (voice.sound_id == sound_id && voice.fade_left == 0) {
            voice.fade_left = kFadeFrames;
        }
    }
}

void VoiceMixer::StopAll() {
    std::scoped_lock lock(mutex_);
    for (Voice& voice : voices_) {
        if (voice.fade_left == 0) {
            voice.fade_left = kFadeFrames;
        }
    }
}

void VoiceMixer::SetVoiceGain(std::uint32_t sound_id, float gain) {
    std::scoped_lock lock(mutex_);
    for (Voice& voice : voices_) {
        if (voice.sound_id == sound_id) {
            voice.gain = CleanGain(gain);
        }
    }
}

void VoiceMixer::SetMasterGain(float gain) {
    std::scoped_lock lock(mutex_);
    master_target_ = CleanGain(gain);
}

bool VoiceMixer::MixInto(float* io, std::size_t frames) {
    if (io == nullptr || frames == 0) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (voices_.empty()) {
        master_current_ = master_target_;
        return false;
    }

    const float master_start = master_current_;
    const float master_step = (master_target_ - master_start) / static_cast<float>(frames);
    bool contributed = false;

    for (Voice& voice : voices_) {
        const std::size_t length = voice.clip->frames();
        const float* samples = voice.clip->samples.data();
        std::size_t count = std::min(frames, length - voice.position);
        if (voice.fade_left > 0) {
            count = std::min(count, voice.fade_left);
        }
        for (std::size_t f = 0; f < count; ++f) {
            float gain = voice.gain * (master_start + master_step * static_cast<float>(f));
            if (voice.fade_left > 0) {
                gain *= static_cast<float>(voice.fade_left - f) / static_cast<float>(kFadeFrames);
            }
            const std::size_t src = (voice.position + f) * 2;
            io[2 * f] += samples[src] * gain;
            io[2 * f + 1] += samples[src + 1] * gain;
        }
        voice.position += count;
        if (voice.fade_left > 0) {
            voice.fade_left -= count;
            if (voice.fade_left == 0) {
                voice.position = length;  // fade finished: the voice is done
            }
        }
        contributed = contributed || count > 0;
    }

    master_current_ = master_target_;
    std::erase_if(voices_, [](const Voice& v) { return v.position >= v.clip->frames(); });
    return contributed;
}

bool VoiceMixer::active() const {
    std::scoped_lock lock(mutex_);
    return !voices_.empty();
}

bool VoiceMixer::IsPlaying(std::uint32_t sound_id) const {
    std::scoped_lock lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) {
        return v.sound_id == sound_id && v.fade_left == 0;
    });
}

std::vector<VoiceInfo> VoiceMixer::Snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<VoiceInfo> result;
    result.reserve(voices_.size());
    for (const Voice& voice : voices_) {
        if (voice.fade_left == 0) {
            result.push_back({voice.sound_id, voice.position, voice.clip->frames()});
        }
    }
    return result;
}

}  // namespace somike::dsp
