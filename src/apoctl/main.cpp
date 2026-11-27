// somike-apoctl - installs, removes and diagnoses the Somike microphone
// injector (capture APO).
//
//   somike-apoctl list
//   somike-apoctl status    [--endpoint <id>|default]
//   somike-apoctl install   [--endpoint <id>|default] [--strategy auto|composite|replace]
//                           [--restart-audio] [--dry-run]
//   somike-apoctl uninstall [--endpoint <id>|default | --all] [--restart-audio]
//   somike-apoctl restart-audio
//
// Common: --report <file>  also write all output to <file> (UTF-8).
// install/uninstall/restart-audio must run elevated; list/status work unelevated.
#include <windows.h>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "somike/common/hresult.h"
#include "somike/common/mapped_file.h"
#include "somike/common/ring_path.h"
#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"
#include "somike/core/audio_devices.h"
#include "somike/core/com_init.h"
#include "somike/core/elevated_tool.h"
#include "somike/dsp/ring_writer.h"
#include "somike/installer/endpoint_id.h"
#include "somike/installer/fx_config.h"
#include "somike/installer/install_plan.h"
#include "somike/installer/system_install.h"
#include "somike/protocol/apo_identity.h"
#include "somike/protocol/shared_ring.h"
#include "somike/version.h"

