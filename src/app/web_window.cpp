#include "web_window.h"

#include <dwmapi.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>

#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <string_view>
#include <thread>

#include "app_state.h"
#include "resource.h"
#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/strings.h"
#include "somike/core/com_init.h"

namespace somike::app {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 250;
constexpr int kHotkeyId = 1;
constexpr UINT WM_APP_INSTALLER_DONE = WM_APP + 1;
constexpr int kExitRefused = 3;  // somike-apoctl: would displace a driver effect

constexpr int kDefaultWidth = 480;  // DIPs
constexpr int kDefaultHeight = 800;
constexpr int kMinWidth = 400;
constexpr int kMinHeight = 600;
constexpr COLORREF kBackground = RGB(11, 11, 14);  // --bg in app.css

constexpr wchar_t kUiHost[] = L"app.somike";
constexpr std::wstring_view kUiOrigin = L"https://app.somike/";
constexpr wchar_t kUiStart[] = L"https://app.somike/index.html";
// Only our own script; inline styles for computed colours; icons as data URLs.
constexpr wchar_t kContentSecurityPolicy[] =
    L"default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    L"img-src 'self' data:; base-uri 'none'; form-action 'none'";

struct UiFile {
    std::wstring_view path;
    int resource;
    const wchar_t* content_type;
};
constexpr UiFile kUiFiles[] = {
    {L"/index.html", IDR_UI_INDEX_HTML, L"text/html; charset=utf-8"},
    {L"/app.css", IDR_UI_APP_CSS, L"text/css; charset=utf-8"},
    {L"/app.js", IDR_UI_APP_JS, L"text/javascript; charset=utf-8"},
    {L"/soundboard.css", IDR_UI_SOUNDBOARD_CSS, L"text/css; charset=utf-8"},
    {L"/soundboard.js", IDR_UI_SOUNDBOARD_JS, L"text/javascript; charset=utf-8"},
};

// Takes ownership of a CoTaskMem string.
std::wstring TakeCoString(LPWSTR value) {
    if (value == nullptr) {
        return {};
    }
    std::wstring result(value);
    ::CoTaskMemFree(value);
    return result;
}

std::wstring DataDirectory() {
    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) {
        result = std::wstring(folder) + L"\\Somike";
    }
    ::CoTaskMemFree(folder);
    return result;
}

std::wstring LastLine(const std::wstring& report) {
    std::wstring last;
    std::size_t start = 0;
    while (start < report.size()) {
        std::size_t end = report.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = report.size();
        }
        const std::wstring_view line = text::Trim(std::wstring_view(report).substr(start, end - start));
        if (!line.empty()) {
            last.assign(line);
        }
        start = end + 1;
    }
    return last;
}

// "Mikrofon (Corsair HS80 ...)" -> "Corsair HS80 ..."
std::wstring ShortDeviceName(const std::wstring& name) {
    const auto open = name.rfind(L'(');
    if (open != std::wstring::npos && !name.empty() && name.back() == L')' && open + 1 < name.size() - 1) {
        return name.substr(open + 1, name.size() - open - 2);
    }
    return name;
}

}  // namespace

WebWindow::WebWindow(AppController& controller, WindowOptions options)
    : controller_(controller),
      soundboard_bridge_(controller, [this](const std::string& json) { PostJson(json); }),
      options_(std::move(options)) {}

WebWindow::~WebWindow() {
    if (background_ != nullptr) {
        ::DeleteObject(background_);
    }
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
bool WebWindow::Create(HINSTANCE instance, int show) {
    background_ = ::CreateSolidBrush(kBackground);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = background_;  // no white flash before the UI paints
    wc.lpszClassName = kMainWindowClass;
    if (::RegisterClassExW(&wc) == 0) {
        return false;
    }

    hwnd_ = ::CreateWindowExW(0, kMainWindowClass, L"Somike", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, kDefaultWidth, kDefaultHeight, nullptr, nullptr,
                              instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }

    // Size in DIPs for this monitor, centred on its work area.
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    RECT frame{0, 0, ::MulDiv(kDefaultWidth, static_cast<int>(dpi), 96),
               ::MulDiv(kDefaultHeight, static_cast<int>(dpi), 96)};
    ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& work = monitor.rcWork;
    const int width = std::min<int>(frame.right - frame.left, work.right - work.left);
    const int height = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
    ::SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - width) / 2,
                   work.top + (work.bottom - work.top - height) / 2, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    ApplyWindowTheme();
    ::ShowWindow(hwnd_, show);
    ::UpdateWindow(hwnd_);

    hotkey_registered_ = ::RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != FALSE;
    if (!hotkey_registered_) {
        log::Warn(L"Ctrl+Alt+M is already used by another program");
    }
    soundboard_bridge_.Attach(hwnd_);
    ::SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    StartWebView();
    return true;
}

