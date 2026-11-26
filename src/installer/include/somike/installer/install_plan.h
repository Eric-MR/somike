// Pure decision logic: given an endpoint's current effect configuration,
// what exactly do we change? Kept free of I/O so it is fully unit-tested.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "somike/common/registry.h"
#include "somike/installer/fx_config.h"

namespace somike::installer {

enum class Strategy {
    // Install only where it cannot disable existing effects; refuse otherwise.
    Auto,
    // Keep existing stream effects and append Somike to the composite list
    // (PKEY_CompositeFX_StreamEffectClsid, Windows 10 1803+). Experimental.
    Composite,
    // Put Somike in the stream-effect slot, replacing (and backing up) any
    // existing driver stream effect.
    Replace,
};

[[nodiscard]] const wchar_t* ToString(Strategy strategy) noexcept;
[[nodiscard]] std::optional<Strategy> ParseStrategy(std::wstring_view text) noexcept;

struct RegistryEdit {
    enum class Kind { SetString, SetMultiString, Delete };

    Kind kind = Kind::Delete;
    std::wstring name;
    std::wstring string_value;
    std::vector<std::wstring> multi_value;

    [[nodiscard]] static RegistryEdit SetString(std::wstring name, std::wstring value);
    [[nodiscard]] static RegistryEdit SetMultiString(std::wstring name,
                                                     std::vector<std::wstring> value);
    [[nodiscard]] static RegistryEdit Delete(std::wstring name);
};

// The registry value an edit produces (nullopt for Delete).
[[nodiscard]] std::optional<reg::RawValue> ExpectedValue(const RegistryEdit& edit);

// Applies edits to an in-memory snapshot (used to verify plans in tests).
void ApplyToSnapshot(FxSnapshot& snapshot, const std::vector<RegistryEdit>& edits);

enum class PlanStatus { Ready, AlreadyInstalled, Refused };

struct InstallPlan {
    PlanStatus status = PlanStatus::Refused;
    std::vector<RegistryEdit> fx_edits;  // edits to the FxProperties key
    bool enable_effects = false;         // set PKEY_AudioEndpoint_Disable_SysFx = 0
    std::vector<std::wstring> notes;     // human-readable explanation, shown to the user
};

[[nodiscard]] InstallPlan PlanInstall(const FxSnapshot& snapshot, Strategy strategy);

// Edits that remove Somike from an endpoint without a backup (fallback).
[[nodiscard]] std::vector<RegistryEdit> PlanRemoval(const FxSnapshot& snapshot);

}  // namespace somike::installer
