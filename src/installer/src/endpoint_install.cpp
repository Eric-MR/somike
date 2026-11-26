#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "somike/common/hresult.h"
#include "somike/common/registry.h"
#include "somike/installer/system_install.h"

namespace somike::installer {

namespace {

constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;
constexpr REGSAM kWrite = KEY_WRITE | KEY_WOW64_64KEY;
constexpr wchar_t kKindFx[] = L"fx";
constexpr wchar_t kKindProperties[] = L"props";

// One value we changed: what it was before, and exactly what we wrote.
struct BackupEntry {
    std::wstring kind;  // kKindFx or kKindProperties
    std::wstring name;
    std::optional<reg::RawValue> original;
    std::optional<reg::RawValue> written;
};

std::wstring KeyPathFor(const EndpointId& id, const std::wstring& kind) {
    return kind == kKindProperties ? PropertiesKeyPath(id) : FxPropertiesKeyPath(id);
}

std::wstring UtcTimestamp() {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    wchar_t buffer[32];
    std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

HRESULT SaveBackup(const EndpointId& id, Strategy strategy,
                   const std::vector<BackupEntry>& entries) {
    const std::wstring root = BackupKeyPath(id);
    if (const HRESULT hr = reg::DeleteTree(HKEY_LOCAL_MACHINE, root); FAILED(hr)) {
        return hr;
    }
    reg::Key key;
    if (const HRESULT hr = reg::Key::Create(HKEY_LOCAL_MACHINE, root, kWrite, key); FAILED(hr)) {
        return hr;
    }
    HRESULT hr = key.WriteString(L"EndpointId", id.full);
    if (SUCCEEDED(hr)) hr = key.WriteString(L"Strategy", ToString(strategy));
    if (SUCCEEDED(hr)) hr = key.WriteString(L"InstalledAtUtc", UtcTimestamp());

    for (std::size_t i = 0; SUCCEEDED(hr) && i < entries.size(); ++i) {
        const BackupEntry& entry = entries[i];
        reg::Key item;
        hr = reg::Key::Create(HKEY_LOCAL_MACHINE, root + L"\\" + std::to_wstring(i), kWrite, item);
        if (SUCCEEDED(hr)) hr = item.WriteString(L"Kind", entry.kind);
        if (SUCCEEDED(hr)) hr = item.WriteString(L"Name", entry.name);
        if (SUCCEEDED(hr)) hr = item.WriteDword(L"HasOriginal", entry.original ? 1 : 0);
        if (SUCCEEDED(hr) && entry.original) hr = item.WriteRaw(L"Original", *entry.original);
        if (SUCCEEDED(hr)) hr = item.WriteDword(L"HasWritten", entry.written ? 1 : 0);
        if (SUCCEEDED(hr) && entry.written) hr = item.WriteRaw(L"Written", *entry.written);
    }
    return hr;
}

std::optional<std::vector<BackupEntry>> LoadBackup(const EndpointId& id) {
    const std::wstring root = BackupKeyPath(id);
    reg::Key key;
    if (FAILED(reg::Key::Open(HKEY_LOCAL_MACHINE, root, kRead, key))) {
        return std::nullopt;
    }
    std::vector<BackupEntry> entries;
    for (const std::wstring& sub : key.SubKeyNames()) {
        reg::Key item;
        if (FAILED(reg::Key::Open(HKEY_LOCAL_MACHINE, root + L"\\" + sub, kRead, item))) {
            continue;
        }
        BackupEntry entry;
        entry.kind = item.ReadString(L"Kind").value_or(L"");
        entry.name = item.ReadString(L"Name").value_or(L"");
        if (entry.name.empty() || (entry.kind != kKindFx && entry.kind != kKindProperties)) {
            continue;
        }
        if (item.ReadDword(L"HasOriginal").value_or(0) != 0) {
            entry.original = item.ReadRaw(L"Original");
        }
        if (item.ReadDword(L"HasWritten").value_or(0) != 0) {
            entry.written = item.ReadRaw(L"Written");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

HRESULT ApplyEdits(const EndpointId& id, const std::vector<RegistryEdit>& edits) {
    if (edits.empty()) {
        return S_OK;
    }
    reg::Key fx;
    if (const HRESULT hr = reg::Key::CreateForRestore(HKEY_LOCAL_MACHINE, FxPropertiesKeyPath(id), fx);
        FAILED(hr)) {
        return hr;
    }
    for (const RegistryEdit& edit : edits) {
        HRESULT hr = S_OK;
        switch (edit.kind) {
            case RegistryEdit::Kind::SetString:
                hr = fx.WriteString(edit.name, edit.string_value);
                break;
            case RegistryEdit::Kind::SetMultiString:
                hr = fx.WriteMultiString(edit.name, edit.multi_value);
                break;
            case RegistryEdit::Kind::Delete:
                hr = fx.DeleteValue(edit.name);
                break;
        }
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

}  // namespace

HRESULT ApplyInstallPlan(const EndpointId& id, const FxSnapshot& before, const InstallPlan& plan,
                         Strategy strategy, Report& report) {
    if (plan.status == PlanStatus::Refused) {
        return E_ABORT;
    }

    std::vector<BackupEntry> entries;
    for (const RegistryEdit& edit : plan.fx_edits) {
        entries.push_back({kKindFx, edit.name, before.Raw(edit.name), ExpectedValue(edit)});
    }
    if (plan.enable_effects) {
        entries.push_back(
            {kKindProperties, fx::kDisableSysFx, before.disable_sysfx, reg::MakeDword(0)});
    }
    if (entries.empty()) {
        return S_OK;
    }

    // Re-running on an installed endpoint (e.g. only to re-enable effects):
    // keep the original values from the first installation.
    if (plan.status == PlanStatus::AlreadyInstalled) {
        if (auto existing = LoadBackup(id)) {
            for (BackupEntry& entry : entries) {
                const auto it = std::find_if(existing->begin(), existing->end(), [&](const auto& e) {
                    return e.kind == entry.kind && e.name == entry.name;
                });
                if (it != existing->end()) {
                    entry.original = it->original;
                    existing->erase(it);
                }
            }
            entries.insert(entries.end(), existing->begin(), existing->end());
        }
    }

    // Backup first: if anything below fails half-way, uninstall can still
    // restore the original state.
    if (const HRESULT hr = SaveBackup(id, strategy, entries); FAILED(hr)) {
        report.push_back(L"Could not write the backup; nothing was changed: " +
                         win32::FormatHResult(hr));
        return hr;
    }

    if (const HRESULT hr = ApplyEdits(id, plan.fx_edits); FAILED(hr)) {
        report.push_back(L"Writing FxProperties failed: " + win32::FormatHResult(hr));
        return hr;
    }
    if (plan.enable_effects) {
        reg::Key properties;
        HRESULT hr =
            reg::Key::CreateForRestore(HKEY_LOCAL_MACHINE, PropertiesKeyPath(id), properties);
        if (SUCCEEDED(hr)) {
            hr = properties.WriteDword(fx::kDisableSysFx, 0);
        }
        if (FAILED(hr)) {
            report.push_back(L"Could not enable audio enhancements: " + win32::FormatHResult(hr));
            return hr;
        }
    }
    return S_OK;
}

HRESULT RestoreEndpoint(const EndpointId& id, Report& report) {
    HRESULT result = S_OK;

    if (auto backup = LoadBackup(id)) {
        for (const BackupEntry& entry : *backup) {
            const std::wstring path = KeyPathFor(id, entry.kind);
            reg::Key reader;
            std::optional<reg::RawValue> current;
            if (SUCCEEDED(reg::Key::Open(HKEY_LOCAL_MACHINE, path, kRead, reader))) {
                current = reader.ReadRaw(entry.name);
            }
            const bool unchanged = entry.written ? (current && *current == *entry.written)
                                                 : !current.has_value();
            if (!unchanged) {
                report.push_back(L"Left " + entry.name +
                                 L" untouched: it changed after installation (driver update?).");
                continue;
            }
            reg::Key writer;
            HRESULT hr = reg::Key::CreateForRestore(HKEY_LOCAL_MACHINE, path, writer);
            if (SUCCEEDED(hr)) {
                hr = entry.original ? writer.WriteRaw(entry.name, *entry.original)
                                    : writer.DeleteValue(entry.name);
            }
            if (FAILED(hr)) {
                report.push_back(L"Could not restore " + entry.name + L": " +
                                 win32::FormatHResult(hr));
                result = hr;
            }
        }
        if (SUCCEEDED(result)) {
            result = reg::DeleteTree(HKEY_LOCAL_MACHINE, BackupKeyPath(id));
        }
    }

    // Whatever the backup said, make sure no Somike CLSID is left behind.
    const std::vector<RegistryEdit> leftovers = PlanRemoval(ReadFxSnapshot(id));
    if (!leftovers.empty()) {
        if (const HRESULT hr = ApplyEdits(id, leftovers); FAILED(hr)) {
            report.push_back(L"Could not remove Somike from FxProperties: " +
                             win32::FormatHResult(hr));
            return hr;
        }
    }
    return result;
}

bool HasBackup(const EndpointId& id) {
    reg::Key key;
    return SUCCEEDED(reg::Key::Open(HKEY_LOCAL_MACHINE, BackupKeyPath(id), kRead, key));
}

std::vector<EndpointId> EndpointsWithBackup() {
    std::vector<EndpointId> result;
    reg::Key root;
    if (FAILED(reg::Key::Open(HKEY_LOCAL_MACHINE, kBackupRootKeyPath, kRead, root))) {
        return result;
    }
    for (const std::wstring& sub : root.SubKeyNames()) {
        reg::Key key;
        if (FAILED(reg::Key::Open(HKEY_LOCAL_MACHINE, std::wstring(kBackupRootKeyPath) + L"\\" + sub,
                                  kRead, key))) {
            continue;
        }
        if (auto full = key.ReadString(L"EndpointId")) {
            if (auto id = ParseEndpointId(*full)) {
                result.push_back(std::move(*id));
            }
        }
    }
    return result;
}

}  // namespace somike::installer