void WebWindow::ApplyWindowTheme() {
    // Dark title bar matching the UI (Windows 11; ignored where unsupported).
    const BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = kBackground;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    const COLORREF text = RGB(241, 241, 244);
    ::DwmSetWindowAttribute(hwnd_, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

LRESULT CALLBACK WebWindow::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WebWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<WebWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<WebWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                           : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT WebWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_MOVE:
            if (webview_controller_) {
                webview_controller_->NotifyParentWindowPositionChanged();
            }
            return 0;
        case WM_SETFOCUS:
            if (webview_controller_) {
                webview_controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const int dpi = static_cast<int>(::GetDpiForWindow(hwnd_));
            info->ptMinTrackSize = {::MulDiv(kMinWidth, dpi, 96), ::MulDiv(kMinHeight, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kTickTimer) {
                Tick();
            }
            return 0;
        case WM_HOTKEY:
            if (wparam == kHotkeyId) {
                controller_.ToggleLive();
                PushState(false);
                UpdateTitle();
            } else {
                soundboard_bridge_.HandleHotkey(wparam);
            }
            return 0;
        case SoundboardBridge::kWakeMessage:
            soundboard_bridge_.OnWake();
            return 0;
        case WM_APP_INSTALLER_DONE:
            OnInstallerFinished(std::unique_ptr<core::ElevatedToolResult>(
                reinterpret_cast<core::ElevatedToolResult*>(lparam)));
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(hwnd_);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;  // nothing unsaved to ask about
        case WM_ENDSESSION:
            // Log-off, shutdown, or the installer closing Somike through the
            // Restart Manager: Windows may end the process right after this
            // message, so save now and close the window properly.
            if (wparam != FALSE) {
                controller_.SaveSettings();
                ::DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DESTROY:
            ::KillTimer(hwnd_, kTickTimer);
            if (hotkey_registered_) {
                ::UnregisterHotKey(hwnd_, kHotkeyId);
            }
            soundboard_bridge_.Detach();
            if (webview_controller_) {
                webview_controller_->Close();
            }
            webview_.Reset();
            webview_controller_.Reset();
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

// ---------------------------------------------------------------------------
// WebView2 setup
// ---------------------------------------------------------------------------
void WebWindow::StartWebView() {
    auto environment_options = Make<CoreWebView2EnvironmentOptions>();
    if (options_.remote_debugging_port > 0) {
        environment_options->put_AdditionalBrowserArguments(
            std::format(L"--remote-debugging-port={}", options_.remote_debugging_port).c_str());
    }
    const std::wstring user_data = DataDirectory() + L"\\WebView2";
    const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), environment_options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) {
                return OnEnvironmentCreated(result, environment);
            })
            .Get());
    if (FAILED(hr)) {
        FailWebView(hr);
    }
}

HRESULT WebWindow::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment) {
    if (FAILED(result) || environment == nullptr || hwnd_ == nullptr) {
        FailWebView(FAILED(result) ? result : E_UNEXPECTED);
        return S_OK;
    }
    environment_ = environment;
    const HRESULT hr = environment_->CreateCoreWebView2Controller(
        hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this](HRESULT created, ICoreWebView2Controller* controller) {
                       return OnControllerCreated(created, controller);
                   })
                   .Get());
    if (FAILED(hr)) {
        FailWebView(hr);
    }
    return S_OK;
}

HRESULT WebWindow::OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || controller == nullptr || hwnd_ == nullptr) {
        FailWebView(FAILED(result) ? result : E_UNEXPECTED);
        return S_OK;
    }
    webview_controller_ = controller;
    if (FAILED(webview_controller_->get_CoreWebView2(&webview_)) || !webview_) {
        FailWebView(E_UNEXPECTED);
        return S_OK;
    }
    ConfigureWebView();
    ResizeWebView();
    webview_->Navigate(kUiStart);
    return S_OK;
}

