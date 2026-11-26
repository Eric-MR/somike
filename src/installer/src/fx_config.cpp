#include "somike/installer/fx_config.h"

#include <algorithm>
#include <cstring>

#include "somike/common/strings.h"
#include "somike/protocol/apo_identity.h"

namespace somike::installer {

std::optional<reg::RawValue> FxSnapshot::Raw(const std::wstring& name) const {
    const auto it = fx_values.find(name);
    if (it == fx_values.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::wstring> FxSnapshot::String(const std::wstring& name) const {
    const auto it = fx_values.find(name);
    if (it == fx_values.end() ||
        (it->second.type != REG_SZ && it->second.type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(it->second.data.size() / sizeof(wchar_t), L'\0');
    if (!value.empty()) {
        std::memcpy(value.data(), it->second.data.data(), value.size() * sizeof(wchar_t));
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::vector<std::wstring> FxSnapshot::MultiString(const std::wstring& name) const {
    const auto it = fx_values.find(name);
    if (it == fx_values.end() || it->second.type != REG_MULTI_SZ) {
        return {};
    }
    return reg::ParseMultiString(it->second.data);
}

bool FxSnapshot::EffectsDisabled() const {
    if (!disable_sysfx || disable_sysfx->type != REG_DWORD ||
        disable_sysfx->data.size() != sizeof(DWORD)) {
        return false;
    }
    DWORD value = 0;
    std::memcpy(&value, disable_sysfx->data.data(), sizeof(value));
    return value != 0;
}

FxSnapshot ReadFxSnapshot(const EndpointId& id) {
    FxSnapshot snapshot;

    reg::Key fx;
    if (SUCCEEDED(reg::Key::Open(HKEY_LOCAL_MACHINE, FxPropertiesKeyPath(id),
                                 KEY_READ | KEY_WOW64_64KEY, fx))) {
        snapshot.fx_key_exists = true;
        for (const wchar_t* name : fx::kTrackedValues) {
            if (auto value = fx.ReadRaw(name)) {
                snapshot.fx_values.emplace(name, std::move(*value));
            }
        }
    }

    reg::Key properties;
    if (SUCCEEDED(reg::Key::Open(HKEY_LOCAL_MACHINE, PropertiesKeyPath(id),
                                 KEY_READ | KEY_WOW64_64KEY, properties))) {
        snapshot.disable_sysfx = properties.ReadRaw(fx::kDisableSysFx);
    }
    return snapshot;
}

bool IsSomikeClsid(std::wstring_view clsid) noexcept {
    return text::EqualsIgnoreCase(text::Trim(clsid), protocol::kApoClsidString);
}

InstallState QueryInstallState(const FxSnapshot& snapshot) {
    if (const auto stream = snapshot.String(fx::kStreamEffect); stream && IsSomikeClsid(*stream)) {
        return InstallState::InstalledStreamEffect;
    }
    const auto composite = snapshot.MultiString(fx::kCompositeStreamEffect);
    if (std::any_of(composite.begin(), composite.end(),
                    [](const std::wstring& clsid) { return IsSomikeClsid(clsid); })) {
        return InstallState::InstalledComposite;
    }
    return InstallState::NotInstalled;
}

const wchar_t* ToString(InstallState state) noexcept {
    switch (state) {
        case InstallState::NotInstalled:
            return L"not installed";
        case InstallState::InstalledStreamEffect:
            return L"installed (stream effect)";
        case InstallState::InstalledComposite:
            return L"installed (composite stream effect)";
    }
    return L"unknown";
}

}  // namespace somike::installer
