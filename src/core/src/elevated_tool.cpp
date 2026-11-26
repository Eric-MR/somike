#include "somike/core/elevated_tool.h"

#include <shellapi.h>

#include <iterator>
#include <string>

#include "somike/common/hresult.h"
#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"

namespace somike::core {

namespace {

std::wstring TempReportPath() {
    wchar_t temp[MAX_PATH + 1];
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    std::wstring directory = length != 0 ? std::wstring(temp, length) : std::wstring(L".\\");
    return directory + L"somike-apoctl-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(::GetTickCount64()) + L".txt";
}

std::wstring ReadUtf8File(const std::wstring& path) {
    win32::UniqueFileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return {};
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        return {};
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    bytes.resize(read);
    return text::FromUtf8(bytes);
}

}  // namespace

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L".";
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

ElevatedToolResult RunApoCtlElevated(HWND owner, const std::wstring& arguments) {
    ElevatedToolResult result;
    const std::wstring tool = ExecutableDirectory() + L"\\" + kApoCtlExecutable;
    const std::wstring report = TempReportPath();
    const std::wstring parameters = arguments + L" --report \"" + report + L"\"";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = tool.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&info)) {
        result.hr = win32::LastErrorAsHResult();
        return result;
    }
    win32::UniqueHandle process(info.hProcess);
    if (!process) {
        result.hr = E_UNEXPECTED;
        return result;
    }
    ::WaitForSingleObject(process.get(), INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.get(), &exit_code);

    result.hr = S_OK;
    result.exit_code = exit_code;
    result.report = ReadUtf8File(report);
    ::DeleteFileW(report.c_str());
    return result;
}

}  // namespace somike::core
