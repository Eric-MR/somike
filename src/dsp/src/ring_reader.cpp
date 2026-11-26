#include "somike/dsp/ring_reader.h"

#include <algorithm>
#include <cstring>

#include "somike/dsp/detail/atomic_field.h"
#include "somike/dsp/gain.h"
#include "somike/protocol/shared_ring.h"

namespace somike::dsp {

namespace proto = somike::protocol;
using namespace detail;

bool RingReader::Attach(std::span<std::byte> region) noexcept {
    Detach();
    if (region.size() < proto::kRegionBytes ||
        reinterpret_cast<std::uintptr_t>(region.data()) % 64 != 0) {
        return false;
    }
    base_ = region.data();
    return true;
}

void RingReader::Detach() noexcept {
    base_ = nullptr;
    Reset();
}

void RingReader::Reset() noexcept {
    phase_ = Phase::Idle;
    read_ = 0;
    backlog_ = 0;
}

ReadResult RingReader::Read(float* out, std::size_t frames, std::uint64_t now_ms) noexcept {
    ReadResult result;
    if (base_ == nullptr || out == nullptr) {
        Reset();
        return result;
    }

    const proto::RegionHeader* header = proto::HeaderOf(base_);
    if (LoadAcquire(header->magic) != proto::kMagic || !proto::HeaderIsCompatible(*header)) {
        Reset();
        return result;
    }

    const proto::ProducerBlock& p = header->producer;
    const std::uint32_t state = LoadAcquire(p.state);
    const std::uint64_t heartbeat = LoadAcquire(p.heartbeat_ms);
    const std::uint64_t age = now_ms > heartbeat ? now_ms - heartbeat : 0;
    if (state != static_cast<std::uint32_t>(proto::ProducerState::Streaming) ||
        age > config_.producer_timeout_ms) {
        Reset();
        return result;
    }

    result.active = true;
    result.inject_gain = SanitizeGain(BitsToFloat(LoadRelaxed(p.inject_gain_bits)), 0.0f);
    result.mic_gain = SanitizeGain(BitsToFloat(LoadRelaxed(p.mic_gain_bits)), 1.0f);

    const std::uint32_t session = LoadAcquire(p.session);
    const std::uint64_t write = LoadAcquire(p.write_frame);
    const std::uint64_t target = config_.target_latency_frames;

    // (Re)start from the writer's current position. Starting at `write`
    // (rather than `write - target`) guarantees we never replay stale audio
    // left in the ring by a previous session.
    if (phase_ == Phase::Idle || session != session_ || write < read_) {
        session_ = session;
        read_ = write;
        phase_ = Phase::Priming;
    }

    std::uint64_t available = write - read_;
    if (phase_ == Phase::Priming) {
        if (available < target) {
            backlog_ = available;
            return result;
        }
        phase_ = Phase::Running;
        read_ = write - target;
        available = target;
    }

    if (available > config_.max_latency_frames) {
        read_ = write - target;
        available = target;
        ++resyncs_;
    }

    const std::size_t count =
        static_cast<std::size_t>(std::min<std::uint64_t>(frames, available));
    CopyOut(out, read_, count);

    // Seqlock-style validation: if the writer lapped the region we were
    // copying from, the data may be torn. Discard it and resynchronise.
    const std::uint64_t write_after = LoadAcquire(p.write_frame);
    if (write_after - read_ > proto::kCapacityFrames) {
        read_ = write_after - target;
        backlog_ = target;
        ++resyncs_;
        return result;  // frames == 0
    }

    read_ += count;
    backlog_ = write - read_;
    result.frames = count;

    if (count < frames) {
        ++underruns_;
        phase_ = Phase::Priming;
    }
    return result;
}

void RingReader::CopyOut(float* out, std::uint64_t from_frame, std::size_t frames) const noexcept {
    if (frames == 0) {
        return;
    }
    const float* samples = proto::SamplesOf(base_);
    constexpr std::uint64_t kMask = proto::kCapacityFrames - 1;
    const std::size_t pos = static_cast<std::size_t>(from_frame & kMask);
    const std::size_t first = std::min<std::size_t>(frames, proto::kCapacityFrames - pos);

    std::memcpy(out, samples + pos * proto::kChannels, first * proto::kChannels * sizeof(float));
    if (first < frames) {
        std::memcpy(out + first * proto::kChannels, samples,
                    (frames - first) * proto::kChannels * sizeof(float));
    }
}

}  // namespace somike::dsp
