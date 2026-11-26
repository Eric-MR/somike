#pragma once

#include <windows.h>

#include <cstddef>
#include <span>
#include <string>

#include "somike/common/unique_handle.h"

namespace somike::win32 {

// Maps an existing file into memory. Views of the same file are coherent
// across processes, which is what the shared ring relies on.
//
// Open()/Close() perform file I/O and must not be called on a real-time thread.
class MappedFile {
public:
    enum class Access { ReadOnly, ReadWrite };

    MappedFile() = default;
    ~MappedFile() { Close(); }
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Maps the first `size` bytes. Fails if the file is smaller than `size`.
    [[nodiscard]] HRESULT Open(const std::wstring& path, std::size_t size, Access access) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return view_ != nullptr; }
    [[nodiscard]] bool writable() const noexcept { return writable_; }
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return {static_cast<std::byte*>(view_), size_};
    }

private:
    UniqueFileHandle file_;
    UniqueHandle mapping_;
    void* view_ = nullptr;
    std::size_t size_ = 0;
    bool writable_ = false;
};

}  // namespace somike::win32
