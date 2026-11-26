#pragma once

#include <string>
#include <string_view>

namespace somike::text {

[[nodiscard]] std::string ToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring FromUtf8(std::string_view text);

// Ordinal, case-insensitive comparison (suitable for GUIDs, file names, ids).
[[nodiscard]] bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;

[[nodiscard]] std::wstring_view Trim(std::wstring_view text) noexcept;
[[nodiscard]] std::string_view Trim(std::string_view text) noexcept;

}  // namespace somike::text
