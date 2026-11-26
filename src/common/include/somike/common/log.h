#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace somike::log {

enum class Level { Debug, Info, Warning, Error };

// Every message goes to OutputDebugString (visible in DebugView / a debugger).
// Optionally also appended to a UTF-8 file: "2026-11-26 14:03:12.345 [INFO] text".
void SetFile(const std::wstring& path);

// Never throws. Not real-time safe (allocates, may block on file I/O).
void Write(Level level, std::wstring_view message) noexcept;

template <class... Args>
void Format(Level level, std::wformat_string<Args...> format, Args&&... args) noexcept {
    try {
        Write(level, std::format(format, std::forward<Args>(args)...));
    } catch (...) {
        Write(level, L"<log formatting failed>");
    }
}

template <class... Args>
void Info(std::wformat_string<Args...> format, Args&&... args) noexcept {
    Format(Level::Info, format, std::forward<Args>(args)...);
}

template <class... Args>
void Warn(std::wformat_string<Args...> format, Args&&... args) noexcept {
    Format(Level::Warning, format, std::forward<Args>(args)...);
}

template <class... Args>
void Error(std::wformat_string<Args...> format, Args&&... args) noexcept {
    Format(Level::Error, format, std::forward<Args>(args)...);
}

}  // namespace somike::log
