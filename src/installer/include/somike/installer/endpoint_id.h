#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace somike::installer {

enum class Flow { Render, Capture };

// An MMDevice endpoint id such as "{0.0.1.00000000}.{a1b2c3d4-...}".
struct EndpointId {
    Flow flow = Flow::Capture;
    std::wstring guid;  // "{...}", lower case: the endpoint's registry key name
    std::wstring full;  // the complete id as returned by IMMDevice::GetId()
};

[[nodiscard]] std::optional<EndpointId> ParseEndpointId(std::wstring_view id);

// Registry paths, all relative to HKEY_LOCAL_MACHINE.
[[nodiscard]] std::wstring EndpointKeyPath(const EndpointId& id);
[[nodiscard]] std::wstring FxPropertiesKeyPath(const EndpointId& id);
[[nodiscard]] std::wstring PropertiesKeyPath(const EndpointId& id);
[[nodiscard]] std::wstring BackupKeyPath(const EndpointId& id);

inline constexpr wchar_t kBackupRootKeyPath[] = L"SOFTWARE\\Somike\\Endpoints";

}  // namespace somike::installer
