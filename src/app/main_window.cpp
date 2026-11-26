#include "main_window.h"

#include <commctrl.h>
#include <windowsx.h>

#include <format>
#include <memory>
#include <string>
#include <thread>

#include "resource.h"
#include "somike/common/hresult.h"
#include "somike/core/com_init.h"
#include "somike/core/elevated_tool.h"
#include "somike/version.h"

namespace somike::app {

namespace {

constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kStatusIntervalMs = 250;
constexpr int kInjectorRefreshTicks = 8;  // registry re-read every ~2 s
constexpr int kHotkeyId = 1;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kHotkeyKey = 'M';
constexpr UINT WM_APP_INSTALLER_DONE = WM_APP + 1;

// Client area layout at 96 DPI (scaled at runtime).
constexpr int kClientWidth = 460;
constexpr int kClientHeight = 334;

struct ControlLayout {
    int id;
    int x, y, width, height;
};

constexpr ControlLayout kLayout[] = {
    {IDC_SOURCE_LABEL, 12, 12, 436, 16},
    {IDC_SOURCE_COMBO, 12, 30, 340, 300},
    {IDC_REFRESH, 360, 29, 88, 25},
    {IDC_MIC_LABEL, 12, 66, 436, 16},
    {IDC_MIC_COMBO, 12, 84, 340, 300},
    {IDC_INSTALL, 360, 83, 88, 25},
    {IDC_INJECTOR_STATUS, 12, 114, 340, 50},
    {IDC_UNINSTALL, 360, 113, 88, 25},
    {IDC_GAIN_LABEL, 12, 172, 200, 16},
    {IDC_GAIN_SLIDER, 6, 190, 352, 30},
    {IDC_GAIN_VALUE, 360, 196, 88, 16},
    {IDC_MUTE_MIC, 12, 226, 436, 20},
    {IDC_TOGGLE, 12, 254, 436, 44},
    {IDC_HINT, 12, 306, 436, 16},
};

std::wstring DescribeStartError(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return L"The microphone injector is not installed yet.\n\n"
               L"Click \"Install...\" next to the microphone first.";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return L"The selected application is not running.\n\n"
               L"Start it (and play something), then click \"Refresh\".";
    }
    return L"Routing could not be started:\n" + win32::FormatHResult(hr);
}

}  // namespace

MainWindow::~MainWindow() {
    if (font_ != nullptr) ::DeleteObject(font_);
    if (bold_font_ != nullptr) ::DeleteObject(bold_font_);
}

bool MainWindow::Create(HINSTANCE instance, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainWindowClass;
    if (!::RegisterClassExW(&wc)) {
        return false;
    }

    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    hwnd_ = ::CreateWindowExW(0, kMainWindowClass, L"Somike " SOMIKE_VERSION_WSTRING, kStyle,
                              CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    ::ShowWindow(hwnd_, show);
    ::UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                           : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

int MainWindow::Scale(int value) const noexcept {
    return ::MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            dpi_ = ::GetDpiForWindow(hwnd_);
            CreateControls();
            ApplyFonts();
            LayoutControls();
            PopulateSources();
            PopulateMicrophones();
            UpdateGainLabel();
            hotkey_registered_ = ::RegisterHotKey(hwnd_, kHotkeyId, kHotkeyModifiers, kHotkeyKey);
            ::SetWindowTextW(Control(IDC_HINT),
                             hotkey_registered_
                                 ? L"Global hotkey: Ctrl+Alt+M toggles routing."
                                 : L"Ctrl+Alt+M is taken by another app; use the button instead.");
            ::SetTimer(hwnd_, kStatusTimer, kStatusIntervalMs, nullptr);
            UpdateStatus(true);
            return 0;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lparam) == Control(IDC_GAIN_SLIDER)) {
                const auto position = ::SendMessageW(Control(IDC_GAIN_SLIDER), TBM_GETPOS, 0, 0);
                controller_.SetInjectGain(static_cast<float>(position) / 100.0f);
                UpdateGainLabel();
            }
            return 0;
        case WM_HOTKEY:
            if (wparam == kHotkeyId) {
                ToggleRouting();
            }
            return 0;
        case WM_TIMER:
            if (wparam == kStatusTimer) {
                UpdateStatus(++tick_ % kInjectorRefreshTicks == 0);
            }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyFonts();
            LayoutControls();
            return 0;
        }
        case WM_APP_INSTALLER_DONE:
            OnInstallerFinished(lparam);
            return 0;
        case WM_DESTROY:
            ::KillTimer(hwnd_, kStatusTimer);
            if (hotkey_registered_) {
                ::UnregisterHotKey(hwnd_, kHotkeyId);
            }
            controller_.StopRouting();
            controller_.SaveSettings();
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

