#include "somike/core/render_stream.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstring>

#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/core/com_init.h"
#include "somike/protocol/shared_ring.h"

namespace somike::core {

using Microsoft::WRL::ComPtr;
namespace proto = somike::protocol;

namespace {

constexpr REFERENCE_TIME kBufferDuration = 400000;  // 40 ms (100 ns units)
constexpr DWORD kWaitMs = 500;

WAVEFORMATEX CanonicalFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(proto::kChannels);
    format.nSamplesPerSec = proto::kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

HRESULT OpenDefaultRender(ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    return hr;
}

}  // namespace

void RenderStream::Start(IRenderSource& source) {
    std::scoped_lock lock(mutex_);
    kicked_ = true;
    if (running_) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();  // a stream that closed itself; its thread has finished
    }
    stop_event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event_) {
        return;
    }
    running_ = true;
    kicked_ = false;
    try {
        thread_ = std::thread(&RenderStream::Run, this, &source, stop_event_.get());
    } catch (const std::system_error&) {
        running_ = false;
    }
}

void RenderStream::Stop() noexcept {
    std::thread thread;
    {
        std::scoped_lock lock(mutex_);
        if (stop_event_) {
            ::SetEvent(stop_event_.get());
        }
        thread = std::move(thread_);
    }
    if (thread.joinable()) {
        thread.join();
    }
    std::scoped_lock lock(mutex_);
    running_ = false;
    stop_event_.reset();
}

bool RenderStream::FinishIfIdle() {
    std::scoped_lock lock(mutex_);
    if (kicked_) {
        kicked_ = false;  // a sound was started meanwhile: keep going
        return false;
    }
    running_ = false;
    return true;
}

void RenderStream::Run(IRenderSource* source, HANDLE stop_event) {
    ComInit com(COINIT_MULTITHREADED);
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    win32::UniqueHandle ready(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    UINT32 buffer_frames = 0;

    constexpr DWORD kFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const WAVEFORMATEX format = CanonicalFormat();
    HRESULT hr = com.usable() ? S_OK : com.result();
    if (SUCCEEDED(hr) && !ready) hr = win32::LastErrorAsHResult();
    if (SUCCEEDED(hr)) hr = OpenDefaultRender(client);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags, kBufferDuration, 0, &format, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready.get());
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        log::Warn(L"Cannot open the output device for the soundboard: {}", win32::FormatHResult(hr));
        std::scoped_lock lock(mutex_);
        running_ = false;
        return;
    }

    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    ULONGLONG last_sound = ::GetTickCount64();
    bool finished = false;
    const HANDLE waits[] = {stop_event, ready.get()};
    while (!finished) {
        const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kWaitMs);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
            break;
        }
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        const UINT32 available = SUCCEEDED(hr) ? buffer_frames - padding : 0;
        BYTE* data = nullptr;
        if (SUCCEEDED(hr) && available > 0) {
            hr = render->GetBuffer(available, &data);
            if (SUCCEEDED(hr)) {
                std::memset(data, 0, std::size_t{available} * format.nBlockAlign);
                if (source->Render(reinterpret_cast<float*>(data), available)) {
                    last_sound = ::GetTickCount64();
                }
                hr = render->ReleaseBuffer(available, 0);
            }
        }
        if (FAILED(hr)) {
            // Typically AUDCLNT_E_DEVICE_INVALIDATED (device unplugged or the
            // default changed); the next Start() opens the new default.
            log::Info(L"Soundboard output stream ended: {}", win32::FormatHResult(hr));
            break;
        }
        if (::GetTickCount64() - last_sound > idle_ms_) {
            finished = FinishIfIdle();
            if (!finished) {
                last_sound = ::GetTickCount64();
            }
        }
    }

    client->Stop();
    if (mmcss != nullptr) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
    if (!finished) {
        std::scoped_lock lock(mutex_);
        running_ = false;
    }
}

}  // namespace somike::core
