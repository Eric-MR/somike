// somike-selftest - end-to-end check of the routing pipeline on real hardware.
//
// Plays a quiet 1 kHz tone from this process, routes this process's audio into
// the default microphone through the installed APO, records that microphone
// and measures the tone level in each phase:
//
//   A  tone playing, router off      -> tone should be absent from the mic
//   B  routing this process          -> tone clearly present
//   C  routing + "mute my mic"       -> tone present, other mic noise gone
//   D  router stopped                -> tone absent again
//
// Requires `somike-apoctl install` on the default microphone. Developer tool;
// not part of the shipped package.
//
// Exit codes: 0 pass, 1 fail, 2 could not run.

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "somike/common/mapped_file.h"
#include "somike/common/ring_path.h"
#include "somike/core/com_init.h"
#include "somike/dsp/ring_writer.h"
#include "somike/protocol/shared_ring.h"
#include "somike/core/router.h"

namespace {

using Microsoft::WRL::ComPtr;
using somike::core::CaptureTarget;
using somike::core::RouteConfig;
using somike::core::Router;

constexpr UINT32 kRate = 48000;
constexpr double kToneHz = 1000.0;
constexpr float kToneAmplitude = 0.05f;           // about -26 dBFS
constexpr std::size_t kBlock = 4800;              // 100 ms: 1 kHz falls exactly on bin 100
constexpr REFERENCE_TIME kBufferDuration = 1'000'000;  // 100 ms
constexpr double kPhaseSeconds = 2.5;
constexpr std::size_t kSkipFrames = kRate / 2;    // ignore the first 0.5 s (priming)

WAVEFORMATEX FloatStereo() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = kRate * 8;
    return f;
}

// How a recording app opens the microphone. Voice apps differ here, and the
// processing mode decides which effects (Somike included) the stream gets.
struct CaptureMode {
    const wchar_t* name;
    AUDIO_STREAM_CATEGORY category;
    AUDCLNT_STREAMOPTIONS options;
};

constexpr CaptureMode kDefaultMode{L"default", AudioCategory_Other, AUDCLNT_STREAMOPTIONS_NONE};
constexpr CaptureMode kCaptureModes[] = {
    kDefaultMode,
    {L"communications", AudioCategory_Communications, AUDCLNT_STREAMOPTIONS_NONE},
    {L"raw", AudioCategory_Other, AUDCLNT_STREAMOPTIONS_RAW},
    {L"communications+raw", AudioCategory_Communications, AUDCLNT_STREAMOPTIONS_RAW},
};

std::optional<CaptureMode> FindCaptureMode(std::wstring_view name) {
    for (const CaptureMode& mode : kCaptureModes) {
        if (name == mode.name) return mode;
    }
    return std::nullopt;
}

// Opens a shared-mode client on the default endpoint, converting to/from
// 48 kHz stereo float.
HRESULT OpenDefaultClient(EDataFlow flow, ComPtr<IAudioClient>& client, DWORD extra_flags = 0,
                          const CaptureMode& mode = kDefaultMode) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(hr)) return hr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;
    if (mode.category != AudioCategory_Other || mode.options != AUDCLNT_STREAMOPTIONS_NONE) {
        ComPtr<IAudioClient2> client2;
        hr = client.As(&client2);
        if (FAILED(hr)) return hr;
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = mode.category;
        properties.Options = mode.options;
        hr = client2->SetClientProperties(&properties);
        if (FAILED(hr)) return hr;
    }
    const WAVEFORMATEX format = FloatStereo();
    return client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                  extra_flags,
                              kBufferDuration, 0, &format, nullptr);
}

// Plays the test tone on the default render device until stopped.
class TonePlayer {
public:
    ~TonePlayer() { Stop(); }

