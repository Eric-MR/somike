#include "http_client.h"

#include <winhttp.h>

#include <memory>

#include "somike/common/hresult.h"
#include "somike/version.h"

namespace somike::app {

namespace {

struct InternetCloser {
    void operator()(HINTERNET handle) const noexcept { ::WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;

constexpr wchar_t kUserAgent[] = L"Somike/" SOMIKE_VERSION_WSTRING;
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 15000;
constexpr int kReceiveTimeoutMs = 30000;

HRESULT LastError() { return win32::LastErrorAsHResult(); }

}  // namespace

HRESULT HttpGet(const std::wstring& url, std::size_t max_bytes, HttpResponse& out) {
    out = {};

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return LastError();
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0) {
        return E_INVALIDARG;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    InternetHandle session(::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Before Windows 8.1 there is no automatic proxy mode.
        session.reset(::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session) {
        return LastError();
    }
    ::WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    InternetHandle connection(::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) {
        return LastError();
    }
    InternetHandle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return LastError();
    }
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    ::WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

    if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.get(), nullptr)) {
        return LastError();
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return LastError();
    }
    out.status = status;

    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
            return LastError();
        }
        if (available == 0) {
            break;
        }
        if (out.body.size() + available > max_bytes) {
            out.body.clear();
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        const std::size_t offset = out.body.size();
        out.body.resize(offset + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.get(), out.body.data() + offset, available, &read)) {
            return LastError();
        }
        out.body.resize(offset + read);
    }
    return S_OK;
}

}  // namespace somike::app
