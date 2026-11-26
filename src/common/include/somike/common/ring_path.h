#pragma once

#include <string>

namespace somike::paths {

// %ProgramData%\Somike
//
// Resolved from the environment instead of SHGetKnownFolderPath so the APO
// does not pull shell32.dll into audiodg.exe.
[[nodiscard]] std::wstring DataDirectory();

// %ProgramData%\Somike\ring-v<version>.bin
[[nodiscard]] std::wstring RingFile();

}  // namespace somike::paths
