#include "somike/dsp/ring_writer.h"

#include <algorithm>
#include <cstring>

#include "somike/dsp/detail/atomic_field.h"
#include "somike/protocol/shared_ring.h"

namespace somike::dsp {

namespace proto = somike::protocol;
using namespace detail;

RingWriter::RingWriter(std::span<std::byte> region) noexcept {
    if (region.size() < proto::kRegionBytes ||
        reinterpret_cast<std::uintptr_t>(region.data()) % 64 != 0) {
        return;
    }
    base_ = region.data();

    proto::RegionHeader* header = proto::HeaderOf(base_);
    if (!proto::HeaderIsCompatible(*header)) {
        // Publish the magic last so a concurrent reader never sees a
        // half-initialised header as valid.
        StoreRelaxed(header->magic, 0u);
        proto::RegionHeader fresh{};
        proto::InitializeHeader(fresh);
        fresh.magic = 0;
        fresh.producer.inject_gain_bits = FloatToBits(1.0f);
        fresh.producer.mic_gain_bits = FloatToBits(1.0f);
        std::memcpy(header, &fresh, sizeof(fresh));
        StoreRelease(header->magic, proto::kMagic);
    }
}

void RingWriter::BeginSession(std::uint64_t now_ms) noexcept {
    if (!base_) {
        return;
    }
    proto::ProducerBlock& p = proto::HeaderOf(base_)->producer;
    StoreRelease(p.session, LoadRelaxed(p.session) + 1u);
    StoreRelease(p.heartbeat_ms, now_ms);
    StoreRelease(p.state, static_cast<std::uint32_t>(proto::ProducerState::Streaming));
}

void RingWriter::EndSession(std::uint64_t now_ms) noexcept {
    if (!base_) {
        return;
    }
    proto::ProducerBlock& p = proto::HeaderOf(base_)->producer;
    StoreRelease(p.state, static_cast<std::uint32_t>(proto::ProducerState::Idle));
    StoreRelease(p.heartbeat_ms, now_ms);
}

void RingWriter::Write(const float* interleaved, std::size_t frames,
                       std::uint64_t now_ms) noexcept {
    if (!base_) {
        return;
    }
    proto::ProducerBlock& p = proto::HeaderOf(base_)->producer;
    if (interleaved == nullptr || frames == 0) {
        StoreRelease(p.heartbeat_ms, now_ms);
        return;
    }

    std::uint64_t write = LoadRelaxed(p.write_frame);  // we are the only writer

    // More than a full ring in one call: only the newest frames survive.
    if (frames > proto::kCapacityFrames) {
        const std::size_t skip = frames - proto::kCapacityFrames;
        interleaved += skip * proto::kChannels;
        write += skip;
        frames = proto::kCapacityFrames;
    }

    float* samples = proto::SamplesOf(base_);
    constexpr std::uint64_t kMask = proto::kCapacityFrames - 1;
    const std::size_t pos = static_cast<std::size_t>(write & kMask);
    const std::size_t first = std::min<std::size_t>(frames, proto::kCapacityFrames - pos);

    std::memcpy(samples + pos * proto::kChannels, interleaved,
                first * proto::kChannels * sizeof(float));
    if (first < frames) {
        std::memcpy(samples, interleaved + first * proto::kChannels,
                    (frames - first) * proto::kChannels * sizeof(float));
    }

    // Release: consumers that observe the new index also observe the samples.
    StoreRelease(p.write_frame, write + frames);
    StoreRelease(p.heartbeat_ms, now_ms);
}

void RingWriter::Heartbeat(std::uint64_t now_ms) noexcept {
    if (base_) {
        StoreRelease(proto::HeaderOf(base_)->producer.heartbeat_ms, now_ms);
    }
}

void RingWriter::SetGains(float inject_gain, float mic_gain) noexcept {
    if (!base_) {
        return;
    }
    proto::ProducerBlock& p = proto::HeaderOf(base_)->producer;
    StoreRelease(p.inject_gain_bits, FloatToBits(inject_gain));
    StoreRelease(p.mic_gain_bits, FloatToBits(mic_gain));
}

std::uint64_t RingWriter::write_frame() const noexcept {
    return base_ ? LoadAcquire(proto::HeaderOf(base_)->producer.write_frame) : 0;
}

namespace {

ConsumerSnapshot ReadConsumerBlock(const proto::ConsumerBlock& c) noexcept {
    ConsumerSnapshot snapshot;
    snapshot.heartbeat_ms = LoadAcquire(c.heartbeat_ms);
    snapshot.frames_injected = LoadRelaxed(c.frames_injected);
    snapshot.underruns = LoadRelaxed(c.underruns);
    snapshot.resyncs = LoadRelaxed(c.resyncs);
    snapshot.sample_rate = LoadRelaxed(c.sample_rate);
    snapshot.channels = LoadRelaxed(c.channels);
    return snapshot;
}

}  // namespace

ConsumerSnapshot RingWriter::ReadConsumer() const noexcept {
    if (!base_) {
        return {};
    }
    return ReadConsumerBlock(proto::HeaderOf(base_)->consumer);
}

RingSnapshot InspectRing(std::span<const std::byte> region) noexcept {
    RingSnapshot snapshot;
    if (region.size() < proto::kRegionBytes ||
        reinterpret_cast<std::uintptr_t>(region.data()) % 64 != 0) {
        return snapshot;
    }
    const auto* header = reinterpret_cast<const proto::RegionHeader*>(region.data());
    if (LoadAcquire(header->magic) != proto::kMagic || !proto::HeaderIsCompatible(*header)) {
        return snapshot;
    }
    snapshot.compatible = true;
    const proto::ProducerBlock& p = header->producer;
    snapshot.producer_streaming =
        LoadAcquire(p.state) == static_cast<std::uint32_t>(proto::ProducerState::Streaming);
    snapshot.producer_heartbeat_ms = LoadAcquire(p.heartbeat_ms);
    snapshot.write_frame = LoadAcquire(p.write_frame);
    snapshot.session = LoadAcquire(p.session);
    snapshot.consumer = ReadConsumerBlock(header->consumer);
    return snapshot;
}

}  // namespace somike::dsp
