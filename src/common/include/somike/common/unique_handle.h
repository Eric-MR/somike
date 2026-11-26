#pragma once

#include <windows.h>

namespace somike::win32 {

struct NullHandleTraits {
    static HANDLE invalid() noexcept { return nullptr; }
};

struct FileHandleTraits {
    static HANDLE invalid() noexcept { return INVALID_HANDLE_VALUE; }
};

// Move-only owner of a kernel HANDLE closed with CloseHandle().
template <class Traits>
class BasicUniqueHandle {
public:
    BasicUniqueHandle() noexcept = default;
    explicit BasicUniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~BasicUniqueHandle() { reset(); }

    BasicUniqueHandle(BasicUniqueHandle&& other) noexcept : handle_(other.release()) {}
    BasicUniqueHandle& operator=(BasicUniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    BasicUniqueHandle(const BasicUniqueHandle&) = delete;
    BasicUniqueHandle& operator=(const BasicUniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = Traits::invalid();
        return handle;
    }

    void reset(HANDLE handle = Traits::invalid()) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = Traits::invalid();
};

using UniqueHandle = BasicUniqueHandle<NullHandleTraits>;
using UniqueFileHandle = BasicUniqueHandle<FileHandleTraits>;

}  // namespace somike::win32
