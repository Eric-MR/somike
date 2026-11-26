#pragma once

#include <optional>
#include <string>
#include <vector>

namespace somike::core {

struct AudioEndpoint {
    std::wstring id;    // IMMDevice::GetId(), e.g. "{0.0.1.00000000}.{guid}"
    std::wstring name;  // friendly name, e.g. "Microphone (USB Audio Device)"
    bool is_default = false;                 // default capture device (console role)
    bool is_default_communications = false;  // default communications capture device
};

// Active capture endpoints (microphones). The calling thread must have COM
// initialised. Returns an empty list on failure.
[[nodiscard]] std::vector<AudioEndpoint> EnumerateCaptureEndpoints();

// Id of the default capture endpoint for the console role.
[[nodiscard]] std::optional<std::wstring> DefaultCaptureEndpointId();

}  // namespace somike::core
