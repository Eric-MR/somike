#include "somike/common/strings.h"

#include <windows.h>

#include <limits>

namespace somike::text {

std::string ToUtf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input = static_cast<int>(text.size());
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), input, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), input, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input = static_cast<int>(text.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), input, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), input, result.data(), size);
    return result;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    if (a.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int length = static_cast<int>(a.size());
    return ::CompareStringOrdinal(a.data(), length, b.data(), length, TRUE) == CSTR_EQUAL;
}

namespace {

template <class Char>
std::basic_string_view<Char> TrimImpl(std::basic_string_view<Char> text) noexcept {
    auto is_space = [](Char c) {
        return c == Char(' ') || c == Char('\t') || c == Char('\r') || c == Char('\n');
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::wstring_view Trim(std::wstring_view text) noexcept { return TrimImpl(text); }
std::string_view Trim(std::string_view text) noexcept { return TrimImpl(text); }

}  // namespace somike::text
