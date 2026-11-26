#include "somike/common/mapped_file.h"

#include <utility>

#include "somike/common/hresult.h"

namespace somike::win32 {

MappedFile::MappedFile(MappedFile&& other) noexcept
    : file_(std::move(other.file_)),
      mapping_(std::move(other.mapping_)),
      view_(std::exchange(other.view_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      writable_(std::exchange(other.writable_, false)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Close();
        file_ = std::move(other.file_);
        mapping_ = std::move(other.mapping_);
        view_ = std::exchange(other.view_, nullptr);
        size_ = std::exchange(other.size_, 0);
        writable_ = std::exchange(other.writable_, false);
    }
    return *this;
}

HRESULT MappedFile::Open(const std::wstring& path, std::size_t size, Access access) noexcept {
    Close();
    if (size == 0) {
        return E_INVALIDARG;
    }
    const bool write = access == Access::ReadWrite;

    UniqueFileHandle file(::CreateFileW(
        path.c_str(), write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return LastErrorAsHResult();
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(file.get(), &file_size)) {
        return LastErrorAsHResult();
    }
    if (file_size.QuadPart < 0 || static_cast<unsigned long long>(file_size.QuadPart) < size) {
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    }

    UniqueHandle mapping(::CreateFileMappingW(file.get(), nullptr,
                                              write ? PAGE_READWRITE : PAGE_READONLY, 0, 0,
                                              nullptr));
    if (!mapping) {
        return LastErrorAsHResult();
    }

    void* view = ::MapViewOfFile(mapping.get(), write ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ,
                                 0, 0, size);
    if (view == nullptr) {
        return LastErrorAsHResult();
    }

    file_ = std::move(file);
    mapping_ = std::move(mapping);
    view_ = view;
    size_ = size;
    writable_ = write;
    return S_OK;
}

void MappedFile::Close() noexcept {
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    mapping_.reset();
    file_.reset();
    size_ = 0;
    writable_ = false;
}

}  // namespace somike::win32
