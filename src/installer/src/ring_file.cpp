#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <iterator>
#include <optional>
#include <string>

#include "somike/common/hresult.h"
#include "somike/common/mapped_file.h"
#include "somike/common/ring_path.h"
#include "somike/common/unique_handle.h"
#include "somike/dsp/ring_writer.h"
#include "somike/installer/system_install.h"
#include "somike/protocol/shared_ring.h"

namespace somike::installer {

namespace proto = somike::protocol;

namespace {

// Owns a self-relative security descriptor built from SDDL.
class SecurityDescriptor {
public:
    explicit SecurityDescriptor(const std::wstring& sddl) {
        ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                               &descriptor_, nullptr);
    }
    ~SecurityDescriptor() {
        if (descriptor_ != nullptr) {
            ::LocalFree(descriptor_);
        }
    }
    SecurityDescriptor(const SecurityDescriptor&) = delete;
    SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;

    [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept { return descriptor_; }

    [[nodiscard]] PACL dacl() const noexcept {
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL acl = nullptr;
        if (descriptor_ == nullptr ||
            !::GetSecurityDescriptorDacl(descriptor_, &present, &acl, &defaulted) || !present) {
            return nullptr;
        }
        return acl;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};

// SID of the Windows Audio service ("NT SERVICE\AudioSrv"). audiodg.exe runs
// with this service SID in its token; files in user-controlled locations are
// only reachable for it if granted explicitly.
std::optional<std::wstring> AudioServiceSid() {
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    wchar_t domain[256];
    DWORD domain_size = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use{};
    if (!::LookupAccountNameW(nullptr, L"NT SERVICE\\AudioSrv", sid, &sid_size, domain,
                              &domain_size, &use)) {
        return std::nullopt;
    }
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) {
        return std::nullopt;
    }
    std::wstring result(text);
    ::LocalFree(text);
    return result;
}

HRESULT ApplyDacl(const std::wstring& path, const SecurityDescriptor& sd) {
    PACL dacl = sd.dacl();
    if (dacl == nullptr) {
        return E_UNEXPECTED;
    }
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
        nullptr);
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

}  // namespace

HRESULT CreateRingFile(Report& report) {
    const std::wstring directory = paths::DataDirectory();
    const std::wstring file = paths::RingFile();

    // Directory: SYSTEM and Administrators full control, Users read-only, so
    // nobody else can plant or replace files in it.
    const SecurityDescriptor directory_sd(
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
    if (directory_sd.get() == nullptr) {
        return win32::LastErrorAsHResult();
    }
    SECURITY_ATTRIBUTES directory_sa{sizeof(directory_sa), directory_sd.get(), FALSE};
    if (!::CreateDirectoryW(directory.c_str(), &directory_sa)) {
        if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            return win32::LastErrorAsHResult();
        }
        if (const HRESULT hr = ApplyDacl(directory, directory_sd); FAILED(hr)) {
            return hr;
        }
    }

    // Ring file: interactive users (the app) and the audio service (the APO)
    // may read and write; nobody but SYSTEM/Administrators may delete it or
    // change its permissions.
    std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;LS)(A;;FRFW;;;IU)";
    if (const auto audiosrv = AudioServiceSid()) {
        sddl += L"(A;;FRFW;;;" + *audiosrv + L")";
    } else {
        report.emplace_back(
            L"Warning: could not resolve NT SERVICE\\AudioSrv; relying on LOCAL SERVICE access.");
    }
    const SecurityDescriptor file_sd(sddl);
    if (file_sd.get() == nullptr) {
        return win32::LastErrorAsHResult();
    }
    SECURITY_ATTRIBUTES file_sa{sizeof(file_sa), file_sd.get(), FALSE};

    win32::UniqueFileHandle handle(::CreateFileW(
        file.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &file_sa, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
    if (!handle) {
        return win32::LastErrorAsHResult();
    }
    const bool existed = ::GetLastError() == ERROR_ALREADY_EXISTS;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.get(), &size)) {
        return win32::LastErrorAsHResult();
    }
    if (static_cast<unsigned long long>(size.QuadPart) != proto::kRegionBytes) {
        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(proto::kRegionBytes);
        if (!::SetFilePointerEx(handle.get(), target, nullptr, FILE_BEGIN) ||
            !::SetEndOfFile(handle.get())) {
            const HRESULT hr = win32::LastErrorAsHResult();
            report.push_back(L"Cannot resize " + file + L" (is it in use?): " +
                             win32::FormatHResult(hr));
            return hr;
        }
    }
    handle.reset();

    if (existed) {
        if (const HRESULT hr = ApplyDacl(file, file_sd); FAILED(hr)) {
            return hr;
        }
    }

    // Initialise the header (RingWriter does so when it is not yet valid).
    win32::MappedFile mapping;
    if (const HRESULT hr =
            mapping.Open(file, proto::kRegionBytes, win32::MappedFile::Access::ReadWrite);
        FAILED(hr)) {
        return hr;
    }
    const dsp::RingWriter writer(mapping.bytes());
    if (!writer.valid()) {
        return E_UNEXPECTED;
    }
    report.push_back(L"Shared ring ready: " + file);

    // APO diagnostics log, same permissions; truncated on every install. The
    // APO cannot create files here itself (the directory is read-only for it).
    const std::wstring log_file = paths::ApoLogFile();
    win32::UniqueFileHandle log(::CreateFileW(log_file.c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &file_sa,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log) {
        report.push_back(L"Warning: could not create " + log_file + L": " +
                         win32::FormatHResult(win32::LastErrorAsHResult()));
    } else {
        log.reset();
        (void)ApplyDacl(log_file, file_sd);  // CREATE_ALWAYS keeps an existing file's DACL
    }
    return S_OK;
}

HRESULT DeleteRingFile(Report& report) {
    const std::wstring file = paths::RingFile();
    ::DeleteFileW(paths::ApoLogFile().c_str());  // best effort
    if (::DeleteFileW(file.c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND) {
        ::RemoveDirectoryW(paths::DataDirectory().c_str());  // only succeeds if empty
        return S_OK;
    }
    const HRESULT hr = win32::LastErrorAsHResult();
    report.push_back(L"Could not delete " + file + L" (still in use?): " +
                     win32::FormatHResult(hr));
    return hr;
}

}  // namespace somike::installer
