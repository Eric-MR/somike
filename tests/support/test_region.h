// A zeroed, page-aligned in-memory stand-in for the memory-mapped ring file.
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <span>

#include "somike/protocol/shared_ring.h"

namespace somike::test {

class TestRegion {
public:
    static constexpr std::size_t kAlignment = 4096;
    // Extra space so tests can also build deliberately misaligned views.
    static constexpr std::size_t kAllocation = protocol::kRegionBytes + kAlignment;

    TestRegion()
        : data_(static_cast<std::byte*>(::operator new(kAllocation, std::align_val_t{kAlignment}))) {
        std::memset(data_, 0, kAllocation);
    }
    ~TestRegion() { ::operator delete(data_, std::align_val_t{kAlignment}); }
    TestRegion(const TestRegion&) = delete;
    TestRegion& operator=(const TestRegion&) = delete;

    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] std::span<std::byte> span() noexcept { return {data_, protocol::kRegionBytes}; }
    [[nodiscard]] std::span<std::byte> raw() noexcept { return {data_, kAllocation}; }

private:
    std::byte* data_;
};

}  // namespace somike::test
