#include "somike/common/guid.h"

#include <windows.h>
#include <combaseapi.h>

#include <iterator>

namespace somike::guid {

std::wstring ToString(const GUID& value) {
    wchar_t buffer[40] = {};
    const int length = ::StringFromGUID2(value, buffer, static_cast<int>(std::size(buffer)));
    return length > 0 ? std::wstring(buffer, static_cast<std::size_t>(length - 1)) : std::wstring{};
}

std::optional<GUID> Parse(std::wstring_view text) {
    // "{8-4-4-4-12}" is exactly 38 characters.
    if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') {
        return std::nullopt;
    }
    const std::wstring terminated(text);
    GUID value{};
    if (FAILED(::IIDFromString(terminated.c_str(), &value))) {
        return std::nullopt;
    }
    return value;
}

}  // namespace somike::guid
