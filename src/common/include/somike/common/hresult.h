#pragma once

#include <windows.h>

#include <string>

namespace somike::win32 {

// HRESULT_FROM_WIN32(GetLastError()), or E_FAIL if no error code is set.
[[nodiscard]] HRESULT LastErrorAsHResult() noexcept;

// "0x80070005 (Access is denied.)" - includes names for common audio codes
// that the system message table does not know about.
[[nodiscard]] std::wstring FormatHResult(HRESULT hr);

}  // namespace somike::win32
