#include "somike/common/log.h"

#include <windows.h>

#include <cstdio>
#include <iterator>
#include <mutex>

#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"

namespace somike::log {

namespace {

std::mutex g_file_mutex;
win32::UniqueFileHandle g_file;

const wchar_t* LevelName(Level level) noexcept {
    switch (level) {
        case Level::Debug:
            return L"DEBUG";
        case Level::Info:
            return L"INFO";
        case Level::Warning:
            return L"WARN";
        case Level::Error:
            return L"ERROR";
    }
    return L"?";
}

}  // namespace

void SetFile(const std::wstring& path) {
    std::scoped_lock lock(g_file_mutex);
    g_file.reset(::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

void Write(Level level, std::wstring_view message) noexcept {
    try {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        wchar_t stamp[32];
        std::swprintf(stamp, std::size(stamp), L"%04u-%02u-%02u %02u:%02u:%02u.%03u", now.wYear,
                      now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                      now.wMilliseconds);

        std::wstring line;
        line.reserve(message.size() + 48);
        line.append(stamp).append(L" [").append(LevelName(level)).append(L"] ");
        line.append(message).append(L"\r\n");

        ::OutputDebugStringW((L"[Somike] " + line).c_str());

        std::scoped_lock lock(g_file_mutex);
        if (g_file) {
            const std::string utf8 = text::ToUtf8(line);
            DWORD written = 0;
            ::WriteFile(g_file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                        nullptr);
        }
    } catch (...) {
        // Logging must never take the process down.
    }
}

}  // namespace somike::log
