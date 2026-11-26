// The main window: a deliberately small Win32 view over AppController.
#pragma once

#include <windows.h>

#include "app_controller.h"
#include "somike/core/router.h"

namespace somike::app {

inline constexpr wchar_t kMainWindowClass[] = L"SomikeMainWindow";

class MainWindow {
public:
    explicit MainWindow(AppController& controller) noexcept : controller_(controller) {}
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, int show);
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void CreateControls();
    void ApplyFonts();
    void LayoutControls();
    [[nodiscard]] int Scale(int value) const noexcept;
    [[nodiscard]] HWND Control(int id) const noexcept { return ::GetDlgItem(hwnd_, id); }

    void PopulateSources();
    void PopulateMicrophones();
    void UpdateGainLabel();
    void UpdateStatus(bool refresh_injector);

    void OnCommand(int id, int code);
    void ToggleRouting();
    void RunInstaller(bool uninstall, const wchar_t* strategy);
    void OnInstallerFinished(LPARAM result);

    AppController& controller_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT bold_font_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool hotkey_registered_ = false;
    bool installer_running_ = false;
    bool last_uninstall_ = false;
    int tick_ = 0;
    core::RouterState last_router_state_ = core::RouterState::Stopped;
};

}  // namespace somike::app