void MainWindow::CreateControls() {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };

    make(L"STATIC", L"Audio source (what other people should hear):", SS_LEFT, IDC_SOURCE_LABEL);
    make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_SOURCE_COMBO);
    make(WC_BUTTONW, L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, IDC_REFRESH);
    make(L"STATIC", L"Microphone (receives the audio):", SS_LEFT, IDC_MIC_LABEL);
    make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_MIC_COMBO);
    make(WC_BUTTONW, L"Install...", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);
    make(L"STATIC", L"", SS_LEFT, IDC_INJECTOR_STATUS);
    make(WC_BUTTONW, L"Uninstall...", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);
    make(L"STATIC", L"Routed audio volume:", SS_LEFT, IDC_GAIN_LABEL);
    make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_GAIN_SLIDER);
    make(L"STATIC", L"", SS_LEFT, IDC_GAIN_VALUE);
    make(WC_BUTTONW, L"Mute my own voice while routing", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_MUTE_MIC);
    make(WC_BUTTONW, L"Start routing", WS_TABSTOP | BS_PUSHBUTTON, IDC_TOGGLE);
    make(L"STATIC", L"", SS_LEFT, IDC_HINT);
    make(STATUSCLASSNAMEW, L"", 0, IDC_STATUSBAR);

    const HWND slider = Control(IDC_GAIN_SLIDER);
    ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    ::SendMessageW(slider, TBM_SETTICFREQ, 25, 0);
    ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
    ::SendMessageW(slider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(controller_.inject_gain() * 100.0f + 0.5f));
    Button_SetCheck(Control(IDC_MUTE_MIC), controller_.mute_microphone() ? BST_CHECKED : BST_UNCHECKED);
}

void MainWindow::ApplyFonts() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);

    if (font_ != nullptr) ::DeleteObject(font_);
    if (bold_font_ != nullptr) ::DeleteObject(bold_font_);
    font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW bold = metrics.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold.lfHeight = bold.lfHeight * 5 / 4;
    bold_font_ = ::CreateFontIndirectW(&bold);

    for (const ControlLayout& item : kLayout) {
        ::SendMessageW(Control(item.id), WM_SETFONT,
                       reinterpret_cast<WPARAM>(item.id == IDC_TOGGLE ? bold_font_ : font_), TRUE);
    }
    ::SendMessageW(Control(IDC_STATUSBAR), WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void MainWindow::LayoutControls() {
    for (const ControlLayout& item : kLayout) {
        ::MoveWindow(Control(item.id), Scale(item.x), Scale(item.y), Scale(item.width),
                     Scale(item.height), TRUE);
    }
    // Status bar positions itself; size the window around content + status bar.
    const HWND status = Control(IDC_STATUSBAR);
    ::SendMessageW(status, WM_SIZE, 0, 0);
    RECT status_rect{};
    ::GetWindowRect(status, &status_rect);

    RECT window{0, 0, Scale(kClientWidth), Scale(kClientHeight) + (status_rect.bottom - status_rect.top)};
    ::AdjustWindowRectExForDpi(&window, static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)),
                               FALSE, 0, dpi_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, window.right - window.left, window.bottom - window.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ::SendMessageW(status, WM_SIZE, 0, 0);
}

void MainWindow::PopulateSources() {
    const HWND combo = Control(IDC_SOURCE_COMBO);
    ComboBox_ResetContent(combo);
    for (const SourceItem& item : controller_.sources()) {
        ComboBox_AddString(combo, item.label.c_str());
    }
    ComboBox_SetCurSel(combo, controller_.selected_source());
}

