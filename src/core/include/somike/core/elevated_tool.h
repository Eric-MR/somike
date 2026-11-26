#pragma once

#include <windows.h>

#include <string>

namespace somike::core {

inline constexpr wchar_t kApoCtlExecutable[] = L"somike-apoctl.exe";

struct ElevatedToolResult {
    HRESULT hr = E_FAIL;     // launch result; HRESULT_FROM_WIN32(ERROR_CANCELLED) if UAC was declined
    DWORD exit_code = 1;     // somike-apoctl exit code (valid when hr succeeded)
    std::wstring report;     // everything the tool printed
};

// Directory containing the running executable.
[[nodiscard]] std::wstring ExecutableDirectory();

// Runs somike-apoctl.exe (located next to the running executable) elevated
// via UAC, waits for it and collects its report. Blocks: call from a worker
// thread when used by a UI.
[[nodiscard]] ElevatedToolResult RunApoCtlElevated(HWND owner, const std::wstring& arguments);

}  // namespace somike::core
