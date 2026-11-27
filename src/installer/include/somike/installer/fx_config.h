// What an endpoint's effect configuration looks like in the registry.
//
// Value names are PROPERTYKEYs rendered as "{fmtid},pid". GUIDs and PIDs are
// taken verbatim from the Windows SDK (audioenginebaseapo.h, mmdeviceapi.h,
// ksmedia.h, 10.0.26100.0).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "somike/common/registry.h"
#include "somike/installer/endpoint_id.h"

namespace somike::installer {

namespace fx {
// FxProperties key
inline constexpr wchar_t kPreMixEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";   // legacy LFX
inline constexpr wchar_t kPostMixEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2";  // legacy GFX
inline constexpr wchar_t kStreamEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";   // SFX
inline constexpr wchar_t kModeEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6";     // MFX
inline constexpr wchar_t kEndpointEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7"; // EFX
inline constexpr wchar_t kCompositeStreamEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
inline constexpr wchar_t kCompositeModeEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14";
inline constexpr wchar_t kCompositeEndpointEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
inline constexpr wchar_t kStreamProcessingModes[] = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5";
// PKEY_EFX_ProcessingModes_Supported_For_Streaming (only DEFAULT is allowed).
inline constexpr wchar_t kEndpointProcessingModes[] = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";

// PKEY_AudioEndpoint_Disable_SysFx ("Audio enhancements" off). Stored in
// FxProperties; Windows sets it itself after an effect fails or hangs.
inline constexpr wchar_t kDisableSysFx[] = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

// Signal processing modes
inline constexpr wchar_t kModeDefault[] = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
inline constexpr wchar_t kModeCommunications[] = L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}";

inline constexpr const wchar_t* kTrackedValues[] = {
    kPreMixEffect,        kPostMixEffect,           kStreamEffect,
    kModeEffect,          kEndpointEffect,          kCompositeStreamEffect,
    kCompositeModeEffect, kCompositeEndpointEffect, kStreamProcessingModes,
    kEndpointProcessingModes,
};
}  // namespace fx

// The installation-relevant registry state of one endpoint.
struct FxSnapshot {
    bool fx_key_exists = false;
    std::map<std::wstring, reg::RawValue> fx_values;  // keyed by fx::k* constants
    std::optional<reg::RawValue> disable_sysfx;

    [[nodiscard]] bool Has(const std::wstring& name) const { return fx_values.contains(name); }
    [[nodiscard]] std::optional<reg::RawValue> Raw(const std::wstring& name) const;
    [[nodiscard]] std::optional<std::wstring> String(const std::wstring& name) const;
    [[nodiscard]] std::vector<std::wstring> MultiString(const std::wstring& name) const;
    [[nodiscard]] bool EffectsDisabled() const;

    // Mutators used by tests and by plan simulation.
    void Set(const std::wstring& name, reg::RawValue value) { fx_values[name] = std::move(value); }
    void Erase(const std::wstring& name) { fx_values.erase(name); }
};

// Reads the current state. Needs only read access (works unelevated).
[[nodiscard]] FxSnapshot ReadFxSnapshot(const EndpointId& id);

// Somike installs as the endpoint effect (EFX): on a capture endpoint it runs
// before the split into processing modes, so it also reaches streams opened
// in raw mode (Discord's voice engine, for example). Version 0.1.0 used the
// stream-effect slot (SFX), which raw streams skip; those states still need
// recognising so they can be upgraded and uninstalled.
enum class InstallState {
    NotInstalled,
    InstalledEndpointEffect,     // our CLSID in PKEY_FX_EndpointEffectClsid
    InstalledCompositeEndpoint,  // our CLSID in PKEY_CompositeFX_EndpointEffectClsid
    InstalledStreamEffect,       // 0.1.0 layout: PKEY_FX_StreamEffectClsid
    InstalledComposite,          // 0.1.0 layout: PKEY_CompositeFX_StreamEffectClsid
};

// Reports the 0.1.0 (stream-effect) layout first: if it is present at all,
// the endpoint needs an upgrade, even when an endpoint effect exists too.
[[nodiscard]] InstallState QueryInstallState(const FxSnapshot& snapshot);
[[nodiscard]] const wchar_t* ToString(InstallState state) noexcept;
// True for the stream-effect layout, which apps recording in raw mode bypass.
[[nodiscard]] bool NeedsUpgrade(InstallState state) noexcept;
[[nodiscard]] bool IsSomikeClsid(std::wstring_view clsid) noexcept;

}  // namespace somike::installer