namespace {

using namespace somike;
namespace inst = somike::installer;

enum ExitCode : int {
    kExitOk = 0,
    kExitFailure = 1,
    kExitUsage = 2,
    kExitRefused = 3,        // plan refused; choose an explicit --strategy
    kExitNeedsElevation = 4,
};

// Console + report sink.
class Output {
public:
    void Line(std::wstring_view text = {}) {
        std::wstring line(text);
        line += L"\r\n";
        text_ += line;

        const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD mode = 0;
        DWORD written = 0;
        if (::GetConsoleMode(out, &mode)) {
            ::WriteConsoleW(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        } else {
            const std::string utf8 = text::ToUtf8(line);
            ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }

    void Lines(const inst::Report& report) {
        for (const std::wstring& line : report) {
            Line(L"  " + line);
        }
    }

    void SaveReport(const std::wstring& path) const {
        if (path.empty()) {
            return;
        }
        win32::UniqueFileHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file) {
            const std::string utf8 = text::ToUtf8(text_);
            DWORD written = 0;
            ::WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }

private:
    std::wstring text_;
};

struct Options {
    std::wstring command;
    std::wstring endpoint;  // empty or "default": default capture device
    bool all = false;
    inst::Strategy strategy = inst::Strategy::Auto;
    bool restart_audio = false;
    bool dry_run = false;
    std::wstring report_path;
};

void PrintUsage(Output& out) {
    out.Line(L"somike-apoctl " SOMIKE_VERSION_WSTRING L" - Somike microphone injector control");
    out.Line();
    out.Line(L"Usage:");
    out.Line(L"  somike-apoctl list");
    out.Line(L"  somike-apoctl status    [--endpoint <id>|default]");
    out.Line(L"  somike-apoctl install   [--endpoint <id>|default] [--strategy auto|composite|replace]");
    out.Line(L"                          [--restart-audio] [--dry-run]");
    out.Line(L"  somike-apoctl uninstall [--endpoint <id>|default | --all] [--restart-audio]");
    out.Line(L"  somike-apoctl restart-audio   (reloads all audio effects, e.g. after an update)");
    out.Line();
    out.Line(L"Options:");
    out.Line(L"  --report <file>   also write all output to <file> (UTF-8)");
    out.Line();
    out.Line(L"install/uninstall/restart-audio require an elevated (administrator) prompt.");
    out.Line(L"Exit codes: 0 ok, 1 failure, 2 usage, 3 refused (pick --strategy), 4 needs elevation");
}

std::optional<Options> ParseArguments(int argc, wchar_t** argv, Output& out) {
    if (argc < 2) {
        return std::nullopt;
    }
    Options options;
    options.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        auto value = [&]() -> std::optional<std::wstring> {
            if (i + 1 >= argc) {
                out.Line(std::format(L"Missing value for {}", arg));
                return std::nullopt;
            }
            return std::wstring(argv[++i]);
        };
        if (arg == L"--endpoint") {
            auto v = value();
            if (!v) return std::nullopt;
            options.endpoint = *v;
        } else if (arg == L"--strategy") {
            auto v = value();
            if (!v) return std::nullopt;
            const auto strategy = inst::ParseStrategy(*v);
            if (!strategy) {
                out.Line(std::format(L"Unknown strategy: {}", *v));
                return std::nullopt;
            }
            options.strategy = *strategy;
        } else if (arg == L"--report") {
            auto v = value();
            if (!v) return std::nullopt;
            options.report_path = *v;
        } else if (arg == L"--all") {
            options.all = true;
        } else if (arg == L"--restart-audio") {
            options.restart_audio = true;
        } else if (arg == L"--dry-run") {
            options.dry_run = true;
        } else {
            out.Line(std::format(L"Unknown option: {}", arg));
            return std::nullopt;
        }
    }
    return options;
}

std::wstring EndpointName(const std::wstring& id) {
    for (const core::AudioEndpoint& endpoint : core::EnumerateCaptureEndpoints()) {
        if (text::EqualsIgnoreCase(endpoint.id, id)) {
            return endpoint.name;
        }
    }
    return L"(unknown or inactive device)";
}

std::optional<inst::EndpointId> ResolveEndpoint(const std::wstring& argument, Output& out) {
    std::wstring id = argument;
    if (id.empty() || text::EqualsIgnoreCase(id, L"default")) {
        const auto default_id = core::DefaultCaptureEndpointId();
        if (!default_id) {
            out.Line(L"No default microphone found.");
            return std::nullopt;
        }
        id = *default_id;
    }
    auto parsed = inst::ParseEndpointId(id);
    if (!parsed || parsed->flow != inst::Flow::Capture) {
        out.Line(L"Not a capture endpoint id: " + id + L" (see 'somike-apoctl list').");
        return std::nullopt;
    }
    return parsed;
}

// Exit code when ResolveEndpoint failed: no microphone at all is a failure of
// the environment (1), a malformed --endpoint argument is a usage error (2).
int UnresolvedEndpointExitCode(const std::wstring& argument) {
    return argument.empty() || text::EqualsIgnoreCase(argument, L"default") ? kExitFailure : kExitUsage;
}

std::wstring Join(const std::vector<std::wstring>& items) {
    std::wstring joined;
    for (const std::wstring& item : items) {
        joined += (joined.empty() ? L"" : L", ") + item;
    }
    return joined.empty() ? L"(none)" : joined;
}

std::wstring DescribeEdit(const inst::RegistryEdit& edit) {
    switch (edit.kind) {
        case inst::RegistryEdit::Kind::SetString:
            return L"set    " + edit.name + L" = " + edit.string_value;
        case inst::RegistryEdit::Kind::SetMultiString:
            return L"set    " + edit.name + L" = [" + Join(edit.multi_value) + L"]";
        case inst::RegistryEdit::Kind::Delete:
            return L"delete " + edit.name;
    }
    return {};
}

void FinishWithRestart(bool restart, Output& out) {
    if (!restart) {
        out.Line(L"Restart the Windows Audio service (or reboot) for the change to take effect,");
        out.Line(L"e.g. re-run with --restart-audio.");
        return;
    }
    out.Line(L"Restarting Windows Audio (all audio pauses briefly)...");
    inst::Report report;
    const HRESULT hr = inst::RestartAudioService(report);
    out.Lines(report);
    if (FAILED(hr)) {
        out.Line(L"Could not restart the audio service: " + win32::FormatHResult(hr) +
                 L". Please reboot.");
    }
}

// --- commands ------------------------------------------------------------------

int CommandList(Output& out) {
    const auto endpoints = core::EnumerateCaptureEndpoints();
    if (endpoints.empty()) {
        out.Line(L"No active microphones found.");
        return kExitOk;
    }
    for (const core::AudioEndpoint& endpoint : endpoints) {
        std::wstring state = L"?";
        bool effects_disabled = false;
        if (const auto id = inst::ParseEndpointId(endpoint.id)) {
            const inst::FxSnapshot snapshot = inst::ReadFxSnapshot(*id);
            state = inst::ToString(inst::QueryInstallState(snapshot));
            effects_disabled = snapshot.EffectsDisabled();
        }
        out.Line(std::format(L"{} {}{}", endpoint.is_default ? L"*" : L" ", endpoint.name,
                             endpoint.is_default_communications ? L"  [communications default]" : L""));
        out.Line(L"    id:     " + endpoint.id);
        out.Line(std::format(L"    somike: {}{}", state,
                             effects_disabled ? L" (audio enhancements disabled!)" : L""));
    }
    out.Line();
    out.Line(L"* = default microphone");
    return kExitOk;
}

int CommandStatus(const Options& options, Output& out) {
    const auto id = ResolveEndpoint(options.endpoint, out);
    if (!id) {
        return UnresolvedEndpointExitCode(options.endpoint);
    }
    const inst::FxSnapshot snapshot = inst::ReadFxSnapshot(*id);

    out.Line(L"Microphone:          " + EndpointName(id->full));
    out.Line(L"Endpoint id:         " + id->full);
    out.Line(std::format(L"Somike:              {}", inst::ToString(inst::QueryInstallState(snapshot))));
    out.Line(std::format(L"Audio enhancements:  {}", snapshot.EffectsDisabled() ? L"DISABLED" : L"enabled"));
    out.Line(L"Endpoint effect:     " + snapshot.String(inst::fx::kEndpointEffect).value_or(L"(none)"));
    out.Line(L"Composite EFX chain: " + Join(snapshot.MultiString(inst::fx::kCompositeEndpointEffect)));
    out.Line(L"Stream effect:       " + snapshot.String(inst::fx::kStreamEffect).value_or(L"(none)"));
    out.Line(L"Composite SFX chain: " + Join(snapshot.MultiString(inst::fx::kCompositeStreamEffect)));
    out.Line(std::format(L"Backup present:      {}", inst::HasBackup(*id) ? L"yes" : L"no"));
    out.Line(L"APO registration:    " + inst::RegisteredApoPath().value_or(L"(not registered)"));

    win32::MappedFile ring;
    const HRESULT hr = ring.Open(paths::RingFile(), protocol::kRegionBytes,
                                 win32::MappedFile::Access::ReadOnly);
    if (FAILED(hr)) {
        out.Line(L"Shared ring:         unavailable (" + win32::FormatHResult(hr) + L")");
        return kExitOk;
    }
    const dsp::RingSnapshot state = dsp::InspectRing(ring.bytes());
    if (!state.compatible) {
        out.Line(L"Shared ring:         present but incompatible/uninitialised");
        return kExitOk;
    }
    const ULONGLONG now = ::GetTickCount64();
    auto age = [now](std::uint64_t then) -> std::wstring {
        if (then == 0) return L"never";
        return std::format(L"{} ms ago", now > then ? now - then : 0);
    };
    out.Line(L"Shared ring:         " + paths::RingFile());
    out.Line(std::format(L"  producer (app):    {}, last seen {}",
                         state.producer_streaming ? L"streaming" : L"idle",
                         age(state.producer_heartbeat_ms)));
    out.Line(std::format(L"  consumer (APO):    last seen {} ({} Hz, {} ch)",
                         age(state.consumer.heartbeat_ms), state.consumer.sample_rate,
                         state.consumer.channels));
    out.Line(std::format(L"  frames injected:   {}  underruns: {}  resyncs: {}",
                         state.consumer.frames_injected, state.consumer.underruns,
                         state.consumer.resyncs));
    return kExitOk;
}

int CommandInstall(const Options& options, Output& out) {
    const auto id = ResolveEndpoint(options.endpoint, out);
    if (!id) {
        return UnresolvedEndpointExitCode(options.endpoint);
    }
    out.Line(L"Microphone: " + EndpointName(id->full));
    out.Line(L"Strategy:   " + std::wstring(inst::ToString(options.strategy)));

    const inst::FxSnapshot before = inst::ReadFxSnapshot(*id);
    const inst::InstallPlan plan = inst::PlanInstall(before, options.strategy);
    for (const std::wstring& note : plan.notes) {
        out.Line(L"- " + note);
    }
    if (plan.status == inst::PlanStatus::Refused) {
        return kExitRefused;
    }

    if (options.dry_run) {
        out.Line(L"Planned changes (dry run, nothing written):");
        for (const inst::RegistryEdit& edit : plan.fx_edits) {
            out.Line(L"  FxProperties: " + DescribeEdit(edit));
        }
        if (plan.enable_effects) {
            out.Line(L"  FxProperties: set    " + std::wstring(inst::fx::kDisableSysFx) + L" = 0");
        }
        return kExitOk;
    }

    if (!inst::IsProcessElevated()) {
        out.Line(L"install must run elevated (administrator).");
        return kExitNeedsElevation;
    }
    if (const HRESULT hr = inst::EnableInstallerPrivileges(); FAILED(hr)) {
        out.Line(L"Cannot enable backup/restore privileges: " + win32::FormatHResult(hr));
        return kExitFailure;
    }

    inst::Report report;
    std::wstring installed_dll;
    HRESULT hr = inst::DeployApoDll(core::ExecutableDirectory() + L"\\" + protocol::kApoDllName,
                                    installed_dll, report);
    if (SUCCEEDED(hr)) hr = inst::RegisterApo(installed_dll);
    if (SUCCEEDED(hr)) hr = inst::CreateRingFile(report);
    if (SUCCEEDED(hr)) hr = inst::ApplyInstallPlan(*id, before, plan, options.strategy, report);
    out.Lines(report);
    if (FAILED(hr)) {
        out.Line(L"Installation failed: " + win32::FormatHResult(hr));
        return kExitFailure;
    }

    switch (plan.status) {
        case inst::PlanStatus::AlreadyInstalled:
            out.Line(L"Somike was already installed; files and registration were refreshed.");
            break;
        case inst::PlanStatus::Upgrade:
            out.Line(L"Somike upgraded: it now runs as the endpoint effect, so every app hears it.");
            break;
        default:
            out.Line(L"Somike injector installed.");
            break;
    }
    FinishWithRestart(options.restart_audio, out);
    return kExitOk;
}

int CommandUninstall(const Options& options, Output& out) {
    if (!inst::IsProcessElevated()) {
        out.Line(L"uninstall must run elevated (administrator).");
        return kExitNeedsElevation;
    }
    if (const HRESULT hr = inst::EnableInstallerPrivileges(); FAILED(hr)) {
        out.Line(L"Cannot enable backup/restore privileges: " + win32::FormatHResult(hr));
        return kExitFailure;
    }

    std::vector<inst::EndpointId> targets;
    if (options.all) {
        targets = inst::EndpointsWithBackup();
        for (const core::AudioEndpoint& endpoint : core::EnumerateCaptureEndpoints()) {
            const auto id = inst::ParseEndpointId(endpoint.id);
            if (!id) continue;
            const bool known = std::any_of(targets.begin(), targets.end(),
                                           [&](const auto& t) { return t.guid == id->guid; });
            if (!known && inst::QueryInstallState(inst::ReadFxSnapshot(*id)) !=
                              inst::InstallState::NotInstalled) {
                targets.push_back(*id);
            }
        }
    } else if (const auto id = ResolveEndpoint(options.endpoint, out)) {
        targets.push_back(*id);
    } else {
        return UnresolvedEndpointExitCode(options.endpoint);
    }

    int exit_code = kExitOk;
    for (const inst::EndpointId& id : targets) {
        out.Line(L"Removing from: " + EndpointName(id.full) + L" (" + id.full + L")");
        inst::Report report;
        const HRESULT hr = inst::RestoreEndpoint(id, report);
        out.Lines(report);
        if (FAILED(hr)) {
            out.Line(L"  failed: " + win32::FormatHResult(hr));
            exit_code = kExitFailure;
        }
    }

    bool restart_pending = options.restart_audio;
    if (options.all) {
        // Restart first so audiodg.exe unloads the DLL before we delete it.
        if (restart_pending) {
            FinishWithRestart(true, out);
            restart_pending = false;
        }
        inst::Report report;
        if (const HRESULT hr = inst::UnregisterApo(); FAILED(hr)) {
            out.Line(L"Could not remove COM registration: " + win32::FormatHResult(hr));
            exit_code = kExitFailure;
        }
        const std::wstring dll = inst::ApoInstallDirectory() + L"\\" + protocol::kApoDllName;
        if (!::DeleteFileW(dll.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
            ::MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            report.push_back(L"APO DLL is in use; it will be deleted at the next reboot.");
        }
        (void)inst::DeleteRingFile(report);
        out.Lines(report);
    }

    out.Line(exit_code == kExitOk ? L"Somike injector removed." : L"Removal finished with errors.");
    if (restart_pending || !options.restart_audio) {
        FinishWithRestart(restart_pending, out);
    }
    return exit_code;
}

// Used by the setup program after replacing an APO DLL that audiodg.exe had
// loaded: the new version only runs once the audio engine restarts.
int CommandRestartAudio(Output& out) {
    if (!inst::IsProcessElevated()) {
        out.Line(L"restart-audio must run elevated (administrator).");
        return kExitNeedsElevation;
    }
    out.Line(L"Restarting Windows Audio (all audio pauses briefly)...");
    inst::Report report;
    const HRESULT hr = inst::RestartAudioService(report);
    out.Lines(report);
    if (FAILED(hr)) {
        out.Line(L"Could not restart the audio service: " + win32::FormatHResult(hr) + L". Please reboot.");
        return kExitFailure;
    }
    return kExitOk;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Output out;
    const core::ComInit com(COINIT_MULTITHREADED);

    const auto options = ParseArguments(argc, argv, out);
    if (!options) {
        PrintUsage(out);
        return kExitUsage;
    }

    int exit_code = kExitUsage;
    if (options->command == L"list") {
        exit_code = CommandList(out);
    } else if (options->command == L"status") {
        exit_code = CommandStatus(*options, out);
    } else if (options->command == L"install") {
        exit_code = CommandInstall(*options, out);
    } else if (options->command == L"uninstall") {
        exit_code = CommandUninstall(*options, out);
    } else if (options->command == L"restart-audio") {
        exit_code = CommandRestartAudio(out);
    } else {
        PrintUsage(out);
    }
    out.SaveReport(options->report_path);
    return exit_code;
}
