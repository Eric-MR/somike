#include "somike/core/router.h"

#include <avrt.h>

#include <algorithm>

#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/ring_path.h"
#include "somike/dsp/voice_mixer.h"
#include "somike/protocol/shared_ring.h"

namespace somike::core {

namespace proto = somike::protocol;

namespace {

constexpr DWORD kPumpPeriodMs = 10;

// The pump's timing (tail, grace, gap; see PumpConfig) at the ring's rate.
dsp::PumpConfig RingPumpConfig() noexcept {
    dsp::PumpConfig config;
    config.frames_per_ms = proto::kSampleRate / 1000.0;
    config.lead_in_frames = proto::kSampleRate * 30 / 1000;
    config.max_burst_frames = proto::kSampleRate / 10;
    return config;
}

double NowMs() noexcept {
    static const double ms_per_tick = [] {
        LARGE_INTEGER frequency{};
        ::QueryPerformanceFrequency(&frequency);
        return 1000.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * ms_per_tick;
}

}  // namespace

Router::Router()
    : schedule_(RingPumpConfig()),
      pump_wake_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      pump_stop_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    // The pump thread then never allocates.
    pump_mix_.reserve(RingPumpConfig().max_burst_frames * proto::kChannels);
}

Router::~Router() {
    Stop();
    StopPump();
}

HRESULT Router::EnsureRing() {
    {
        std::scoped_lock write(write_mutex_);
        if (ring_file_.is_open() && writer_.valid()) {
            return S_OK;
        }
    }
    win32::MappedFile file;
    const HRESULT hr =
        file.Open(paths::RingFile(), proto::kRegionBytes, win32::MappedFile::Access::ReadWrite);
    if (FAILED(hr)) {
        return hr;
    }
    std::scoped_lock write(write_mutex_);
    ring_file_ = std::move(file);
    writer_ = dsp::RingWriter(ring_file_.bytes());
    session_open_ = false;
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

    {
        std::scoped_lock write(write_mutex_);
        app_gain_ = config.inject_gain;
        mute_microphone_ = config.mute_microphone;
        capture_running_ = true;
        schedule_.OnCaptureStart(NowMs());
        UpdateRingGains();
        if (!session_open_) {
            writer_.BeginSession(::GetTickCount64());
            session_open_ = true;
        }
    }

    hr = capture_.Start(config.target, *this);
    if (FAILED(hr)) {
        log::Error(L"Cannot start loopback capture of PID {}: {}", config.target.process_id,
                   win32::FormatHResult(hr));
        {
            std::scoped_lock write(write_mutex_);
            capture_running_ = false;
            UpdateRingGains();
            EndSessionUnlessSounding(NowMs());
        }
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
    {
        std::scoped_lock write(write_mutex_);
        capture_running_ = false;
        if (writer_.valid()) {
            UpdateRingGains();
            EndSessionUnlessSounding(NowMs());
        }
    }
    if (pump_wake_) {
        ::SetEvent(pump_wake_.get());  // a sound may still be playing: the pump takes over
    }
    if (state_.exchange(RouterState::Stopped) == RouterState::Routing) {
        log::Info(L"Routing stopped after {} frames", frames_written_.load());
    }
}

void Router::SetGains(float inject_gain, bool mute_microphone) noexcept {
    std::scoped_lock write(write_mutex_);
    app_gain_ = inject_gain;
    mute_microphone_ = mute_microphone;
    if (writer_.valid()) {
        UpdateRingGains();
    }
}

void Router::SetSoundSource(dsp::VoiceMixer* source) noexcept {
    std::scoped_lock write(write_mutex_);
    sound_ = source;
}

HRESULT Router::KickSoundSource() {
    std::scoped_lock lock(control_mutex_);
    const HRESULT hr = EnsureRing();
    StartPump();
    {
        std::scoped_lock write(write_mutex_);
        schedule_.Kick(NowMs());
    }
    if (pump_wake_) {
        ::SetEvent(pump_wake_.get());
    }
    return hr;
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
    std::scoped_lock write(write_mutex_);
    return writer_.ReadConsumer();
}

// ---------------------------------------------------------------------------
// ring writing (write_mutex_ held)
// ---------------------------------------------------------------------------
void Router::UpdateRingGains() {
    // Inject gain 1: the app and the sounds are scaled before the ring.
    const bool mute = capture_running_ && mute_microphone_;
    writer_.SetGains(1.0f, mute ? 0.0f : 1.0f);
}

void Router::EndSessionUnlessSounding(double now_ms) {
    if (session_open_ && !capture_running_ && !schedule_.Sounding(now_ms)) {
        writer_.EndSession(::GetTickCount64());
        session_open_ = false;
    }
}

void Router::WriteBlock(const float* frames, std::size_t count) {
    if (writer_.valid()) {
        writer_.Write(frames, count, ::GetTickCount64());
    }
}

bool Router::PumpOnce(double now_ms) {
    const bool sounding = sound_ != nullptr && sound_->active();
    // Without a ring (not set up yet) the pump still runs the voices in real
    // time as if a session were open, so they end normally; only the
    // microphone does not get them.
    const bool ring = writer_.valid();
    const dsp::PumpStep step = schedule_.Next(now_ms, sounding, capture_running_, session_open_ || !ring);

    if (step.begin_session) {
        UpdateRingGains();
        writer_.BeginSession(::GetTickCount64());
        session_open_ = true;
    }
    switch (step.action) {
        case dsp::PumpStep::Action::Sleep:
            if (step.end_session && session_open_) {
                writer_.EndSession(::GetTickCount64());
                session_open_ = false;
            }
            return false;  // nothing to play: sleep until the next kick
        case dsp::PumpStep::Action::Tick:
            return true;
        case dsp::PumpStep::Action::Write:
            break;
    }

    pump_mix_.assign(step.frames() * proto::kChannels, 0.0f);  // within the reserved capacity
    if (step.sound_frames > 0 && sound_ != nullptr) {
        sound_->MixInto(pump_mix_.data() + step.silence_frames * proto::kChannels, step.sound_frames);
    }
    WriteBlock(pump_mix_.data(), step.frames());
    schedule_.Wrote(now_ms, step);
    return true;
}

// ---------------------------------------------------------------------------
// pump thread
// ---------------------------------------------------------------------------
void Router::StartPump() {
    if (pump_.joinable()) {
        return;
    }
    if (!pump_wake_ || !pump_stop_) {
        log::Error(L"Cannot create the soundboard pump events: {}",
                   win32::FormatHResult(win32::LastErrorAsHResult()));
        return;
    }
    try {
        pump_ = std::thread(&Router::PumpThread, this);
    } catch (const std::system_error&) {
        log::Error(L"Cannot start the soundboard pump thread");
    }
}

void Router::StopPump() noexcept {
    if (pump_.joinable()) {
        ::SetEvent(pump_stop_.get());
        pump_.join();
    }
}

void Router::PumpThread() {
    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    // A high-resolution timer keeps the 10 ms period precise even while the
    // window is in the background (plain waits round up to ~15.6 ms). The
    // pacer makes either work; this just keeps the blocks small and even.
    win32::UniqueHandle timer(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS));

    for (;;) {
        bool ticking = false;
        {
            std::scoped_lock write(write_mutex_);
            ticking = PumpOnce(NowMs());
        }
        DWORD wait = 0;
        if (!ticking) {
            const HANDLE handles[] = {pump_stop_.get(), pump_wake_.get()};
            wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        } else if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(kPumpPeriodMs) * 10000;  // relative, 100 ns units
            ::SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE);
            const HANDLE handles[] = {pump_stop_.get(), pump_wake_.get(), timer.get()};
            wait = ::WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        } else {
            const HANDLE handles[] = {pump_stop_.get(), pump_wake_.get()};
            wait = ::WaitForMultipleObjects(2, handles, FALSE, kPumpPeriodMs);
        }
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
            break;
        }
    }

    if (mmcss != nullptr) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
}

