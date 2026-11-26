#include <windows.h>
#include <audioenginebaseapo.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>

#include "somike/common/guid.h"
#include "somike/common/hresult.h"
#include "somike/common/registry.h"
#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"
#include "somike/installer/system_install.h"
#include "somike/protocol/apo_identity.h"

namespace somike::installer {

namespace proto = somike::protocol;

namespace {

constexpr REGSAM kWrite = KEY_WRITE | KEY_WOW64_64KEY;

std::wstring ClsidKeyPath() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + proto::kApoClsidString;
}

std::wstring ApoKeyPath() {
    return std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") +
           proto::kApoClsidString;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        return {};
    }
    std::wstring result(path);
    ::CoTaskMemFree(path);
    return result;
}

std::wstring FullPath(const std::wstring& path) {
    const DWORD size = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) {
        return path;
    }
    std::wstring full(size, L'\0');
    const DWORD written = ::GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    full.resize(written);
    return full;
}

#define SOMIKE_RETURN_IF_FAILED(expr) \
    do {                              \
        const HRESULT hr_ = (expr);   \
        if (FAILED(hr_)) {            \
            return hr_;               \
        }                             \
    } while (false)

}  // namespace

bool IsProcessElevated() noexcept {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return false;
    }
    win32::UniqueHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return ::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation),
                                 &size) &&
           elevation.TokenIsElevated != 0;
}

HRESULT EnableInstallerPrivileges() noexcept {
    SOMIKE_RETURN_IF_FAILED(reg::EnablePrivilege(SE_RESTORE_NAME));
    SOMIKE_RETURN_IF_FAILED(reg::EnablePrivilege(SE_BACKUP_NAME));
    return S_OK;
}

std::wstring ApoInstallDirectory() {
    const std::wstring program_files = KnownFolder(FOLDERID_ProgramFiles);
    return program_files.empty() ? std::wstring{} : program_files + L"\\Somike";
}

HRESULT DeployApoDll(const std::wstring& source, std::wstring& installed_path, Report& report) {
    if (::GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        report.push_back(L"APO DLL not found: " + source);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const std::wstring directory = ApoInstallDirectory();
    if (directory.empty()) {
        return E_UNEXPECTED;
    }
    if (!::CreateDirectoryW(directory.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return win32::LastErrorAsHResult();
    }

    installed_path = directory + L"\\" + proto::kApoDllName;
    if (text::EqualsIgnoreCase(FullPath(source), installed_path)) {
        return S_OK;  // already running from the install location
    }
    if (::CopyFileW(source.c_str(), installed_path.c_str(), FALSE)) {
        report.push_back(L"Copied APO to " + installed_path);
        return S_OK;
    }

    const DWORD error = ::GetLastError();
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED &&
        error != ERROR_USER_MAPPED_FILE) {
        return HRESULT_FROM_WIN32(error);
    }
    // The previous DLL is loaded by audiodg.exe: it cannot be overwritten, but
    // it can be renamed. Park it and delete it at the next reboot.
    const std::wstring parked = installed_path + L".old-" + std::to_wstring(::GetTickCount64());
    if (!::MoveFileExW(installed_path.c_str(), parked.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return win32::LastErrorAsHResult();
    }
    ::MoveFileExW(parked.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!::CopyFileW(source.c_str(), installed_path.c_str(), FALSE)) {
        return win32::LastErrorAsHResult();
    }
    report.push_back(L"Replaced the in-use APO at " + installed_path +
                     L"; the new version loads after the audio service restarts.");
    return S_OK;
}

HRESULT RegisterApo(const std::wstring& dll_path) {
    reg::Key clsid;
    SOMIKE_RETURN_IF_FAILED(reg::Key::Create(HKEY_LOCAL_MACHINE, ClsidKeyPath(), kWrite, clsid));
    SOMIKE_RETURN_IF_FAILED(clsid.WriteString(L"", proto::kApoFriendlyName));

    reg::Key server;
    SOMIKE_RETURN_IF_FAILED(reg::Key::Create(HKEY_LOCAL_MACHINE, ClsidKeyPath() + L"\\InprocServer32",
                                             kWrite, server));
    SOMIKE_RETURN_IF_FAILED(server.WriteString(L"", dll_path));
    SOMIKE_RETURN_IF_FAILED(server.WriteString(L"ThreadingModel", L"Both"));

    // Mirrors APO_REG_PROPERTIES as returned by the APO itself.
    reg::Key apo;
    SOMIKE_RETURN_IF_FAILED(reg::Key::Create(HKEY_LOCAL_MACHINE, ApoKeyPath(), kWrite, apo));
    SOMIKE_RETURN_IF_FAILED(apo.WriteString(L"FriendlyName", proto::kApoFriendlyName));
    SOMIKE_RETURN_IF_FAILED(apo.WriteString(L"Copyright", proto::kApoCopyright));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MajorVersion", proto::kApoMajorVersion));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MinorVersion", proto::kApoMinorVersion));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"Flags", proto::kApoFlags));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MinInputConnections", 1));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MaxInputConnections", 1));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MinOutputConnections", 1));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MaxOutputConnections", 1));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"MaxInstances", 0xFFFFFFFF));
    SOMIKE_RETURN_IF_FAILED(apo.WriteDword(L"NumAPOInterfaces", 1));
    SOMIKE_RETURN_IF_FAILED(
        apo.WriteString(L"APOInterface0", guid::ToString(__uuidof(IAudioProcessingObject))));
    return S_OK;
}

HRESULT UnregisterApo() {
    SOMIKE_RETURN_IF_FAILED(reg::DeleteTree(HKEY_LOCAL_MACHINE, ApoKeyPath()));
    SOMIKE_RETURN_IF_FAILED(reg::DeleteTree(HKEY_LOCAL_MACHINE, ClsidKeyPath()));
    return S_OK;
}

std::optional<std::wstring> RegisteredApoPath() {
    reg::Key server;
    if (FAILED(reg::Key::Open(HKEY_LOCAL_MACHINE, ClsidKeyPath() + L"\\InprocServer32",
                              KEY_READ | KEY_WOW64_64KEY, server))) {
        return std::nullopt;
    }
    return server.ReadString(L"");
}

}  // namespace somike::installer
