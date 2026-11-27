#include "icon_cache.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <vector>

namespace somike::app {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kIconPixels = 64;  // shown at 30 CSS px: crisp up to 200 % scaling

std::optional<std::vector<BYTE>> RenderPng(const std::wstring& path) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage({kIconPixels, kIconPixels}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                                 &bitmap)) ||
        bitmap == nullptr) {
        return std::nullopt;
    }

    std::optional<std::vector<BYTE>> png;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> source;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha,
                                               &source)) &&
        (stream.Attach(::SHCreateMemStream(nullptr, 0)), stream) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit())) {
        STATSTG stat{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 &&
            stat.cbSize.QuadPart < (1u << 20)) {
            std::vector<BYTE> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
            ULONG read = 0;
            const LARGE_INTEGER zero{};
            if (SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) &&
                SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) &&
                read == bytes.size()) {
                png = std::move(bytes);
            }
        }
    }
    ::DeleteObject(bitmap);
    return png;
}

std::optional<std::string> Base64(const std::vector<BYTE>& bytes) {
    constexpr DWORD kFlags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD chars = 0;
    if (!::CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kFlags, nullptr,
                                &chars)) {
        return std::nullopt;
    }
    std::string text(chars, '\0');
    if (!::CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kFlags, text.data(),
                                &chars)) {
        return std::nullopt;
    }
    text.resize(chars);  // drops the terminating NUL
    return text;
}

}  // namespace

std::optional<std::string> IconCache::DataUrl(const std::wstring& image_path) {
    if (image_path.empty()) {
        return std::nullopt;
    }
    if (const auto it = cache_.find(image_path); it != cache_.end()) {
        return it->second;
    }
    std::optional<std::string> url;
    if (const auto png = RenderPng(image_path)) {
        if (const auto encoded = Base64(*png)) {
            url = "data:image/png;base64," + *encoded;
        }
    }
    cache_.emplace(image_path, url);
    return url;
}

}  // namespace somike::app