void WebWindow::ConfigureWebView() {
    const bool developer = !options_.ui_directory.empty() || options_.remote_debugging_port > 0;

    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(webview_controller_.As(&controller2))) {
        controller2->put_DefaultBackgroundColor({255, 11, 11, 14});  // = kBackground
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(developer);
        settings->put_AreDefaultContextMenusEnabled(developer);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(developer);  // no Ctrl+R, Ctrl+F, ...
        }
        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings.As(&settings5))) {
            settings5->put_IsPinchZoomEnabled(FALSE);
        }
    }

    EventRegistrationToken token{};
    if (!options_.ui_directory.empty()) {
        ComPtr<ICoreWebView2_3> webview3;
        if (SUCCEEDED(webview_.As(&webview3))) {
            webview3->SetVirtualHostNameToFolderMapping(kUiHost, options_.ui_directory.c_str(),
                                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        }
    } else {
        webview_->AddWebResourceRequestedFilter(L"https://app.somike/*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                    ServeUiResource(args);
                    return S_OK;
                })
                .Get(),
            &token);
    }

    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                LPWSTR source = nullptr;
                if (FAILED(args->get_Source(&source)) || !TakeCoString(source).starts_with(kUiOrigin)) {
                    return S_OK;  // only our own page may drive the app
                }
                // Files dropped on the page arrive as additional objects
                // (chrome.webview.postMessageWithAdditionalObjects).
                std::vector<std::wstring> files;
                ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
                ComPtr<ICoreWebView2ObjectCollectionView> objects;
                UINT32 count = 0;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) &&
                    SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects &&
                    SUCCEEDED(objects->get_Count(&count))) {
                    for (UINT32 i = 0; i < count; ++i) {
                        ComPtr<IUnknown> object;
                        ComPtr<ICoreWebView2File> file;
                        LPWSTR path = nullptr;
                        if (SUCCEEDED(objects->GetValueAtIndex(i, &object)) && object &&
                            SUCCEEDED(object.As(&file)) && SUCCEEDED(file->get_Path(&path))) {
                            files.push_back(TakeCoString(path));
                        }
                    }
                }
                LPWSTR json = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&json))) {
                    OnWebMessage(text::ToUtf8(TakeCoString(json)), files);
                }
                return S_OK;
            })
            .Get(),
        &token);

    // The window only ever shows our UI.
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && !TakeCoString(uri).starts_with(kUiOrigin)) {
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                args->get_ProcessFailedKind(&kind);
                log::Error(L"WebView2 process failed (kind {}); reloading the UI", static_cast<int>(kind));
                ui_ready_ = false;
                if (webview_) {
                    webview_->Reload();
                }
                return S_OK;
            })
            .Get(),
        &token);
}

void WebWindow::ServeUiResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    LPWSTR raw_uri = nullptr;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&raw_uri))) {
        return;
    }
    std::wstring path = TakeCoString(raw_uri);
    path.erase(0, kUiOrigin.size() - 1);  // keep the leading '/'
    path = path.substr(0, path.find_first_of(L"?#"));

    ComPtr<ICoreWebView2WebResourceResponse> response;
    const auto file = std::find_if(std::begin(kUiFiles), std::end(kUiFiles),
                                   [&](const UiFile& f) { return f.path == path; });
    HRSRC resource = file != std::end(kUiFiles)
                         ? ::FindResourceW(nullptr, MAKEINTRESOURCEW(file->resource), RT_RCDATA)
                         : nullptr;
    HGLOBAL loaded = resource != nullptr ? ::LoadResource(nullptr, resource) : nullptr;
    const void* bytes = loaded != nullptr ? ::LockResource(loaded) : nullptr;
    if (bytes != nullptr) {
        ComPtr<IStream> stream;
        stream.Attach(::SHCreateMemStream(static_cast<const BYTE*>(bytes),
                                          ::SizeofResource(nullptr, resource)));
        const std::wstring headers =
            std::format(L"Content-Type: {}\r\nCache-Control: no-store\r\nContent-Security-Policy: {}",
                        file->content_type, kContentSecurityPolicy);
        environment_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    } else {
        environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    }
    args->put_Response(response.Get());
}

