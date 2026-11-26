#include "somike/common/registry.h"

#include <cstring>
#include <iterator>

#include "somike/common/unique_handle.h"

namespace somike::reg {

namespace {

HRESULT FromStatus(LSTATUS status) noexcept {
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

}  // namespace

std::vector<std::wstring> ParseMultiString(std::span<const std::uint8_t> data) {
    std::vector<std::wstring> items;
    const std::size_t count = data.size() / sizeof(wchar_t);
    std::wstring chars(count, L'\0');
    if (count != 0) {
        std::memcpy(chars.data(), data.data(), count * sizeof(wchar_t));
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i <= chars.size(); ++i) {
        if (i == chars.size() || chars[i] == L'\0') {
            if (i > start) {
                items.emplace_back(chars, start, i - start);
            }
            start = i + 1;
        }
    }
    return items;
}

std::vector<std::uint8_t> SerializeMultiString(const std::vector<std::wstring>& items) {
    std::wstring chars;
    for (const std::wstring& item : items) {
        if (item.empty()) {
            continue;  // an empty entry would terminate the list early
        }
        chars.append(item);
        chars.push_back(L'\0');
    }
    chars.push_back(L'\0');
    if (chars.size() == 1) {
        chars.push_back(L'\0');
    }
    std::vector<std::uint8_t> data(chars.size() * sizeof(wchar_t));
    std::memcpy(data.data(), chars.data(), data.size());
    return data;
}

RawValue MakeString(std::wstring_view value) {
    const std::wstring terminated(value);
    RawValue raw;
    raw.type = REG_SZ;
    raw.data.resize((terminated.size() + 1) * sizeof(wchar_t));
    std::memcpy(raw.data.data(), terminated.c_str(), raw.data.size());
    return raw;
}

RawValue MakeMultiString(const std::vector<std::wstring>& items) {
    RawValue raw;
    raw.type = REG_MULTI_SZ;
    raw.data = SerializeMultiString(items);
    return raw;
}

RawValue MakeDword(DWORD value) {
    RawValue raw;
    raw.type = REG_DWORD;
    raw.data.resize(sizeof(value));
    std::memcpy(raw.data.data(), &value, sizeof(value));
    return raw;
}

HRESULT Key::Open(HKEY root, const std::wstring& path, REGSAM access, Key& out) noexcept {
    HKEY key = nullptr;
    const HRESULT hr = FromStatus(::RegOpenKeyExW(root, path.c_str(), 0, access, &key));
    if (SUCCEEDED(hr)) {
        out = Key(key);
    }
    return hr;
}

HRESULT Key::Create(HKEY root, const std::wstring& path, REGSAM access, Key& out) noexcept {
    HKEY key = nullptr;
    const HRESULT hr = FromStatus(::RegCreateKeyExW(root, path.c_str(), 0, nullptr,
                                                    REG_OPTION_NON_VOLATILE, access, nullptr, &key,
                                                    nullptr));
    if (SUCCEEDED(hr)) {
        out = Key(key);
    }
    return hr;
}

HRESULT Key::CreateForRestore(HKEY root, const std::wstring& path, Key& out) noexcept {
    HKEY key = nullptr;
    // samDesired is ignored with REG_OPTION_BACKUP_RESTORE.
    const HRESULT hr = FromStatus(::RegCreateKeyExW(root, path.c_str(), 0, nullptr,
                                                    REG_OPTION_BACKUP_RESTORE, 0, nullptr, &key,
                                                    nullptr));
    if (SUCCEEDED(hr)) {
        out = Key(key);
    }
    return hr;
}

std::optional<RawValue> Key::ReadRaw(const std::wstring& name) const {
    if (!valid()) {
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD type = REG_NONE;
        DWORD size = 0;
        LSTATUS status = ::RegQueryValueExW(key_, name.c_str(), nullptr, &type, nullptr, &size);
        if (status != ERROR_SUCCESS) {
            return std::nullopt;
        }
        RawValue value;
        value.type = type;
        value.data.resize(size);
        status = ::RegQueryValueExW(key_, name.c_str(), nullptr, &type,
                                    value.data.empty() ? nullptr : value.data.data(), &size);
        if (status == ERROR_MORE_DATA) {
            continue;  // value grew between the two calls
        }
        if (status != ERROR_SUCCESS) {
            return std::nullopt;
        }
        value.type = type;
        value.data.resize(size);
        return value;
    }
    return std::nullopt;
}

std::optional<std::wstring> Key::ReadString(const std::wstring& name) const {
    const std::optional<RawValue> raw = ReadRaw(name);
    if (!raw || (raw->type != REG_SZ && raw->type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(raw->data.size() / sizeof(wchar_t), L'\0');
    if (!value.empty()) {
        std::memcpy(value.data(), raw->data.data(), value.size() * sizeof(wchar_t));
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::optional<std::vector<std::wstring>> Key::ReadMultiString(const std::wstring& name) const {
    const std::optional<RawValue> raw = ReadRaw(name);
    if (!raw || raw->type != REG_MULTI_SZ) {
        return std::nullopt;
    }
    return ParseMultiString(raw->data);
}

std::optional<DWORD> Key::ReadDword(const std::wstring& name) const {
    const std::optional<RawValue> raw = ReadRaw(name);
    if (!raw || raw->type != REG_DWORD || raw->data.size() != sizeof(DWORD)) {
        return std::nullopt;
    }
    DWORD value = 0;
    std::memcpy(&value, raw->data.data(), sizeof(value));
    return value;
}

HRESULT Key::WriteRaw(const std::wstring& name, const RawValue& value) const {
    if (!valid()) {
        return E_HANDLE;
    }
    return FromStatus(::RegSetValueExW(key_, name.c_str(), 0, value.type,
                                       value.data.empty() ? nullptr : value.data.data(),
                                       static_cast<DWORD>(value.data.size())));
}

HRESULT Key::WriteString(const std::wstring& name, std::wstring_view value) const {
    return WriteRaw(name, MakeString(value));
}

HRESULT Key::WriteMultiString(const std::wstring& name,
                              const std::vector<std::wstring>& value) const {
    return WriteRaw(name, MakeMultiString(value));
}

HRESULT Key::WriteDword(const std::wstring& name, DWORD value) const {
    return WriteRaw(name, MakeDword(value));
}

HRESULT Key::DeleteValue(const std::wstring& name) const {
    if (!valid()) {
        return E_HANDLE;
    }
    const LSTATUS status = ::RegDeleteValueW(key_, name.c_str());
    return status == ERROR_FILE_NOT_FOUND ? S_OK : FromStatus(status);
}

std::vector<std::wstring> Key::SubKeyNames() const {
    std::vector<std::wstring> names;
    if (!valid()) {
        return names;
    }
    wchar_t buffer[256];
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(std::size(buffer));
        const LSTATUS status =
            ::RegEnumKeyExW(key_, index, buffer, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status == ERROR_SUCCESS) {
            names.emplace_back(buffer, length);
        }
    }
    return names;
}

HRESULT DeleteTree(HKEY root, const std::wstring& path) noexcept {
    const LSTATUS status = ::RegDeleteTreeW(root, path.c_str());
    if (status == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }
    if (status != ERROR_SUCCESS) {
        return FromStatus(status);
    }
    // RegDeleteTree removes the contents; remove the (now empty) key itself.
    const LSTATUS self = ::RegDeleteKeyExW(root, path.c_str(), KEY_WOW64_64KEY, 0);
    return self == ERROR_FILE_NOT_FOUND ? S_OK : FromStatus(self);
}

HRESULT EnablePrivilege(const wchar_t* privilege) noexcept {
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                            &raw_token)) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    win32::UniqueHandle token(raw_token);

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::LookupPrivilegeValueW(nullptr, privilege, &privileges.Privileges[0].Luid)) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    if (!::AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr,
                                 nullptr)) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    // AdjustTokenPrivileges "succeeds" even if the privilege is not held.
    if (::GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        return HRESULT_FROM_WIN32(ERROR_NOT_ALL_ASSIGNED);
    }
    return S_OK;
}

}  // namespace somike::reg