    HRESULT Start() {
        std::atomic<HRESULT> result{E_PENDING};
        thread_ = std::thread([this, &result] { Run(result); });
        while (result.load() == E_PENDING) ::Sleep(5);
        const HRESULT hr = result.load();
        if (FAILED(hr)) Stop();
        return hr;
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    void Run(std::atomic<HRESULT>& result) {
        somike::core::ComInit com;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        UINT32 buffer_frames = 0;
        HRESULT hr = OpenDefaultClient(eRender, client);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
        // Windows remembers per-app volume; make the reference level exact.
        ComPtr<ISimpleAudioVolume> session_volume;
        if (SUCCEEDED(hr) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&session_volume)))) {
            float level = 0.0f;
            BOOL muted = FALSE;
            (void)session_volume->GetMasterVolume(&level);
            (void)session_volume->GetMute(&muted);
            std::wprintf(L"  test tone session volume was %.1f%%%ls; set to 100%%\n", level * 100.0f,
                         muted ? L" (muted)" : L"");
            (void)session_volume->SetMasterVolume(1.0f, nullptr);
            (void)session_volume->SetMute(FALSE, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->Start();
        result = SUCCEEDED(hr) ? S_OK : hr;
        if (FAILED(hr)) return;

        const double increment = 2.0 * std::numbers::pi * kToneHz / kRate;
        double phase = 0.0;
        while (!stop_) {
            UINT32 padding = 0;
            if (SUCCEEDED(client->GetCurrentPadding(&padding)) && padding < buffer_frames) {
                const UINT32 frames = buffer_frames - padding;
                BYTE* data = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &data))) {
                    auto* samples = reinterpret_cast<float*>(data);
                    for (UINT32 i = 0; i < frames; ++i) {
                        const float s = kToneAmplitude * static_cast<float>(std::sin(phase));
                        samples[2 * i] = s;
                        samples[2 * i + 1] = s;
                        phase += increment;
                        if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
                    }
                    render->ReleaseBuffer(frames, 0);
                }
            }
            ::Sleep(10);
        }
        client->Stop();
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// Records the default microphone (or, with `speaker_loopback`, what the
// default render device plays), downmixed to mono.
HRESULT RecordMicrophone(double seconds, std::vector<float>& out, bool speaker_loopback = false,
                         const CaptureMode& mode = kDefaultMode) {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HRESULT hr = OpenDefaultClient(speaker_loopback ? eRender : eCapture, client,
                                   speaker_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, mode);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) return hr;

    const auto target = static_cast<std::size_t>(seconds * kRate);
    out.clear();
    out.reserve(target);
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0) + 3000;
    while (out.size() < target) {
        if (::GetTickCount64() > deadline) {
            client->Stop();
            std::wprintf(L"  (microphone delivered only %zu of %zu frames)\n", out.size(), target);
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        ::Sleep(10);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const auto* samples = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                out.push_back(silent ? 0.0f : 0.5f * (samples[2 * i] + samples[2 * i + 1]));
            }
            capture->ReleaseBuffer(frames);
        }
    }
    client->Stop();
    return S_OK;
}

// Prints the default endpoint's volume (the mic level scales the routed audio
// if the endpoint volume is applied in software after the effect).
void PrintEndpointVolume(EDataFlow flow, const wchar_t* label) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.GetAddressOf())))) {
        std::wprintf(L"  %ls volume: unavailable\n", label);
        return;
    }
    float db = 0.0f;
    float scalar = 0.0f;
    BOOL muted = FALSE;
    DWORD hardware = 0;
    (void)volume->GetMasterVolumeLevel(&db);
    (void)volume->GetMasterVolumeLevelScalar(&scalar);
    (void)volume->GetMute(&muted);
    (void)volume->QueryHardwareSupport(&hardware);
    std::wprintf(L"  %ls volume: %.0f%% (%.1f dB)%ls%ls\n", label, scalar * 100.0f, db,
                 muted ? L", MUTED" : L"",
                 (hardware & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? L", hardware" : L", software");
}

struct Levels {
    double tone_db = -200.0;      // level of the 1 kHz component (peak amplitude, dBFS)
    double residual_db = -200.0;  // RMS of everything else (dBFS)
};

double ToDb(double amplitude) { return 20.0 * std::log10(std::max(amplitude, 1e-10)); }

