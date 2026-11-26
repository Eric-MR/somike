// Operations that change the system. All of them require an elevated
// (administrator) process; they are only called from somike-apoctl.exe.
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "somike/installer/endpoint_id.h"
#include "somike/installer/fx_config.h"
#include "somike/installer/install_plan.h"

namespace somike::installer {

// Human-readable progress/warning lines collected for the user.
using Report = std::vector<std::wstring>;

[[nodiscard]] bool IsProcessElevated() noexcept;

// Enables SeRestorePrivilege + SeBackupPrivilege (needed for MMDevices keys).
[[nodiscard]] HRESULT EnableInstallerPrivileges() noexcept;

// --- APO binary + COM registration ------------------------------------------
[[nodiscard]] std::wstring ApoInstallDirectory();  // %ProgramFiles%\Somike

// Copies the APO DLL to ApoInstallDirectory() (audiodg.exe cannot read user
// profile folders). Handles the case where the old DLL is currently loaded.
[[nodiscard]] HRESULT DeployApoDll(const std::wstring& source, std::wstring& installed_path,
                                   Report& report);
[[nodiscard]] HRESULT RegisterApo(const std::wstring& dll_path);
[[nodiscard]] HRESULT UnregisterApo();
[[nodiscard]] std::optional<std::wstring> RegisteredApoPath();

// --- Shared ring file --------------------------------------------------------
// Creates %ProgramData%\Somike\ring-v1.bin with a DACL that lets interactive
// users write and the audio service (LOCAL SERVICE / NT SERVICE\AudioSrv)
// read/write, and initialises its header.
[[nodiscard]] HRESULT CreateRingFile(Report& report);
[[nodiscard]] HRESULT DeleteRingFile(Report& report);

// --- Endpoint configuration --------------------------------------------------
// Backs up every value the plan touches, then applies it.
[[nodiscard]] HRESULT ApplyInstallPlan(const EndpointId& id, const FxSnapshot& before,
                                       const InstallPlan& plan, Strategy strategy,
                                       Report& report);

// Restores backed-up values that are still exactly as we wrote them, then
// removes any remaining Somike CLSID from the endpoint.
[[nodiscard]] HRESULT RestoreEndpoint(const EndpointId& id, Report& report);

[[nodiscard]] bool HasBackup(const EndpointId& id);
[[nodiscard]] std::vector<EndpointId> EndpointsWithBackup();

// --- Audio service -------------------------------------------------------------
// Restarts "Windows Audio" (Audiosrv) so endpoint effect changes take effect.
[[nodiscard]] HRESULT RestartAudioService(Report& report);

}  // namespace somike::installer
