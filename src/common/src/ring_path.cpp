#include "somike/common/ring_path.h"

#include <windows.h>

#include "somike/protocol/shared_ring.h"

namespace somike::paths {

namespace {

std::wstring EnvironmentVariable(const wchar_t* name) {
    const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring ProgramDataRoot() {
    std::wstring root = EnvironmentVariable(L"ProgramData");
    if (root.empty()) {
        root = EnvironmentVariable(L"SystemDrive");
        root = (root.empty() ? std::wstring(L"C:") : root) + L"\\ProgramData";
    }
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();
    }
    return root;
}

}  // namespace

std::wstring DataDirectory() {
    return ProgramDataRoot() + L"\\" + protocol::kDataDirectoryName;
}

std::wstring RingFile() {
    return ProgramDataRoot() + L"\\" + protocol::kRingFileRelativePath;
}

std::wstring ApoLogFile() {
    return ProgramDataRoot() + L"\\" + protocol::kApoLogFileRelativePath;
}

}  // namespace somike::paths