// Per 100 ms block: least-squares sine fit around 1 kHz (searching +-10 Hz,
// because drift compensation may shift the pitch by a fraction of a percent),
// then the RMS of what the fit does not explain (noise + distortion).
Levels Analyze(const std::vector<float>& samples) {
    double tone_power = 0.0;      // mean of amplitude^2
    double residual_power = 0.0;  // mean of (sample - fit)^2
    std::size_t blocks = 0;
    for (std::size_t start = kSkipFrames; start + kBlock <= samples.size(); start += kBlock) {
        const float* x = samples.data() + start;
        double best_a = 0.0, best_b = 0.0, best_w = 0.0, best_amp_sq = -1.0;
        for (double hz = kToneHz - 10.0; hz <= kToneHz + 10.0; hz += 0.25) {
            const double w = 2.0 * std::numbers::pi * hz / kRate;
            double sc = 0.0, ss = 0.0;
            for (std::size_t i = 0; i < kBlock; ++i) {
                const double phase = w * static_cast<double>(i);
                sc += x[i] * std::cos(phase);
                ss += x[i] * std::sin(phase);
            }
            const double a = 2.0 * sc / static_cast<double>(kBlock);
            const double b = 2.0 * ss / static_cast<double>(kBlock);
            if (a * a + b * b > best_amp_sq) {
                best_amp_sq = a * a + b * b;
                best_a = a;
                best_b = b;
                best_w = w;
            }
        }
        double residual = 0.0;
        for (std::size_t i = 0; i < kBlock; ++i) {
            const double phase = best_w * static_cast<double>(i);
            const double e = x[i] - (best_a * std::cos(phase) + best_b * std::sin(phase));
            residual += e * e;
        }
        tone_power += best_amp_sq;
        residual_power += residual / static_cast<double>(kBlock);
        ++blocks;
    }
    Levels levels;
    if (blocks == 0) return levels;
    levels.tone_db = ToDb(std::sqrt(tone_power / static_cast<double>(blocks)));
    levels.residual_db = ToDb(std::sqrt(residual_power / static_cast<double>(blocks)));
    return levels;
}

// Tone level of the most recent 100 ms in the shared ring (what the producer
// wrote, before the APO touches it). Left channel.
std::optional<double> RingToneDb() {
    namespace proto = somike::protocol;
    somike::win32::MappedFile ring;
    if (FAILED(ring.Open(somike::paths::RingFile(), proto::kRegionBytes,
                         somike::win32::MappedFile::Access::ReadOnly))) {
        return std::nullopt;
    }
    const somike::dsp::RingSnapshot snapshot = somike::dsp::InspectRing(ring.bytes());
    if (!snapshot.compatible || snapshot.write_frame < kBlock) {
        return std::nullopt;
    }
    const auto* samples =
        reinterpret_cast<const float*>(ring.bytes().data() + proto::kHeaderBytes);
    std::vector<float> left(kSkipFrames + kBlock);
    const std::uint64_t first = snapshot.write_frame - kBlock;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const std::uint64_t frame = (first + i) % proto::kCapacityFrames;
        left[kSkipFrames + i] = samples[frame * proto::kChannels];
    }
    return Analyze(left).tone_db;
}

struct PhaseResult {
    Levels levels;
    std::uint64_t injected = 0;
    std::uint64_t underruns = 0;
    std::uint64_t resyncs = 0;
    bool ok = false;
};

PhaseResult RunPhase(const wchar_t* name, Router& router) {
    PhaseResult result;
    const auto before = router.ConsumerStatus();
    std::vector<float> samples;
    const HRESULT hr = RecordMicrophone(kPhaseSeconds, samples);
    const auto after = router.ConsumerStatus();
    if (FAILED(hr)) {
        std::wprintf(L"  %-26ls recording failed: 0x%08lX\n", name, static_cast<unsigned long>(hr));
        return result;
    }
    result.ok = true;
    result.levels = Analyze(samples);
    if (before && after) {
        result.injected = after->frames_injected - before->frames_injected;
        result.underruns = after->underruns - before->underruns;
        result.resyncs = after->resyncs - before->resyncs;
    }
    std::wprintf(L"  %-26ls tone %7.1f dBFS   other %7.1f dBFS   injected %7llu   underruns %llu   resyncs %llu\n",
                 name, result.levels.tone_db, result.levels.residual_db,
                 static_cast<unsigned long long>(result.injected),
                 static_cast<unsigned long long>(result.underruns),
                 static_cast<unsigned long long>(result.resyncs));
    return result;
}

}  // namespace

