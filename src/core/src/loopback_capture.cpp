#include "somike/core/loopback_capture.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <future>
#include <vector>

#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/core/com_init.h"
#include "somike/protocol/shared_ring.h"

namespace somike::core {

using Microsoft::WRL::ComPtr;
namespace proto = somike::protocol;

namespace {

constexpr REFERENCE_TIME kBufferDuration = 200000;  // 20 ms (100 ns units)
constexpr DWORD kActivationTimeoutMs = 5000;
constexpr DWORD kTickMs = 100;

// Receives the result of ActivateAudioInterfaceAsync. Must be agile
// (FtmBase) because the callback arrives on an arbitrary worker thread.
class ActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    HRESULT RuntimeClassInitialize() noexcept {
        done_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        return done_ ? S_OK : win32::LastErrorAsHResult();
    }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activate_result = E_FAIL;
        ComPtr<IUnknown> unknown;
        HRESULT hr = operation->GetActivateResult(&activate_result, &unknown);
        if (SUCCEEDED(hr)) {
            hr = activate_result;
        }
        if (SUCCEEDED(hr)) {
            hr = unknown.As(&client_);
        }
        result_ = hr;
        ::SetEvent(done_.get());
        return S_OK;
    }

    HRESULT Wait(ComPtr<IAudioClient>& client) noexcept {
        if (::WaitForSingleObject(done_.get(), kActivationTimeoutMs) != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        client = client_;
        return result_;
    }

private:
    win32::UniqueHandle done_;
    HRESULT result_ = E_FAIL;
    ComPtr<IAudioClient> client_;
};

HRESULT ActivateProcessLoopback(const CaptureTarget& target, ComPtr<IAudioClient>& client) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = target.process_id;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        target.mode == CaptureTarget::Mode::IncludeProcessTree
            ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
            : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);  // not owned: no PropVariantClear

    ComPtr<ActivationHandler> handler;
    HRESULT hr = Microsoft::WRL::MakeAndInitialize<ActivationHandler>(&handler);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    hr = ::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                       __uuidof(IAudioClient), &activation, handler.Get(),
                                       &operation);
    if (FAILED(hr)) {
        return hr;
    }
    return handler->Wait(client);
}

WAVEFORMATEX CanonicalFloatFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(proto::kChannels);
    format.nSamplesPerSec = proto::kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

WAVEFORMATEX CanonicalPcm16Format() {
    WAVEFORMATEX format = CanonicalFloatFormat();
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

// Initialises the client for the canonical format, preferring float. The
// AUTOCONVERTPCM flag lets the engine convert the source mix for us.
HRESULT InitializeClient(IAudioClient* client, bool& is_float) {
    constexpr DWORD kFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    WAVEFORMATEX format = CanonicalFloatFormat();
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags, kBufferDuration, 0, &format,
                                    nullptr);
    if (SUCCEEDED(hr)) {
        is_float = true;
        return hr;
    }
    log::Warn(L"Float loopback format rejected ({}); falling back to 16-bit PCM",
              win32::FormatHResult(hr));
    format = CanonicalPcm16Format();
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags, kBufferDuration, 0, &format,
                            nullptr);
    is_float = false;
    return hr;
}

// Pulls every pending packet and forwards it in canonical form.
HRESULT Drain(IAudioCaptureClient* capture, bool is_float, std::vector<float>& scratch,
              ICaptureSink& sink) {
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = capture->GetNextPacketSize(&packet);
        if (FAILED(hr) || packet == 0) {
            return hr;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            return hr;
        }
        if (frames > 0) {
            const std::size_t samples = std::size_t{frames} * proto::kChannels;
            if (scratch.size() < samples) {
                scratch.resize(samples);
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                std::fill_n(scratch.begin(), samples, 0.0f);
            } else if (is_float) {
                std::memcpy(scratch.data(), data, samples * sizeof(float));
            } else {
                const auto* pcm = reinterpret_cast<const std::int16_t*>(data);
                for (std::size_t i = 0; i < samples; ++i) {
                    scratch[i] = static_cast<float>(pcm[i]) / 32768.0f;
                }
            }
            sink.OnAudio(scratch.data(), frames);
        }
        hr = capture->ReleaseBuffer(frames);
        if (FAILED(hr)) {
            return hr;
        }
    }
}

void CaptureThread(CaptureTarget target, ICaptureSink* sink, HANDLE stop_event,
                   std::promise<HRESULT> started) {
    ComInit com(COINIT_MULTITHREADED);

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    win32::UniqueHandle ready(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    bool is_float = true;

    HRESULT hr = com.usable() ? S_OK : com.result();
    if (SUCCEEDED(hr) && !ready) hr = win32::LastErrorAsHResult();
    if (SUCCEEDED(hr)) hr = ActivateProcessLoopback(target, client);
    if (SUCCEEDED(hr)) hr = InitializeClient(client.Get(), is_float);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready.get());
    if (SUCCEEDED(hr)) hr = client->Start();
    started.set_value(hr);
    if (FAILED(hr)) {
        return;
    }

    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

    std::vector<float> scratch(std::size_t{proto::kSampleRate} / 10 * proto::kChannels);  // 100 ms
    const HANDLE waits[] = {stop_event, ready.get()};
    for (;;) {
        const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, kTickMs);
        if (wait == WAIT_OBJECT_0) {
            break;  // stop requested
        }
        if (wait == WAIT_FAILED) {
            sink->OnCaptureError(win32::LastErrorAsHResult());
            break;
        }
        hr = Drain(capture.Get(), is_float, scratch, *sink);
        if (FAILED(hr)) {
            log::Error(L"Loopback capture failed: {}", win32::FormatHResult(hr));
            sink->OnCaptureError(hr);
            break;
        }
        sink->OnTick();
    }

    client->Stop();
    if (mmcss != nullptr) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
}

}  // namespace

HRESULT LoopbackCapture::Start(const CaptureTarget& target, ICaptureSink& sink) {
    Stop();
    stop_event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event_) {
        return win32::LastErrorAsHResult();
    }

    std::promise<HRESULT> started;
    std::future<HRESULT> result = started.get_future();
    try {
        thread_ = std::thread(CaptureThread, target, &sink, stop_event_.get(), std::move(started));
    } catch (const std::system_error&) {
        return E_FAIL;
    }

    const HRESULT hr = result.get();
    if (FAILED(hr)) {
        thread_.join();
        stop_event_.reset();
        return hr;
    }
    running_ = true;
    return S_OK;
}

void LoopbackCapture::Stop() noexcept {
    if (thread_.joinable()) {
        ::SetEvent(stop_event_.get());
        thread_.join();
    }
    stop_event_.reset();
    running_ = false;
}

}  // namespace somike::core