// ---------------------------------------------------------------------------
// capture callbacks (capture thread)
// ---------------------------------------------------------------------------
void Router::OnAudio(const float* frames, std::size_t frame_count) noexcept {
    const std::size_t samples = frame_count * proto::kChannels;
    float block_peak = 0.0f;
    for (std::size_t i = 0; i < samples; ++i) {
        const float magnitude = frames[i] < 0.0f ? -frames[i] : frames[i];
        block_peak = magnitude > block_peak ? magnitude : block_peak;  // NaN never wins
    }
    float previous = peak_.load(std::memory_order_relaxed);
    while (block_peak > previous &&
           !peak_.compare_exchange_weak(previous, block_peak, std::memory_order_relaxed)) {
    }

    std::scoped_lock write(write_mutex_);
    try {
        capture_mix_.resize(samples);
    } catch (const std::bad_alloc&) {
        return;
    }
    for (std::size_t i = 0; i < samples; ++i) {
        capture_mix_[i] = frames[i] * app_gain_;
    }
    if (sound_ != nullptr) {
        sound_->MixInto(capture_mix_.data(), frame_count);
    }
    WriteBlock(capture_mix_.data(), frame_count);
    schedule_.OnCaptureBlock(NowMs());  // if the app falls silent, the pump continues from here
    frames_written_.fetch_add(frame_count, std::memory_order_relaxed);
}

void Router::OnTick() noexcept {
    std::scoped_lock write(write_mutex_);
    writer_.Heartbeat(::GetTickCount64());
}

void Router::OnCaptureError(HRESULT hr) noexcept {
    // Runs on the capture thread, which exits right after this call.
    {
        std::scoped_lock write(write_mutex_);
        capture_running_ = false;
        if (writer_.valid()) {
            UpdateRingGains();
            EndSessionUnlessSounding(NowMs());
        }
    }
    if (pump_wake_) {
        ::SetEvent(pump_wake_.get());
    }
    error_ = hr;
    state_ = RouterState::Failed;
}

}  // namespace somike::core
