// Shared-memory protocol between the Somike app (producer) and the Somike
// capture APO running inside audiodg.exe (consumer).
//
// This header is the single source of truth for the memory layout. It is
// header-only, has no Win32 dependencies and is compiled into both sides.
// Any incompatible change MUST bump kVersion (and therefore the file name).
//
// See docs/ipc-protocol.md for the full specification.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace somike::protocol {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMagic = 0x4B4D4F53;  // "SOMK" (little-endian)
inline constexpr std::uint32_t kVersion = 1;

// Relative to %ProgramData%. The version is part of the name so that an old
// APO and a new app never interpret each other's layout.
inline constexpr wchar_t kRingFileRelativePath[] = L"Somike\\ring-v1.bin";
inline constexpr wchar_t kDataDirectoryName[] = L"Somike";

// ---------------------------------------------------------------------------
// Canonical stream format carried by the ring
// ---------------------------------------------------------------------------
// The producer always writes 48 kHz, stereo, 32-bit float, interleaved.
// The consumer adapts to whatever format the microphone endpoint runs at.
inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::uint32_t kChannels = 2;
inline constexpr std::uint32_t kCapacityFrames = 1u << 15;  // 32768 frames ~ 683 ms

static_assert((kCapacityFrames & (kCapacityFrames - 1)) == 0,
              "capacity must be a power of two (index masking)");

inline constexpr std::size_t kHeaderBytes = 4096;
inline constexpr std::size_t kDataBytes =
    std::size_t{kCapacityFrames} * kChannels * sizeof(float);
inline constexpr std::size_t kRegionBytes = kHeaderBytes + kDataBytes;

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
enum class ProducerState : std::uint32_t {
    Idle = 0,       // not routing; consumers pass the microphone through untouched
    Streaming = 1,  // routing; consumers mix ring audio into the microphone
};

// The blocks are cache-line aligned on purpose (producer and consumers write
// different lines); the resulting padding is intended.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// Written only by the producer (the app). Read by every consumer instance.
struct alignas(64) ProducerBlock {
    std::uint64_t write_frame;       // monotonic count of frames ever written
    std::uint64_t heartbeat_ms;      // GetTickCount64() of the last producer sign of life
    std::uint32_t state;             // ProducerState
    std::uint32_t session;           // bumped on every (re)start; consumers re-prime on change
    std::uint32_t inject_gain_bits;  // float bit pattern, linear gain for ring audio
    std::uint32_t mic_gain_bits;     // float bit pattern, linear gain for the real microphone
};

// Written by consumer instances (diagnostics only, never trusted for control).
// Multiple APO instances may write concurrently; fields are updated atomically.
struct alignas(64) ConsumerBlock {
    std::uint64_t heartbeat_ms;     // GetTickCount64() of the last processed block
    std::uint64_t frames_injected;  // total frames mixed into microphone streams
    std::uint64_t underruns;        // ring ran dry while streaming
    std::uint64_t resyncs;          // reader jumped because it fell too far behind
    std::uint32_t sample_rate;      // endpoint format of the last active instance
    std::uint32_t channels;
};

struct RegionHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t sample_rate;
    std::uint32_t channels;
    std::uint32_t capacity_frames;
    std::uint32_t reserved[10];
    ProducerBlock producer;
    ConsumerBlock consumer;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(std::is_standard_layout_v<RegionHeader>);
static_assert(std::is_trivially_copyable_v<RegionHeader>);
static_assert(sizeof(RegionHeader) <= kHeaderBytes);
static_assert(offsetof(RegionHeader, producer) % 64 == 0);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free to be usable across processes");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool HeaderIsCompatible(const RegionHeader& header) noexcept {
    // Plain reads are fine here: these fields are written once at file creation.
    return header.magic == kMagic && header.version == kVersion &&
           header.header_bytes == kHeaderBytes && header.sample_rate == kSampleRate &&
           header.channels == kChannels && header.capacity_frames == kCapacityFrames;
}

inline void InitializeHeader(RegionHeader& header) noexcept {
    header = RegionHeader{};
    header.magic = kMagic;
    header.version = kVersion;
    header.header_bytes = kHeaderBytes;
    header.sample_rate = kSampleRate;
    header.channels = kChannels;
    header.capacity_frames = kCapacityFrames;
}

[[nodiscard]] inline RegionHeader* HeaderOf(std::byte* region) noexcept {
    return reinterpret_cast<RegionHeader*>(region);
}

[[nodiscard]] inline float* SamplesOf(std::byte* region) noexcept {
    return reinterpret_cast<float*>(region + kHeaderBytes);
}

}  // namespace somike::protocol
