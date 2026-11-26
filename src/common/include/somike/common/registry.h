#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace somike::reg {

// A registry value exactly as stored (type + bytes); used for lossless backups.
struct RawValue {
    DWORD type = REG_NONE;
    std::vector<std::uint8_t> data;

    bool operator==(const RawValue&) const = default;
};

// REG_MULTI_SZ codec. Parsing tolerates missing terminators and skips empty
// entries; serialising an empty list yields a single terminator pair.
[[nodiscard]] std::vector<std::wstring> ParseMultiString(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> SerializeMultiString(const std::vector<std::wstring>& items);

// Canonical encodings. Key::Write* use exactly these, so a value written by
// us can later be recognised byte-for-byte (see installer backups).
[[nodiscard]] RawValue MakeString(std::wstring_view value);
[[nodiscard]] RawValue MakeMultiString(const std::vector<std::wstring>& items);
[[nodiscard]] RawValue MakeDword(DWORD value);

// Move-only owner of an HKEY.
class Key {
public:
    Key() = default;
    explicit Key(HKEY key) noexcept : key_(key) {}
    ~Key() { reset(); }
    Key(Key&& other) noexcept : key_(other.release()) {}
    Key& operator=(Key&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

    [[nodiscard]] static HRESULT Open(HKEY root, const std::wstring& path, REGSAM access,
                                      Key& out) noexcept;
    [[nodiscard]] static HRESULT Create(HKEY root, const std::wstring& path, REGSAM access,
                                        Key& out) noexcept;

    // Opens (or creates) the key with REG_OPTION_BACKUP_RESTORE. With
    // SeRestorePrivilege enabled this bypasses the key's DACL, which is how
    // we write to TrustedInstaller-owned MMDevices keys without permanently
    // changing their ownership or ACLs. The handle has KEY_WRITE access only.
    [[nodiscard]] static HRESULT CreateForRestore(HKEY root, const std::wstring& path,
                                                  Key& out) noexcept;

    [[nodiscard]] HKEY get() const noexcept { return key_; }
    [[nodiscard]] bool valid() const noexcept { return key_ != nullptr; }

    [[nodiscard]] std::optional<RawValue> ReadRaw(const std::wstring& name) const;
    [[nodiscard]] std::optional<std::wstring> ReadString(const std::wstring& name) const;
    [[nodiscard]] std::optional<std::vector<std::wstring>> ReadMultiString(
        const std::wstring& name) const;
    [[nodiscard]] std::optional<DWORD> ReadDword(const std::wstring& name) const;

    [[nodiscard]] HRESULT WriteRaw(const std::wstring& name, const RawValue& value) const;
    [[nodiscard]] HRESULT WriteString(const std::wstring& name, std::wstring_view value) const;
    [[nodiscard]] HRESULT WriteMultiString(const std::wstring& name,
                                           const std::vector<std::wstring>& value) const;
    [[nodiscard]] HRESULT WriteDword(const std::wstring& name, DWORD value) const;

    // S_OK if the value was deleted or did not exist.
    [[nodiscard]] HRESULT DeleteValue(const std::wstring& name) const;

    [[nodiscard]] std::vector<std::wstring> SubKeyNames() const;

private:
    [[nodiscard]] HKEY release() noexcept {
        HKEY key = key_;
        key_ = nullptr;
        return key;
    }
    void reset(HKEY key = nullptr) noexcept {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
        }
        key_ = key;
    }

    HKEY key_ = nullptr;
};

// Deletes a key and all its subkeys. S_OK if it did not exist.
[[nodiscard]] HRESULT DeleteTree(HKEY root, const std::wstring& path) noexcept;

// Enables a privilege (e.g. SE_RESTORE_NAME) on the process token.
[[nodiscard]] HRESULT EnablePrivilege(const wchar_t* privilege) noexcept;

}  // namespace somike::reg
