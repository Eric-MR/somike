#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "somike/dsp/ring_writer.h"
#include "somike/installer/fx_config.h"

namespace somike::core {

// Everything the UI needs to tell the user whether injection can work.
struct InjectorStatus {
    bool endpoint_valid = false;
    installer::InstallState install_state = installer::InstallState::NotInstalled;
    bool effects_disabled = false;  // "Audio enhancements" switched off for this mic
    bool ring_available = false;    // shared ring exists (injector installed at least once)
    bool apo_alive = false;         // an APO instance reported in recently
    std::uint64_t heartbeat_age_ms = 0;
    dsp::ConsumerSnapshot consumer{};
};

// A consumer heartbeat younger than this means "the APO is loaded and
// processing a microphone stream right now".
inline constexpr std::uint64_t kApoAliveThresholdMs = 2500;

[[nodiscard]] InjectorStatus QueryInjectorStatus(
    const std::wstring& endpoint_id, const std::optional<dsp::ConsumerSnapshot>& consumer);

}  // namespace somike::core