void MainWindow::PopulateMicrophones() {
    const HWND combo = Control(IDC_MIC_COMBO);
    ComboBox_ResetContent(combo);
    for (const core::AudioEndpoint& mic : controller_.microphones()) {
        std::wstring label = mic.name;
        if (mic.is_default) label += L"  (default)";
        ComboBox_AddString(combo, label.c_str());
    }
    ComboBox_SetCurSel(combo, controller_.selected_microphone());
}

void MainWindow::UpdateGainLabel() {
    const std::wstring text = std::format(L"{:.0f} %", controller_.inject_gain() * 100.0f);
    ::SetWindowTextW(Control(IDC_GAIN_VALUE), text.c_str());
}

void MainWindow::UpdateStatus(bool refresh_injector) {
    // --- router -----------------------------------------------------------------
    const core::RouterStatus router = controller_.router_status();
    ::SetWindowTextW(Control(IDC_TOGGLE),
                     router.state == core::RouterState::Routing ? L"Stop routing" : L"Start routing");

    std::wstring status;
    switch (router.state) {
        case core::RouterState::Routing:
            status = std::format(L"Routing {} into \"{}\"  -  {:.1f} s of audio sent",
                                 controller_.routing_label(), controller_.selected_microphone_name(),
                                 static_cast<double>(router.frames_written) / 48000.0);
            break;
        case core::RouterState::Failed:
            status = L"Routing stopped: " + win32::FormatHResult(router.error);
            break;
        case core::RouterState::Stopped:
            status = L"Ready. Routing is off.";
            break;
    }
    if (installer_running_) {
        status = L"Waiting for the injector installer (administrator approval)...";
    }
    ::SendMessageW(Control(IDC_STATUSBAR), SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(status.c_str()));

    if (router.state == core::RouterState::Failed && last_router_state_ == core::RouterState::Routing) {
        ::MessageBeep(MB_ICONWARNING);
    }
    last_router_state_ = router.state;

    // --- injector (APO) ------------------------------------------------------------
    if (!refresh_injector) {
        return;
    }
    const core::InjectorStatus injector = controller_.QueryInjectorStatus();
    const bool installed = injector.install_state != installer::InstallState::NotInstalled;

    std::wstring text;
    if (!injector.endpoint_valid) {
        text = L"No microphone selected.";
    } else if (!installed) {
        text = L"The injector is not installed on this microphone. Click \"Install...\" "
               L"(administrator rights required once).";
    } else if (injector.effects_disabled) {
        text = L"Installed, but \"Audio enhancements\" are off for this microphone. "
               L"Click \"Repair...\" to enable them.";
    } else if (injector.apo_alive) {
        text = std::format(L"Injector active ({} Hz, {} ch). Apps using this microphone "
                           L"receive the routed audio.",
                           injector.consumer.sample_rate, injector.consumer.channels);
    } else {
        text = L"Injector installed. It activates as soon as an app uses this microphone "
               L"(restart the audio service or reboot after installing).";
    }
    ::SetWindowTextW(Control(IDC_INJECTOR_STATUS), text.c_str());
    ::SetWindowTextW(Control(IDC_INSTALL), installed ? L"Repair..." : L"Install...");
    ::EnableWindow(Control(IDC_INSTALL), !installer_running_ && injector.endpoint_valid);
    ::EnableWindow(Control(IDC_UNINSTALL), !installer_running_ && installed);
}

void MainWindow::OnCommand(int id, int code) {
    switch (id) {
        case IDC_SOURCE_COMBO:
            if (code == CBN_SELCHANGE) {
                controller_.SelectSource(ComboBox_GetCurSel(Control(IDC_SOURCE_COMBO)));
                UpdateStatus(false);
            }
            break;
        case IDC_MIC_COMBO:
            if (code == CBN_SELCHANGE) {
                controller_.SelectMicrophone(ComboBox_GetCurSel(Control(IDC_MIC_COMBO)));
                UpdateStatus(true);
            }
            break;
        case IDC_REFRESH:
            controller_.RefreshSources();
            controller_.RefreshMicrophones();
            PopulateSources();
            PopulateMicrophones();
            UpdateStatus(true);
            break;
        case IDC_MUTE_MIC:
            if (code == BN_CLICKED) {
                controller_.SetMuteMicrophone(Button_GetCheck(Control(IDC_MUTE_MIC)) == BST_CHECKED);
            }
            break;
        case IDC_TOGGLE:
            ToggleRouting();
            break;
        case IDC_INSTALL:
            RunInstaller(false, L"auto");
            break;
        case IDC_UNINSTALL:
            RunInstaller(true, nullptr);
            break;
        default:
            break;
    }
}

