#include "app_state.h"

#include <cmath>

#include <nlohmann/json.hpp>

namespace somike::app {

using nlohmann::json;

namespace {

const char* ToString(LivePhase phase) {
    switch (phase) {
        case LivePhase::Live:
            return "live";
        case LivePhase::Waiting:
            return "waiting";
        case LivePhase::Error:
            return "error";
        case LivePhase::Off:
            break;
    }
    return "off";
}

// Rounds for a stable serialization (identical states serialize identically,
// so the host can skip unchanged pushes).
double Round(float value, double step) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::round(static_cast<double>(value) / step) * step;
}

// Invalid UTF-8 (e.g. odd app names) must not throw.
std::string Dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace

std::string SerializeStateMessage(const UiState& s) {
    json sources = json::array();
    for (const UiSource& source : s.sources) {
        sources.push_back({{"id", source.id},
                           {"name", source.name},
                           {"iconKey", source.icon_key},
                           {"running", source.running},
                           {"active", source.active},
                           {"system", source.system}});
    }
    json mics = json::array();
    for (const UiMic& mic : s.mics) {
        mics.push_back({{"id", mic.id}, {"name", mic.name}, {"isDefault", mic.is_default}});
    }
    const json state = {
        {"view", s.setup_view ? "setup" : "main"},
        {"setup", {{"busy", s.setup_busy}, {"message", s.setup_message}, {"error", s.setup_error}}},
        {"mics", std::move(mics)},
        {"micId", s.mic_id},
        {"micMutedInWindows", s.mic_muted_in_windows},
        {"injector",
         {{"installed", s.installed},
          {"enhancementsOff", s.enhancements_off},
          {"needsUpdate", s.needs_update},
          {"micInUse", s.mic_in_use}}},
        {"sources", std::move(sources)},
        {"sourceId", s.source_id},
        {"live", {{"on", s.live_on}, {"phase", ToString(s.live_phase)}, {"message", s.live_message}}},
        {"level", Round(s.level, 0.01)},
        {"gain", Round(s.gain, 0.01)},
        {"muteMic", s.mute_mic},
    };
    return Dump({{"type", "state"}, {"state", state}});
}

std::string SerializeIconsMessage(const std::map<std::string, std::string>& icons) {
    return Dump({{"type", "icons"}, {"icons", icons}});
}

std::string SerializeToastMessage(std::string_view text, bool error) {
    return Dump({{"type", "toast"}, {"text", std::string(text)}, {"tone", error ? "err" : "info"}});
}

std::optional<UiCommand> ParseCommand(std::string_view text) {
    const json value = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!value.is_object()) {
        return std::nullopt;
    }
    const auto cmd = value.find("cmd");
    if (cmd == value.end() || !cmd->is_string()) {
        return std::nullopt;
    }
    UiCommand command;
    command.cmd = cmd->get<std::string>();
    if (const auto id = value.find("id"); id != value.end() && id->is_string()) {
        command.id = id->get<std::string>();
    }
    if (const auto v = value.find("value"); v != value.end()) {
        if (v->is_number()) {
            const double number = v->get<double>();
            if (std::isfinite(number)) {
                command.number = number;
            }
        } else if (v->is_boolean()) {
            command.flag = v->get<bool>();
        }
    }
    return command;
}

}  // namespace somike::app
