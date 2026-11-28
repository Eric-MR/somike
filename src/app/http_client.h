#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace somike::app {

struct HttpResponse {
    std::uint32_t status = 0;
    std::string body;
};

// Blocking HTTPS GET with WinHTTP: system proxy settings, certificate
// validation by Windows, redirects followed (never from https to http),
// gzip/deflate handled. Fails with HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)
// when the body exceeds `max_bytes`, and with E_INVALIDARG for non-https URLs.
// Any status code is a success at this level; check `status`.
[[nodiscard]] HRESULT HttpGet(const std::wstring& url, std::size_t max_bytes, HttpResponse& out);

}  // namespace somike::app