void MainWindow::ToggleRouting() {
    if (controller_.routing()) {
        controller_.StopRouting();
    } else {
        // Re-resolve the selected app's process id (it may have restarted).
        controller_.RefreshSources();
        PopulateSources();
        const HRESULT hr = controller_.StartRouting();
        if (FAILED(hr)) {
            ::MessageBoxW(hwnd_, DescribeStartError(hr).c_str(), L"Somike", MB_OK | MB_ICONWARNING);
        }
    }
    UpdateStatus(false);
}

void MainWindow::RunInstaller(bool uninstall, const wchar_t* strategy) {
    if (installer_running_) {
        return;
    }
    const std::wstring mic_id = controller_.selected_microphone_id();
    if (mic_id.empty()) {
        return;
    }
    const std::wstring question =
        uninstall
            ? L"Remove the Somike injector from \"" + controller_.selected_microphone_name() +
                  L"\" and restore its previous effect configuration?\n\n"
                  L"The Windows Audio service will be restarted (all audio pauses briefly)."
            : L"Install the Somike injector on \"" + controller_.selected_microphone_name() +
                  L"\"?\n\nThis registers an audio effect for this microphone (no virtual device "
                  L"is created) and restarts the Windows Audio service (all audio pauses "
                  L"briefly). Administrator rights are required.";
    if (::MessageBoxW(hwnd_, question.c_str(), L"Somike", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    if (controller_.routing()) {
        controller_.StopRouting();  // the audio service restart would break it anyway
    }

    std::wstring arguments = (uninstall ? L"uninstall" : L"install");
    arguments += L" --endpoint \"" + mic_id + L"\" --restart-audio";
    if (!uninstall && strategy != nullptr) {
        arguments += std::wstring(L" --strategy ") + strategy;
    }

    installer_running_ = true;
    last_uninstall_ = uninstall;
    UpdateStatus(true);

    const HWND hwnd = hwnd_;
    std::thread([hwnd, arguments] {
        std::unique_ptr<core::ElevatedToolResult> result;
        {
            // ShellExecuteEx wants an STA on the calling thread.
            const core::ComInit com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            result = std::make_unique<core::ElevatedToolResult>(core::RunApoCtlElevated(hwnd, arguments));
        }
        if (::PostMessageW(hwnd, WM_APP_INSTALLER_DONE, 0, reinterpret_cast<LPARAM>(result.get()))) {
            static_cast<void>(result.release());  // ownership passes to the window
        }
    }).detach();
}

void MainWindow::OnInstallerFinished(LPARAM lparam) {
    const std::unique_ptr<core::ElevatedToolResult> result(
        reinterpret_cast<core::ElevatedToolResult*>(lparam));
    installer_running_ = false;

    constexpr int kExitRefused = 3;
    if (FAILED(result->hr)) {
        if (result->hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            const std::wstring message = L"Could not start " + std::wstring(core::kApoCtlExecutable) +
                                         L":\n" + win32::FormatHResult(result->hr);
            ::MessageBoxW(hwnd_, message.c_str(), L"Somike", MB_OK | MB_ICONERROR);
        }
    } else if (!last_uninstall_ && result->exit_code == kExitRefused) {
        const std::wstring message =
            result->report +
            L"\nInstall anyway, keeping the existing effect and chaining Somike after it "
            L"(experimental \"composite\" strategy)?";
        if (::MessageBoxW(hwnd_, message.c_str(), L"Somike", MB_YESNO | MB_ICONWARNING) == IDYES) {
            UpdateStatus(true);
            RunInstaller(false, L"composite");
            return;
        }
    } else {
        ::MessageBoxW(hwnd_, result->report.empty() ? L"Done." : result->report.c_str(), L"Somike",
                      MB_OK | (result->exit_code == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    UpdateStatus(true);
}

}  // namespace somike::app
