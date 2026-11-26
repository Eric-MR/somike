#include "somike/common/hresult.h"

#include <audioclient.h>
#include <audioenginebaseapo.h>

#include <cwchar>
#include <iterator>

namespace somike::win32 {

namespace {

struct KnownCode {
    HRESULT hr;
    const wchar_t* text;
};

// Codes from facilities that FormatMessage cannot resolve.
constexpr KnownCode kKnownCodes[] = {
    {AUDCLNT_E_NOT_INITIALIZED, L"AUDCLNT_E_NOT_INITIALIZED"},
    {AUDCLNT_E_ALREADY_INITIALIZED, L"AUDCLNT_E_ALREADY_INITIALIZED"},
    {AUDCLNT_E_DEVICE_INVALIDATED, L"Audio device was removed or reconfigured"},
    {AUDCLNT_E_UNSUPPORTED_FORMAT, L"Audio format not supported"},
    {AUDCLNT_E_DEVICE_IN_USE, L"Audio device is in use (exclusive mode)"},
    {AUDCLNT_E_SERVICE_NOT_RUNNING, L"Windows Audio service is not running"},
    {AUDCLNT_E_ENDPOINT_CREATE_FAILED, L"AUDCLNT_E_ENDPOINT_CREATE_FAILED"},
    {AUDCLNT_E_INVALID_STREAM_FLAG, L"AUDCLNT_E_INVALID_STREAM_FLAG"},
    {AUDCLNT_E_RESOURCES_INVALIDATED, L"AUDCLNT_E_RESOURCES_INVALIDATED"},
    {APOERR_FORMAT_NOT_SUPPORTED, L"APOERR_FORMAT_NOT_SUPPORTED"},
    {APOERR_ALREADY_INITIALIZED, L"APOERR_ALREADY_INITIALIZED"},
    {APOERR_NOT_INITIALIZED, L"APOERR_NOT_INITIALIZED"},
};

}  // namespace

HRESULT LastErrorAsHResult() noexcept {
    const DWORD error = ::GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

std::wstring FormatHResult(HRESULT hr) {
    wchar_t code[16];
    std::swprintf(code, std::size(code), L"0x%08X", static_cast<unsigned>(hr));
    std::wstring result = code;

    for (const KnownCode& known : kKnownCodes) {
        if (known.hr == hr) {
            return result + L" (" + known.text + L")";
        }
    }

    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        std::wstring message(buffer, length);
        ::LocalFree(buffer);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                    message.back() == L' ')) {
            message.pop_back();
        }
        result += L" (" + message + L")";
    }
    return result;
}

}  // namespace somike::win32