// --listen <seconds> [--mode <name>]: record the default microphone like a
// voice app would and print its level every half second (for real-app
// scenarios: route e.g. Spotify with somike.exe, then listen here).
int Listen(double seconds, const CaptureMode& mode) {
    std::wprintf(L"Listening to the default microphone (%ls mode)\n", mode.name);
    std::vector<float> samples;
    if (const HRESULT hr = RecordMicrophone(seconds, samples, false, mode); FAILED(hr)) {
        std::wprintf(L"Recording failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    constexpr std::size_t kWindow = kRate / 2;
    double total_sq = 0.0;
    for (std::size_t start = 0; start + kWindow <= samples.size(); start += kWindow) {
        double sum_sq = 0.0;
        float peak = 0.0f;
        for (std::size_t i = start; i < start + kWindow; ++i) {
            sum_sq += static_cast<double>(samples[i]) * samples[i];
            peak = std::max(peak, std::abs(samples[i]));
        }
        total_sq += sum_sq;
        const double rms_db = ToDb(std::sqrt(sum_sq / kWindow));
        const int bar = std::clamp(static_cast<int>((rms_db + 70.0) / 2.0), 0, 35);
        std::wprintf(L"  %5.1f s  rms %6.1f dBFS  peak %6.1f dBFS  %ls\n",
                     static_cast<double>(start) / kRate, rms_db, ToDb(peak),
                     std::wstring(static_cast<std::size_t>(bar), L'#').c_str());
    }
    std::wprintf(L"  overall rms %.1f dBFS, 1 kHz test tone %.1f dBFS\n",
                 ToDb(std::sqrt(total_sq / std::max<std::size_t>(samples.size(), 1))),
                 Analyze(samples).tone_db);
    return 0;
}

// --tone <seconds>: just play the 1 kHz test tone from this process (no
// routing), e.g. as a stand-in app for somike.exe scenarios.
int PlayTone(double seconds) {
    TonePlayer tone;
    if (const HRESULT hr = tone.Start(); FAILED(hr)) {
        std::wprintf(L"Could not play the test tone: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    ::Sleep(static_cast<DWORD>(seconds * 1000.0));
    tone.Stop();
    return 0;
}

// --modes: routes the test tone and records the microphone once per capture
// processing mode. Voice apps open the mic in different modes (Discord's
// voice engine, for example, does not use the default one), and only modes
// that pass through the Somike effect hear the routed audio.
int ProbeModes() {
    std::wprintf(L"Somike capture-mode check (default microphone, quiet 1 kHz tone)\n\n");
    TonePlayer tone;
    if (const HRESULT hr = tone.Start(); FAILED(hr)) {
        std::wprintf(L"Could not play the test tone: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    Router router;
    RouteConfig config;
    config.target = CaptureTarget::Application(::GetCurrentProcessId());
    config.inject_gain = 1.0f;
    if (const HRESULT hr = router.Start(config); FAILED(hr)) {
        std::wprintf(L"Router failed to start: 0x%08lX (is the injector installed?)\n",
                     static_cast<unsigned long>(hr));
        return 2;
    }
    ::Sleep(300);

    const double expected_db = ToDb(kToneAmplitude);
    bool all_pass = true;
    for (const CaptureMode& mode : kCaptureModes) {
        const auto before = router.ConsumerStatus();
        std::vector<float> samples;
        const HRESULT hr = RecordMicrophone(kPhaseSeconds, samples, false, mode);
        const auto after = router.ConsumerStatus();
        if (FAILED(hr)) {
            std::wprintf(L"  %-20ls could not record: 0x%08lX\n", mode.name, static_cast<unsigned long>(hr));
            all_pass = false;
            continue;
        }
        const Levels levels = Analyze(samples);
        const std::uint64_t injected =
            before && after ? after->frames_injected - before->frames_injected : 0;
        const bool heard = levels.tone_db > expected_db - 6.0;
        all_pass = all_pass && heard;
        std::wprintf(L"  %-20ls tone %7.1f dBFS   injected %7llu   [%ls]\n", mode.name, levels.tone_db,
                     static_cast<unsigned long long>(injected), heard ? L"PASS" : L"FAIL");
    }
    router.Stop();
    tone.Stop();
    std::wprintf(L"\n%ls\n", all_pass ? L"RESULT: PASS (routed audio reaches every capture mode)"
                                       : L"RESULT: FAIL (some capture modes bypass the Somike effect)");
    return all_pass ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    somike::core::ComInit com;
    if (!com.usable()) {
        std::wprintf(L"COM initialisation failed\n");
        return 2;
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--listen") {
        CaptureMode mode = kDefaultMode;
        if (argc >= 5 && std::wstring_view(argv[3]) == L"--mode") {
            const auto found = FindCaptureMode(argv[4]);
            if (!found) {
                std::wprintf(L"Unknown mode; use default, communications, raw or communications+raw\n");
                return 2;
            }
            mode = *found;
        }
        return Listen(std::clamp(std::wcstod(argv[2], nullptr), 1.0, 120.0), mode);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--modes") {
        return ProbeModes();
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--tone") {
        return PlayTone(std::clamp(std::wcstod(argv[2], nullptr), 1.0, 600.0));
    }

    std::wprintf(L"Somike end-to-end self-test (default microphone, quiet 1 kHz tone)\n");
    PrintEndpointVolume(eCapture, L"microphone");
    PrintEndpointVolume(eRender, L"speakers  ");
    std::wprintf(L"\n");

    TonePlayer tone;
    if (const HRESULT hr = tone.Start(); FAILED(hr)) {
        std::wprintf(L"Could not play the test tone: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    ::Sleep(300);

    {
        std::vector<float> played;
        if (SUCCEEDED(RecordMicrophone(1.5, played, true))) {
            std::wprintf(L"  tone in the speaker mix: %.1f dBFS (generated at %.1f dBFS)\n\n",
                         Analyze(played).tone_db, ToDb(kToneAmplitude));
        }
    }

    Router router;
    const PhaseResult a = RunPhase(L"A  router off", router);

    RouteConfig config;
    config.target = CaptureTarget::Application(::GetCurrentProcessId());
    config.inject_gain = 1.0f;
    if (const HRESULT hr = router.Start(config); FAILED(hr)) {
        std::wprintf(L"Router failed to start: 0x%08lX (is the injector installed?)\n",
                     static_cast<unsigned long>(hr));
        return 2;
    }
    ::Sleep(200);
    const PhaseResult b = RunPhase(L"B  routing", router);
    if (const auto ring_db = RingToneDb()) {
        std::wprintf(L"     (tone level written to the shared ring: %.1f dBFS)\n", *ring_db);
    }

    router.SetGains(1.0f, true);
    const PhaseResult c = RunPhase(L"C  routing + mic muted", router);

    router.Stop();
    ::Sleep(200);
    const PhaseResult d = RunPhase(L"D  router stopped", router);
    tone.Stop();

    if (!(a.ok && b.ok && c.ok && d.ok)) return 2;

    const double expected_db = ToDb(kToneAmplitude);
    const bool injected = b.injected > 0;
    const bool tone_routed = b.levels.tone_db > expected_db - 6.0 && b.levels.tone_db > a.levels.tone_db + 15.0;
    const bool tone_removed = d.levels.tone_db < b.levels.tone_db - 15.0;
    const bool muted_tone_kept = c.levels.tone_db > expected_db - 6.0;

    std::wprintf(L"\n");
    std::wprintf(L"  [%ls] APO injected frames while routing\n", injected ? L"PASS" : L"FAIL");
    std::wprintf(L"  [%ls] tone reaches the microphone at ~%.0f dBFS\n", tone_routed ? L"PASS" : L"FAIL", expected_db);
    std::wprintf(L"  [%ls] tone still routed while the mic is muted\n", muted_tone_kept ? L"PASS" : L"FAIL");
    std::wprintf(L"  [%ls] tone disappears when routing stops\n", tone_removed ? L"PASS" : L"FAIL");
    std::wprintf(L"  [info] mic noise with mute: %.1f dBFS (without: %.1f dBFS)\n", c.levels.residual_db,
                 b.levels.residual_db);

    const bool pass = injected && tone_routed && tone_removed && muted_tone_kept;
    std::wprintf(L"\n%ls\n", pass ? L"RESULT: PASS" : L"RESULT: FAIL");
    return pass ? 0 : 1;
}
