// The main window: a plain Win32 window hosting the HTML UI in WebView2.
//
// * The UI (src/app/ui) is compiled into the executable as resources and
//   served from https://app.somike/ - no files on disk, no web server.
// * State flows one way: AppController -> UiState JSON -> UI, ~4 times per
//   second (only when it changed). User actions come back as small commands.
// * Elevated work (installing the injector) runs on a worker thread.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <WebView2.h>

#include <memory>
#include <set>
#include <string>

#include "app_controller.h"
#include "icon_cache.h"
#include "somike/core/elevated_tool.h"

namespace somike::app {

inline constexpr wchar_t kMainWindowClass[] = L"Somike.MainWindow";

struct WindowOptions {
    std::wstring ui_directory;      // development: load the UI from this folder
    int remote_debugging_port = 0;  // development/testing: expose DevTools protocol
};

class WebWindow {
public:
    WebWindow(AppController& controller, WindowOptions options);
    ~WebWindow();
    WebWindow(const WebWindow&) = delete;
    WebWindow& operator=(const WebWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, int show);
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    enum class InstallerAction { Install, Uninstall };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void ApplyWindowTheme();
    void StartWebView();
    HRESULT OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment);
    HRESULT OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);
    void ConfigureWebView();
    void ServeUiResource(ICoreWebView2WebResourceRequestedEventArgs* args);
    void FailWebView(HRESULT hr);
    void ResizeWebView();

    void OnWebMessage(const std::string& json);
    void Tick();
    void PushState(bool force);
    void PushIcons();
    void PostJson(const std::string& json);
    void Toast(const std::wstring& text, bool error);
    void UpdateTitle();

    void RunInstaller(InstallerAction action, const wchar_t* strategy);
    void OnInstallerFinished(std::unique_ptr<core::ElevatedToolResult> result);

    AppController& controller_;
    WindowOptions options_;
    HWND hwnd_ = nullptr;
    HBRUSH background_ = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webview_controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    bool ui_ready_ = false;
    std::string last_state_json_;
    std::set<std::string> sent_icons_;
    IconCache icons_;
    std::wstring title_;

    bool hotkey_registered_ = false;
    bool installer_running_ = false;
    InstallerAction installer_action_ = InstallerAction::Install;
};

}  // namespace somike::app
