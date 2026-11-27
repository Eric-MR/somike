#include "somike/core/router.h"

#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/ring_path.h"
#include "somike/protocol/shared_ring.h"

namespace somike::core {

namespace proto = somike::protocol;

Router::~Router() { Stop(); }

HRESULT Router::EnsureRing() {
    if (ring_file_.is_open() && writer_.valid()) {
        return S_OK;
    }
    const HRESULT hr = ring_file_.Open(paths::RingFile(), proto::kRegionBytes,
                                       win32::MappedFile::Access::ReadWrite);
    if (FAILED(hr)) {
        return hr;
    }
    writer_ = dsp::RingWriter(ring_file_.bytes());
    return writer_.valid() ? S_OK : E_UNEXPECTED;
}

HRESULT Router::Start(const RouteConfig& config) {
    std::scoped_lock lock(control_mutex_);

    capture_.Stop();
    frames_written_ = 0;

    HRESULT hr = EnsureRing();
    if (FAILED(hr)) {
        log::Error(L"Cannot open shared ring {}: {}", paths::RingFile(), win32::FormatHResult(hr));
        error_ = hr;
        state_ = RouterState::Failed;
        return hr;
    }

    writer_.SetGains(config.inject_gain, config.mute_microphone ? 0.0f : 1.0f);
    writer_.BeginSession(::GetTickCount64());

    hr = capture_.Start(config.target, *this);
    if (FAILED(hr)) {
        log::Error(L"Cannot start loopback capture of PID {}: {}", config.target.process_id,
                   win32::FormatHResult(hr));
        writer_.EndSession(::GetTickCount64());
        error_ = hr;
        state_ = RouterState::Failed;
        return hr;
    }

    error_ = S_OK;
    state_ = RouterState::Routing;
    log::Info(L"Routing started (PID {}, mode {})", config.target.process_id,
              config.target.mode == CaptureTarget::Mode::IncludeProcessTree ? L"include"
                                                                             : L"exclude");
    return S_OK;
}

void Router::Stop() noexcept {
    std::scoped_lock lock(control_mutex_);
    capture_.Stop();
    if (writer_.valid()) {
        writer_.EndSession(::GetTickCount64());
    }
    if (state_.exchange(RouterState::Stopped) == RouterState::Routing) {
        log::Info(L"Routing stopped after {} frames", frames_written_.load());
    }
}

void Router::SetGains(float inject_gain, bool mute_microphone) noexcept {
    // The lock only protects writer_ itself (EnsureRing may replace it); the
    // gains are independent atomics that the APO picks up on its next block.
    std::scoped_lock lock(control_mutex_);
    if (writer_.valid()) {
        writer_.SetGains(inject_gain, mute_microphone ? 0.0f : 1.0f);
    }
}

RouterStatus Router::status() const noexcept {
    RouterStatus status;
    status.state = state_.load();
    status.error = error_.load();
    status.frames_written = frames_written_.load();
    return status;
}

std::optional<dsp::ConsumerSnapshot> Router::ConsumerStatus() {
    std::scoped_lock lock(control_mutex_);
    if (FAILED(EnsureRing())) {
        return std::nullopt;
    }
    return writer_.ReadConsumer();
}

void Router::OnAudio(const float* frames, std::size_t frame_count) noexcept {
    writer_.Write(frames, frame_count, ::GetTickCount64());
    frames_written_.fetch_add(frame_count, std::memory_order_relaxed);

    float block_peak = 0.0f;
    for (std::size_t i = 0; i < frame_count * proto::kChannels; ++i) {
        const float magnitude = frames[i] < 0.0f ? -frames[i] : frames[i];
        block_peak = magnitude > block_peak ? magnitude : block_peak;  // NaN never wins
    }
    float previous = peak_.load(std::memory_order_relaxed);
    while (block_peak > previous &&
           !peak_.compare_exchange_weak(previous, block_peak, std::memory_order_relaxed)) {
    }
}

void Router::OnTick() noexcept { writer_.Heartbeat(::GetTickCount64()); }

void Router::OnCaptureError(HRESULT hr) noexcept {
    // Runs on the capture thread, which exits right after this call.
    writer_.EndSession(::GetTickCount64());
    error_ = hr;
    state_ = RouterState::Failed;
}

}  // namespace somike::core
