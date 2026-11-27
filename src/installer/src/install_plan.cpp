#include "somike/installer/install_plan.h"

#include <algorithm>

#include "somike/common/strings.h"
#include "somike/protocol/apo_identity.h"

namespace somike::installer {

namespace {

const std::wstring kSomike = protocol::kApoClsidString;

// PKEY_EFX_ProcessingModes_Supported_For_Streaming accepts only DEFAULT.
std::vector<std::wstring> EndpointModes() {
    return {fx::kModeDefault};
}

// Removes Somike from one single-CLSID slot and its composite chain.
void AppendRemoval(const FxSnapshot& s, const wchar_t* slot, const wchar_t* chain_name,
                   std::vector<RegistryEdit>& edits) {
    if (auto value = s.String(slot); value && IsSomikeClsid(*value)) {
        edits.push_back(RegistryEdit::Delete(slot));
    }
    auto chain = s.MultiString(chain_name);
    const auto removed = std::erase_if(chain, [](const std::wstring& c) { return IsSomikeClsid(c); });
    if (removed != 0) {
        edits.push_back(chain.empty() ? RegistryEdit::Delete(chain_name)
                                      : RegistryEdit::SetMultiString(chain_name, chain));
    }
}

bool HasModernEffects(const FxSnapshot& s) {
    return s.Has(fx::kStreamEffect) || s.Has(fx::kModeEffect) || s.Has(fx::kEndpointEffect) ||
           s.Has(fx::kCompositeStreamEffect) || s.Has(fx::kCompositeModeEffect) ||
           s.Has(fx::kCompositeEndpointEffect);
}

bool HasLegacyEffects(const FxSnapshot& s) {
    return s.Has(fx::kPreMixEffect) || s.Has(fx::kPostMixEffect);
}

std::wstring Describe(const FxSnapshot& s, const std::wstring& name) {
    if (auto value = s.String(name)) {
        return *value;
    }
    return L"(non-string value)";
}

}  // namespace

const wchar_t* ToString(Strategy strategy) noexcept {
    switch (strategy) {
        case Strategy::Auto:
            return L"auto";
        case Strategy::Composite:
            return L"composite";
        case Strategy::Replace:
            return L"replace";
    }
    return L"auto";
}

std::optional<Strategy> ParseStrategy(std::wstring_view text) noexcept {
    for (Strategy s : {Strategy::Auto, Strategy::Composite, Strategy::Replace}) {
        if (text::EqualsIgnoreCase(text, ToString(s))) {
            return s;
        }
    }
    return std::nullopt;
}

RegistryEdit RegistryEdit::SetString(std::wstring name, std::wstring value) {
    RegistryEdit edit;
    edit.kind = Kind::SetString;
    edit.name = std::move(name);
    edit.string_value = std::move(value);
    return edit;
}

RegistryEdit RegistryEdit::SetMultiString(std::wstring name, std::vector<std::wstring> value) {
    RegistryEdit edit;
    edit.kind = Kind::SetMultiString;
    edit.name = std::move(name);
    edit.multi_value = std::move(value);
    return edit;
}

RegistryEdit RegistryEdit::Delete(std::wstring name) {
    RegistryEdit edit;
    edit.kind = Kind::Delete;
    edit.name = std::move(name);
    return edit;
}

std::optional<reg::RawValue> ExpectedValue(const RegistryEdit& edit) {
    switch (edit.kind) {
        case RegistryEdit::Kind::SetString:
            return reg::MakeString(edit.string_value);
        case RegistryEdit::Kind::SetMultiString:
            return reg::MakeMultiString(edit.multi_value);
        case RegistryEdit::Kind::Delete:
            return std::nullopt;
    }
    return std::nullopt;
}

void ApplyToSnapshot(FxSnapshot& snapshot, const std::vector<RegistryEdit>& edits) {
    for (const RegistryEdit& edit : edits) {
        if (auto value = ExpectedValue(edit)) {
            snapshot.Set(edit.name, std::move(*value));
        } else {
            snapshot.Erase(edit.name);
        }
        snapshot.fx_key_exists = true;
    }
}

InstallPlan PlanInstall(const FxSnapshot& current, Strategy strategy) {
    InstallPlan plan;

    if (current.EffectsDisabled()) {
        plan.enable_effects = true;
        plan.notes.emplace_back(
            L"\"Audio enhancements\" are disabled for this microphone; they will be enabled "
            L"because Windows does not run any effect (including Somike) otherwise.");
    }

    const InstallState state = QueryInstallState(current);
    if (state != InstallState::NotInstalled && !NeedsUpgrade(state)) {
        plan.status = PlanStatus::AlreadyInstalled;
        plan.notes.emplace_back(L"Somike is already installed on this microphone.");
        return plan;
    }

    // An upgrade first takes Somike out of the 0.1.0 stream-effect slot; the
    // endpoint effect is then planned against the endpoint as it is without it.
    const bool upgrade = NeedsUpgrade(state);
    FxSnapshot s = current;
    if (upgrade) {
        AppendRemoval(current, fx::kStreamEffect, fx::kCompositeStreamEffect, plan.fx_edits);
        ApplyToSnapshot(s, plan.fx_edits);
        plan.notes.emplace_back(
            L"Somike is installed as a stream effect, which apps recording in raw mode (such as "
            L"Discord) bypass. Moving it to the endpoint effect, which every stream passes.");
        if (QueryInstallState(s) != InstallState::NotInstalled) {
            plan.status = PlanStatus::Upgrade;  // already an endpoint effect as well
            return plan;
        }
    }
    auto refuse = [&plan](std::wstring note) {
        plan.status = PlanStatus::Refused;
        plan.fx_edits.clear();
        plan.notes.push_back(std::move(note));
        return plan;
    };

    const bool has_endpoint = s.Has(fx::kEndpointEffect);
    const bool has_composite = s.Has(fx::kCompositeEndpointEffect);
    const bool legacy_only = HasLegacyEffects(s) && !HasModernEffects(s);
    const bool has_modes = s.Has(fx::kEndpointProcessingModes);

    switch (strategy) {
        case Strategy::Auto: {
            if (has_composite) {
                auto chain = s.MultiString(fx::kCompositeEndpointEffect);
                chain.push_back(kSomike);
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kCompositeEndpointEffect, std::move(chain)));
                plan.notes.emplace_back(
                    L"Appending Somike to the existing composite endpoint-effect chain.");
                break;
            }
            if (has_endpoint) {
                return refuse(L"This microphone already has a driver endpoint effect (" +
                              Describe(s, fx::kEndpointEffect) +
                              L"). Choose --strategy composite to keep it (experimental) or "
                              L"--strategy replace to disable it while Somike is installed.");
            }
            if (legacy_only) {
                return refuse(
                    L"This microphone uses legacy (LFX/GFX) driver effects. Adding an endpoint "
                    L"effect makes Windows ignore them. Use --strategy replace to accept that.");
            }
            plan.fx_edits.push_back(RegistryEdit::SetString(fx::kEndpointEffect, kSomike));
            if (!has_modes) {
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kEndpointProcessingModes, EndpointModes()));
            }
            plan.notes.emplace_back(L"Installing Somike as the endpoint effect (no existing one).");
            break;
        }

        case Strategy::Composite: {
            std::vector<std::wstring> chain;
            if (has_composite) {
                chain = s.MultiString(fx::kCompositeEndpointEffect);
            } else if (auto existing = s.String(fx::kEndpointEffect)) {
                chain.push_back(*existing);
            }
            chain.push_back(kSomike);
            plan.fx_edits.push_back(
                RegistryEdit::SetMultiString(fx::kCompositeEndpointEffect, std::move(chain)));
            if (!has_endpoint && !has_composite && !has_modes) {
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kEndpointProcessingModes, EndpointModes()));
            }
            plan.notes.emplace_back(
                L"Experimental: Microsoft does not document how the single and composite "
                L"endpoint-effect keys interact. Verify with 'somike-apoctl status' that the "
                L"injector reports activity.");
            if (legacy_only) {
                plan.notes.emplace_back(
                    L"Warning: legacy LFX/GFX effects will be ignored by Windows from now on.");
            }
            break;
        }

        case Strategy::Replace: {
            if (has_endpoint) {
                plan.notes.push_back(L"Replacing driver endpoint effect " +
                                     Describe(s, fx::kEndpointEffect) +
                                     L" (backed up; restored on uninstall).");
            }
            plan.fx_edits.push_back(RegistryEdit::SetString(fx::kEndpointEffect, kSomike));
            if (has_composite) {
                plan.fx_edits.push_back(RegistryEdit::Delete(fx::kCompositeEndpointEffect));
            }
            plan.fx_edits.push_back(
                RegistryEdit::SetMultiString(fx::kEndpointProcessingModes, EndpointModes()));
            if (legacy_only) {
                plan.notes.emplace_back(
                    L"Legacy LFX/GFX effects will be ignored while Somike is installed.");
            }
            break;
        }
    }

    plan.status = upgrade ? PlanStatus::Upgrade : PlanStatus::Ready;
    return plan;
}

std::vector<RegistryEdit> PlanRemoval(const FxSnapshot& s) {
    std::vector<RegistryEdit> edits;
    AppendRemoval(s, fx::kStreamEffect, fx::kCompositeStreamEffect, edits);
    AppendRemoval(s, fx::kEndpointEffect, fx::kCompositeEndpointEffect, edits);
    return edits;
}

}  // namespace somike::installer
