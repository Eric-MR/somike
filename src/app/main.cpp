// somike.exe - entry point of the desktop application.
//
// Command line (all optional):
//   --source <app.exe|system>     select what to play into the mic
//   --live                        go live right away (e.g. from a shortcut)
//   --ui-dir <folder>             development: load the UI from disk
//   --remote-debugging-port <n>   development/testing: DevTools protocol port
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cwchar>
#include <string>

#include "app_controller.h"
#include "somike/common/log.h"
#include "somike/common/unique_handle.h"
#include "somike/core/com_init.h"
#include "somike/core/settings.h"
#include "somike/version.h"
#include "web_window.h"

namespace {

constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Somike.SingleInstance";
constexpr LONGLONG kMaxLogBytes = 1024 * 1024;

// %LOCALAPPDATA%\Somike\somike.log, truncated when it grows beyond 1 MB.
void InitializeLogging() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) {
        return;
    }
    const std::wstring directory = std::wstring(folder) + L"\\Somike";
    ::CoTaskMemFree(folder);
    ::CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring path = directory + L"\\somike.log";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const LONGLONG size = (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > kMaxLogBytes) {
            ::DeleteFileW(path.c_str());
        }
    }
    somike::log::SetFile(path);
    somike::log::Info(L"Somike {} starting", SOMIKE_VERSION_WSTRING);
}

struct CommandLine {
    std::wstring source;
    bool live = false;
    somike::app::WindowOptions window;
};

CommandLine ParseCommandLine() {
    CommandLine result;
    int count = 0;
    LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (args == nullptr) {
        return result;
    }
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = args[i];
        const bool has_value = i + 1 < count;
        if (arg == L"--source" && has_value) {
            result.source = args[++i];
        } else if (arg == L"--live") {
            result.live = true;
        } else if (arg == L"--ui-dir" && has_value) {
            result.window.ui_directory = args[++i];
        } else if (arg == L"--remote-debugging-port" && has_value) {
            result.window.remote_debugging_port = static_cast<int>(std::wcstol(args[++i], nullptr, 10));
        }
    }
    ::LocalFree(args);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*command_line*/, int show) {
    // One instance only: a second launch brings the first window forward.
    somike::win32::UniqueHandle mutex(::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const HWND existing = ::FindWindowW(somike::app::kMainWindowClass, nullptr)) {
            ::ShowWindow(existing, SW_RESTORE);
            ::SetForegroundWindow(existing);
        }
        return 0;
    }

    // WebView2 requires a single-threaded apartment on the UI thread.
    const somike::core::ComInit com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!com.usable()) {
        return 1;
    }
    InitializeLogging();

    const CommandLine command_line = ParseCommandLine();
    somike::app::AppController controller(somike::core::DefaultSettingsPath());
    if (!command_line.source.empty()) {
        controller.SelectSource(command_line.source);
    }
    if (command_line.live) {
        controller.SetLive(true);
    }

    somike::app::WebWindow window(controller, command_line.window);
    if (!window.Create(instance, show)) {
        return 1;
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
