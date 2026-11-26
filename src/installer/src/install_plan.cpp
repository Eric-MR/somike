#include "somike/installer/install_plan.h"

#include <algorithm>

#include "somike/common/strings.h"
#include "somike/protocol/apo_identity.h"

namespace somike::installer {

namespace {

const std::wstring kSomike = protocol::kApoClsidString;

std::vector<std::wstring> DefaultModes() {
    return {fx::kModeDefault, fx::kModeCommunications};
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

InstallPlan PlanInstall(const FxSnapshot& s, Strategy strategy) {
    InstallPlan plan;

    if (s.EffectsDisabled()) {
        plan.enable_effects = true;
        plan.notes.emplace_back(
            L"\"Audio enhancements\" are disabled for this microphone; they will be enabled "
            L"because Windows does not run any effect (including Somike) otherwise.");
    }

    if (QueryInstallState(s) != InstallState::NotInstalled) {
        plan.status = PlanStatus::AlreadyInstalled;
        plan.notes.emplace_back(L"Somike is already installed on this microphone.");
        return plan;
    }

    const bool has_stream = s.Has(fx::kStreamEffect);
    const bool has_composite = s.Has(fx::kCompositeStreamEffect);
    const bool legacy_only = HasLegacyEffects(s) && !HasModernEffects(s);
    const bool has_modes = s.Has(fx::kStreamProcessingModes);

    switch (strategy) {
        case Strategy::Auto: {
            if (has_composite) {
                auto chain = s.MultiString(fx::kCompositeStreamEffect);
                chain.push_back(kSomike);
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kCompositeStreamEffect, std::move(chain)));
                plan.notes.emplace_back(
                    L"Appending Somike to the existing composite stream-effect chain.");
                break;
            }
            if (has_stream) {
                plan.status = PlanStatus::Refused;
                plan.notes.push_back(
                    L"This microphone already has a driver stream effect (" +
                    Describe(s, fx::kStreamEffect) +
                    L"). Choose --strategy composite to keep it (experimental) or "
                    L"--strategy replace to disable it while Somike is installed.");
                return plan;
            }
            if (legacy_only) {
                plan.status = PlanStatus::Refused;
                plan.notes.emplace_back(
                    L"This microphone uses legacy (LFX/GFX) driver effects. Adding a stream "
                    L"effect makes Windows ignore them. Use --strategy replace to accept that.");
                return plan;
            }
            plan.fx_edits.push_back(RegistryEdit::SetString(fx::kStreamEffect, kSomike));
            if (!has_modes) {
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kStreamProcessingModes, DefaultModes()));
            }
            plan.notes.emplace_back(L"Installing Somike as the stream effect (no existing one).");
            break;
        }

        case Strategy::Composite: {
            std::vector<std::wstring> chain;
            if (has_composite) {
                chain = s.MultiString(fx::kCompositeStreamEffect);
            } else if (auto existing = s.String(fx::kStreamEffect)) {
                chain.push_back(*existing);
            }
            chain.push_back(kSomike);
            plan.fx_edits.push_back(
                RegistryEdit::SetMultiString(fx::kCompositeStreamEffect, std::move(chain)));
            if (!has_stream && !has_composite && !has_modes) {
                plan.fx_edits.push_back(
                    RegistryEdit::SetMultiString(fx::kStreamProcessingModes, DefaultModes()));
            }
            plan.notes.emplace_back(
                L"Experimental: Microsoft does not document how the single and composite "
                L"stream-effect keys interact. Verify with 'somike-apoctl status' that the "
                L"injector reports activity.");
            if (legacy_only) {
                plan.notes.emplace_back(
                    L"Warning: legacy LFX/GFX effects will be ignored by Windows from now on.");
            }
            break;
        }

        case Strategy::Replace: {
            if (has_stream) {
                plan.notes.push_back(L"Replacing driver stream effect " +
                                     Describe(s, fx::kStreamEffect) +
                                     L" (backed up; restored on uninstall).");
            }
            plan.fx_edits.push_back(RegistryEdit::SetString(fx::kStreamEffect, kSomike));
            if (has_composite) {
                plan.fx_edits.push_back(RegistryEdit::Delete(fx::kCompositeStreamEffect));
            }
            plan.fx_edits.push_back(
                RegistryEdit::SetMultiString(fx::kStreamProcessingModes, DefaultModes()));
            if (legacy_only) {
                plan.notes.emplace_back(
                    L"Legacy LFX/GFX effects will be ignored while Somike is installed.");
            }
            break;
        }
    }

    plan.status = PlanStatus::Ready;
    return plan;
}

std::vector<RegistryEdit> PlanRemoval(const FxSnapshot& s) {
    std::vector<RegistryEdit> edits;
    if (auto stream = s.String(fx::kStreamEffect); stream && IsSomikeClsid(*stream)) {
        edits.push_back(RegistryEdit::Delete(fx::kStreamEffect));
    }
    auto chain = s.MultiString(fx::kCompositeStreamEffect);
    const auto removed = std::erase_if(chain, [](const std::wstring& c) { return IsSomikeClsid(c); });
    if (removed != 0) {
        edits.push_back(chain.empty()
                            ? RegistryEdit::Delete(fx::kCompositeStreamEffect)
                            : RegistryEdit::SetMultiString(fx::kCompositeStreamEffect, chain));
    }
    return edits;
}

}  // namespace somike::installer
