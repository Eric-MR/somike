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
        // The "Audio enhancements" switch lives in FxProperties; this is also
        // where the audio service sets it when it auto-disables a misbehaving
        // effect.
        snapshot.disable_sysfx = fx.ReadRaw(fx::kDisableSysFx);
    }
    return snapshot;
}

bool IsSomikeClsid(std::wstring_view clsid) noexcept {
    return text::EqualsIgnoreCase(text::Trim(clsid), protocol::kApoClsidString);
}

namespace {

bool SlotHasSomike(const FxSnapshot& snapshot, const wchar_t* name) {
    const auto value = snapshot.String(name);
    return value && IsSomikeClsid(*value);
}

bool ChainHasSomike(const FxSnapshot& snapshot, const wchar_t* name) {
    const auto chain = snapshot.MultiString(name);
    return std::any_of(chain.begin(), chain.end(),
                       [](const std::wstring& clsid) { return IsSomikeClsid(clsid); });
}

}  // namespace

InstallState QueryInstallState(const FxSnapshot& snapshot) {
    if (SlotHasSomike(snapshot, fx::kStreamEffect)) return InstallState::InstalledStreamEffect;
    if (ChainHasSomike(snapshot, fx::kCompositeStreamEffect)) return InstallState::InstalledComposite;
    if (SlotHasSomike(snapshot, fx::kEndpointEffect)) return InstallState::InstalledEndpointEffect;
    if (ChainHasSomike(snapshot, fx::kCompositeEndpointEffect)) {
        return InstallState::InstalledCompositeEndpoint;
    }
    return InstallState::NotInstalled;
}

const wchar_t* ToString(InstallState state) noexcept {
    switch (state) {
        case InstallState::NotInstalled:
            return L"not installed";
        case InstallState::InstalledEndpointEffect:
            return L"installed (endpoint effect)";
        case InstallState::InstalledCompositeEndpoint:
            return L"installed (composite endpoint effect)";
        case InstallState::InstalledStreamEffect:
            return L"installed as stream effect (outdated: raw-mode apps such as Discord bypass it; run install to upgrade)";
        case InstallState::InstalledComposite:
            return L"installed as composite stream effect (outdated: run install to upgrade)";
    }
    return L"unknown";
}

bool NeedsUpgrade(InstallState state) noexcept {
    return state == InstallState::InstalledStreamEffect || state == InstallState::InstalledComposite;
}

}  // namespace somike::installer
