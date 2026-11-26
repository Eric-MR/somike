#include "somike/core/injector_status.h"

#include <windows.h>

#include "somike/installer/endpoint_id.h"

namespace somike::core {

InjectorStatus QueryInjectorStatus(const std::wstring& endpoint_id,
                                   const std::optional<dsp::ConsumerSnapshot>& consumer) {
    InjectorStatus status;

    if (const auto id = installer::ParseEndpointId(endpoint_id)) {
        status.endpoint_valid = true;
        const installer::FxSnapshot snapshot = installer::ReadFxSnapshot(*id);
        status.install_state = installer::QueryInstallState(snapshot);
        status.effects_disabled = snapshot.EffectsDisabled();
    }

    if (consumer) {
        status.ring_available = true;
        status.consumer = *consumer;
        if (consumer->heartbeat_ms != 0) {
            const std::uint64_t now = ::GetTickCount64();
            status.heartbeat_age_ms = now > consumer->heartbeat_ms ? now - consumer->heartbeat_ms : 0;
            status.apo_alive = status.heartbeat_age_ms < kApoAliveThresholdMs;
        }
    }
    return status;
}

}  // namespace somike::core
