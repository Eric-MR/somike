// Atomic access to plain fields that live in cross-process shared memory.
//
// The shared layout uses plain integers (so it is standard-layout and has a
// fixed ABI); std::atomic_ref gives us well-defined atomic access to them.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace somike::dsp::detail {

template <class T>
[[nodiscard]] inline T LoadAcquire(const T& field) noexcept {
    return std::atomic_ref<T>(const_cast<T&>(field)).load(std::memory_order_acquire);
}

template <class T>
[[nodiscard]] inline T LoadRelaxed(const T& field) noexcept {
    return std::atomic_ref<T>(const_cast<T&>(field)).load(std::memory_order_relaxed);
}

template <class T>
inline void StoreRelease(T& field, T value) noexcept {
    std::atomic_ref<T>(field).store(value, std::memory_order_release);
}

template <class T>
inline void StoreRelaxed(T& field, T value) noexcept {
    std::atomic_ref<T>(field).store(value, std::memory_order_relaxed);
}

template <class T>
inline void AddRelaxed(T& field, T delta) noexcept {
    std::atomic_ref<T>(field).fetch_add(delta, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint32_t FloatToBits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] inline float BitsToFloat(std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

}  // namespace somike::dsp::detail
