// somike.exe - entry point of the desktop application.
#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>

#include "app_controller.h"
#include "main_window.h"
#include "somike/common/log.h"
#include "somike/common/unique_handle.h"
#include "somike/core/com_init.h"
#include "somike/core/settings.h"
#include "somike/version.h"

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

    const somike::core::ComInit com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!com.usable()) {
        return 1;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&controls);
    InitializeLogging();

    somike::app::AppController controller(somike::core::DefaultSettingsPath());
    somike::app::MainWindow window(controller);
    if (!window.Create(instance, show)) {
        return 1;
    }

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(window.hwnd(), &message)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
