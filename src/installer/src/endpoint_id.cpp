#include "somike/installer/endpoint_id.h"

#include <algorithm>
#include <cwctype>

#include "somike/common/guid.h"

namespace somike::installer {

namespace {

constexpr std::wstring_view kRenderPrefix = L"{0.0.0.00000000}.";
constexpr std::wstring_view kCapturePrefix = L"{0.0.1.00000000}.";
constexpr wchar_t kMmDevicesRoot[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\";

}  // namespace

std::optional<EndpointId> ParseEndpointId(std::wstring_view id) {
    EndpointId result;
    std::wstring_view rest;
    if (id.starts_with(kCapturePrefix)) {
        result.flow = Flow::Capture;
        rest = id.substr(kCapturePrefix.size());
    } else if (id.starts_with(kRenderPrefix)) {
        result.flow = Flow::Render;
        rest = id.substr(kRenderPrefix.size());
    } else {
        return std::nullopt;
    }
    if (!guid::Parse(rest)) {
        return std::nullopt;
    }
    result.guid.assign(rest);
    std::transform(result.guid.begin(), result.guid.end(), result.guid.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    result.full.assign(id);
    return result;
}

std::wstring EndpointKeyPath(const EndpointId& id) {
    return std::wstring(kMmDevicesRoot) + (id.flow == Flow::Capture ? L"Capture\\" : L"Render\\") +
           id.guid;
}

std::wstring FxPropertiesKeyPath(const EndpointId& id) {
    return EndpointKeyPath(id) + L"\\FxProperties";
}

std::wstring PropertiesKeyPath(const EndpointId& id) {
    return EndpointKeyPath(id) + L"\\Properties";
}

std::wstring BackupKeyPath(const EndpointId& id) {
    return std::wstring(kBackupRootKeyPath) + L"\\" + id.guid;
}

}  // namespace somike::installer
