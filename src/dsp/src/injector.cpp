#include "somike/dsp/injector.h"

#include <algorithm>
#include <cmath>

#include "somike/dsp/detail/atomic_field.h"
#include "somike/dsp/mixer.h"
#include "somike/protocol/shared_ring.h"

namespace somike::dsp {

namespace proto = somike::protocol;
using namespace detail;

bool IsSupportedFormat(const EndpointFormat& format) noexcept {
    return format.sample_rate >= 8000 && format.sample_rate <= 384000 && format.channels >= 1 &&
           format.channels <= 32;
}

Injector::Injector(InjectorConfig config) : config_(config), reader_(config.reader) {}

bool Injector::Configure(const EndpointFormat& format, std::uint32_t max_frames) {
    max_frames_ = 0;
    if (!IsSupportedFormat(format) || max_frames == 0 || max_frames > kMaxBlockFrames) {
        return false;
    }
    const double max_step = static_cast<double>(proto::kSampleRate) / format.sample_rate *
                            (1.0 + config_.max_drift_correction);
    max_source_frames_ =
        static_cast<std::size_t>(std::ceil(static_cast<double>(max_frames) * max_step)) + 4;

    source_.assign(max_source_frames_ * proto::kChannels, 0.0f);
    resampled_.assign(static_cast<std::size_t>(max_frames) * proto::kChannels, 0.0f);
    format_ = format;
    max_frames_ = max_frames;
    Reset();
    return true;
}

void Injector::Attach(std::span<std::byte> region, bool writable) noexcept {
    if (reader_.Attach(region)) {
        diagnostics_ = writable ? region.data() : nullptr;
    } else {
        diagnostics_ = nullptr;
    }
    Reset();
}

void Injector::Detach() noexcept {
    reader_.Detach();
    diagnostics_ = nullptr;
    Reset();
}

void Injector::Reset() noexcept {
    reader_.Reset();
    resampler_.Reset();
    was_active_ = false;
    smoothed_backlog_ = static_cast<double>(config_.reader.target_latency_frames);
    last_step_ = format_.sample_rate != 0
                     ? static_cast<double>(proto::kSampleRate) / format_.sample_rate
                     : 1.0;
}

double Injector::ComputeStep() const noexcept {
    const double nominal = static_cast<double>(proto::kSampleRate) / format_.sample_rate;
    if (!reader_.running()) {
        return nominal;
    }
    // Proportional controller on the smoothed backlog: consume slightly
    // faster when the producer's clock runs ahead of the microphone's, and
    // slightly slower when it lags. Keeps latency near the target without
    // audible skips.
    const double target = static_cast<double>(config_.reader.target_latency_frames);
    const double excess_seconds = (smoothed_backlog_ - target) / proto::kSampleRate;
    const double correction = std::clamp(config_.drift_gain * excess_seconds,
                                         -config_.max_drift_correction,
                                         config_.max_drift_correction);
    return nominal * (1.0 + correction);
}

bool Injector::Process(float* io, std::uint32_t frames, bool input_silent,
                       std::uint64_t now_ms) noexcept {
    if (io == nullptr || frames == 0 || frames > max_frames_ || !reader_.attached()) {
        return false;
    }

    const double step = ComputeStep();
    const std::size_t needed = resampler_.InputFramesRequired(step, frames);
    if (needed > max_source_frames_) {
        return false;
    }

    const ReadResult read = reader_.Read(source_.data(), needed, now_ms);
    if (!read.active) {
        if (was_active_) {
            was_active_ = false;
            resampler_.Reset();
        }
        PublishDiagnostics(now_ms, 0);
        return false;  // pass the microphone through untouched
    }

    if (!was_active_) {
        was_active_ = true;
        resampler_.Reset();
        smoothed_backlog_ = static_cast<double>(config_.reader.target_latency_frames);
    }

    // Underrun or priming: pad with silence so the resampler stays continuous.
    if (read.frames < needed) {
        std::fill(source_.begin() + static_cast<std::ptrdiff_t>(read.frames * proto::kChannels),
                  source_.begin() + static_cast<std::ptrdiff_t>(needed * proto::kChannels), 0.0f);
    }

    resampler_.Process(source_.data(), needed, resampled_.data(), frames, step);
    last_step_ = step;

    if (reader_.running()) {
        smoothed_backlog_ +=
            config_.backlog_smoothing * (static_cast<double>(reader_.backlog()) - smoothed_backlog_);
    } else {
        smoothed_backlog_ = static_cast<double>(config_.reader.target_latency_frames);
    }

    MixInto(io, frames, format_.channels, resampled_.data(), read.inject_gain, read.mic_gain,
            input_silent);
    PublishDiagnostics(now_ms, read.frames > 0 ? frames : 0);
    return true;
}

void Injector::PublishDiagnostics(std::uint64_t now_ms, std::uint32_t injected_frames) noexcept {
    if (diagnostics_ == nullptr) {
        return;
    }
    // Throttle while idle so a merely-open microphone does not keep dirtying
    // the shared page every engine period.
    if (!was_active_ && now_ms - last_diagnostics_ms_ < config_.idle_diagnostics_interval_ms &&
        last_diagnostics_ms_ != 0) {
        return;
    }
    last_diagnostics_ms_ = now_ms;

    proto::ConsumerBlock& c = proto::HeaderOf(diagnostics_)->consumer;
    StoreRelaxed(c.sample_rate, format_.sample_rate);
    StoreRelaxed(c.channels, format_.channels);
    if (injected_frames != 0) {
        AddRelaxed(c.frames_injected, std::uint64_t{injected_frames});
    }
    if (reader_.underruns() != published_underruns_) {
        AddRelaxed(c.underruns, reader_.underruns() - published_underruns_);
        published_underruns_ = reader_.underruns();
    }
    if (reader_.resyncs() != published_resyncs_) {
        AddRelaxed(c.resyncs, reader_.resyncs() - published_resyncs_);
        published_resyncs_ = reader_.resyncs();
    }
    StoreRelease(c.heartbeat_ms, now_ms);
}

}  // namespace somike::dsp
