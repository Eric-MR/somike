#include "somike/core/audio_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

// Define (not just declare) PKEY_Device_FriendlyName in this translation
// unit: initguid.h switches DEFINE_PROPERTYKEY to definition mode, and
// propkeydef.h re-evaluates the macro.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

namespace somike::core {

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IMMDeviceEnumerator> CreateEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       IID_PPV_ARGS(&enumerator));
    return enumerator;
}

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (device == nullptr || FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }
    std::wstring result(id);
    ::CoTaskMemFree(id);
    return result;
}

std::wstring DefaultId(IMMDeviceEnumerator* enumerator, ERole role) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, role, &device))) {
        return {};
    }
    return DeviceId(device.Get());
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return {};
    }
    PROPVARIANT value;
    ::PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    ::PropVariantClear(&value);
    return name;
}

}  // namespace

std::vector<AudioEndpoint> EnumerateCaptureEndpoints() {
    std::vector<AudioEndpoint> endpoints;
    const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
    if (!enumerator) {
        return endpoints;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        return endpoints;
    }
    const std::wstring default_console = DefaultId(enumerator.Get(), eConsole);
    const std::wstring default_comms = DefaultId(enumerator.Get(), eCommunications);

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        AudioEndpoint endpoint;
        endpoint.id = DeviceId(device.Get());
        if (endpoint.id.empty()) {
            continue;
        }
        endpoint.name = FriendlyName(device.Get());
        if (endpoint.name.empty()) {
            endpoint.name = endpoint.id;
        }
        endpoint.is_default = endpoint.id == default_console;
        endpoint.is_default_communications = endpoint.id == default_comms;
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

std::optional<std::wstring> DefaultCaptureEndpointId() {
    const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
    if (!enumerator) {
        return std::nullopt;
    }
    std::wstring id = DefaultId(enumerator.Get(), eConsole);
    if (id.empty()) {
        return std::nullopt;
    }
    return id;
}

}  // namespace somike::core