void WebWindow::FailWebView(HRESULT hr) {
    log::Error(L"WebView2 unavailable: {}", win32::FormatHResult(hr));
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        const int choice = ::MessageBoxW(
            hwnd_,
            L"Somike needs the Microsoft Edge WebView2 Runtime, which is part of Windows 11 "
            L"and most Windows 10 installations.\n\nOpen the download page now?",
            L"Somike", MB_YESNO | MB_ICONINFORMATION);
        if (choice == IDYES) {
            ::ShellExecuteW(hwnd_, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                            nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else {
        const std::wstring message = L"The Somike window could not be created:\n" + win32::FormatHResult(hr);
        ::MessageBoxW(hwnd_, message.c_str(), L"Somike", MB_OK | MB_ICONERROR);
    }
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
    }
}

void WebWindow::ResizeWebView() {
    if (webview_controller_) {
        RECT bounds{};
        ::GetClientRect(hwnd_, &bounds);
        webview_controller_->put_Bounds(bounds);
    }
}

// ---------------------------------------------------------------------------
// UI bridge
// ---------------------------------------------------------------------------
void WebWindow::OnWebMessage(const std::string& json, const std::vector<std::wstring>& dropped_files) {
    const std::optional<UiCommand> command = ParseCommand(json);
    if (!command) {
        return;
    }
    const std::string& cmd = command->cmd;
    const std::wstring id = text::FromUtf8(command->id);
    if (cmd.starts_with("sb.")) {
        soundboard_bridge_.HandleCommand(cmd, json, dropped_files);
        return;
    }
    if (cmd == "ready") {
        ui_ready_ = true;
        sent_icons_.clear();
        last_state_json_.clear();
        soundboard_bridge_.OnUiReady();
    } else if (cmd == "selectSource") {
        controller_.SelectSource(id);
    } else if (cmd == "toggleLive") {
        controller_.ToggleLive();
    } else if (cmd == "setGain" && command->number) {
        controller_.SetInjectGain(static_cast<float>(*command->number));
    } else if (cmd == "setMuteMic" && command->flag) {
        controller_.SetMuteMicrophone(*command->flag);
    } else if (cmd == "selectMic") {
        controller_.SelectMicrophone(id);
    } else if (cmd == "setup" || cmd == "repair") {
        RunInstaller(InstallerAction::Install, nullptr);
    } else if (cmd == "uninstall") {
        const std::wstring question =
            L"Remove Somike from " + ShortDeviceName(controller_.selected_microphone_name()) +
            L"?\n\nThe microphone goes back to exactly how it was. Windows Audio restarts briefly.";
        if (::MessageBoxW(hwnd_, question.c_str(), L"Somike", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) ==
            IDYES) {
            controller_.SetLive(false);
            RunInstaller(InstallerAction::Uninstall, nullptr);
        }
    } else if (cmd == "refresh") {
        controller_.RefreshNow();
    } else if (cmd == "openLogs") {
        ::ShellExecuteW(hwnd_, L"open", DataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        log::Warn(L"Unknown UI command '{}'", text::FromUtf8(cmd));
        return;
    }
    PushIcons();
    PushState(false);
    UpdateTitle();
}

void WebWindow::Tick() {
    controller_.Tick(::GetTickCount64());
    PushIcons();
    PushState(false);
    soundboard_bridge_.Tick();
    UpdateTitle();
}

void WebWindow::PushState(bool force) {
    if (!ui_ready_ || !webview_) {
        return;
    }
    std::string json = SerializeStateMessage(controller_.BuildUiState());
    if (!force && json == last_state_json_) {
        return;
    }
    last_state_json_ = std::move(json);
    PostJson(last_state_json_);
}

void WebWindow::PushIcons() {
    if (!ui_ready_ || !webview_) {
        return;
    }
    std::map<std::string, std::string> batch;
    for (const SourceEntry& source : controller_.sources()) {
        if (source.image_path.empty()) {
            continue;
        }
        const std::string key = text::ToUtf8(source.id);
        if (!sent_icons_.insert(key).second) {
            continue;
        }
        if (auto url = icons_.DataUrl(source.image_path)) {
            batch.emplace(key, std::move(*url));
        }
    }
    if (!batch.empty()) {
        PostJson(SerializeIconsMessage(batch));
    }
}

void WebWindow::PostJson(const std::string& json) {
    if (webview_) {
        webview_->PostWebMessageAsJson(text::FromUtf8(json).c_str());
    }
}

void WebWindow::Toast(const std::wstring& message, bool error) {
    PostJson(SerializeToastMessage(text::ToUtf8(message), error));
}

void WebWindow::UpdateTitle() {
    std::wstring title = L"Somike";
    if (controller_.live_requested()) {
        const LivePhase phase = controller_.live_phase();
        title += phase == LivePhase::Live      ? L" • Live: " + controller_.selected_source_name()
                 : phase == LivePhase::Waiting ? L" • Waiting for " + controller_.selected_source_name()
                                               : std::wstring(L" • Reconnecting");
    }
    if (title != title_) {
        title_ = title;
        ::SetWindowTextW(hwnd_, title_.c_str());
    }
}

// ---------------------------------------------------------------------------
// one-time setup (elevated)
// ---------------------------------------------------------------------------
void WebWindow::RunInstaller(InstallerAction action, const wchar_t* strategy) {
    if (installer_running_) {
        return;
    }
    const std::wstring mic = controller_.selected_microphone_id();
    if (mic.empty()) {
        Toast(L"No microphone found. Plug one in and try again.", true);
        return;
    }
    installer_running_ = true;
    installer_action_ = action;
    controller_.SetSetupProgress(
        true,
        action == InstallerAction::Install ? L"Allow the Windows prompt. Sound pauses for a moment…"
                                           : L"Removing…",
        false);
    PushState(false);

    std::wstring arguments = action == InstallerAction::Install ? L"install" : L"uninstall";
    arguments += L" --endpoint \"" + mic + L"\" --restart-audio";
    if (strategy != nullptr) {
        arguments += std::wstring(L" --strategy ") + strategy;
    }
    log::Info(L"Running somike-apoctl {}", arguments);

    const HWND hwnd = hwnd_;
    std::thread([hwnd, arguments] {
        auto result = std::make_unique<core::ElevatedToolResult>();
        {
            // ShellExecuteEx (UAC) wants an STA on the calling thread.
            const core::ComInit com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            *result = core::RunApoCtlElevated(hwnd, arguments);
        }
        if (::PostMessageW(hwnd, WM_APP_INSTALLER_DONE, 0, reinterpret_cast<LPARAM>(result.get()))) {
            static_cast<void>(result.release());  // the window owns it now
        }
    }).detach();
}

void WebWindow::OnInstallerFinished(std::unique_ptr<core::ElevatedToolResult> result) {
    installer_running_ = false;
    const bool install = installer_action_ == InstallerAction::Install;
    if (!result->report.empty()) {
        log::Info(L"somike-apoctl (exit {}):\n{}", result->exit_code, result->report);
    }

    if (FAILED(result->hr)) {
        controller_.SetSetupProgress(
            false,
            result->hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)
                ? L"Somike needs that Windows permission once. Click “Set up” to try again."
                : L"Could not start the installer: " + win32::FormatHResult(result->hr),
            true);
    } else if (install && result->exit_code == kExitRefused) {
        controller_.SetSetupProgress(false, L"", false);
        const int choice = ::MessageBoxW(
            hwnd_,
            L"This microphone already uses an audio effect from its driver, for example noise "
            L"suppression.\n\nSomike can run right after that effect and keep it working. This mode "
            L"is still experimental. Continue?",
            L"Somike", MB_YESNO | MB_ICONQUESTION);
        if (choice == IDYES) {
            RunInstaller(InstallerAction::Install, L"composite");
            return;
        }
    } else if (result->exit_code != 0) {
        const std::wstring detail = LastLine(result->report);
        controller_.SetSetupProgress(false, detail.empty() ? L"Setup failed. See the log for details." : detail,
                                     true);
    } else {
        controller_.SetSetupProgress(false, L"", false);
        Toast(install ? L"Connected to " + ShortDeviceName(controller_.selected_microphone_name()) +
                            L". Pick an app and go live."
                      : std::wstring(L"Somike was removed from this microphone."),
              false);
    }
    controller_.RefreshNow();
    PushState(false);
}

}  // namespace somike::app
