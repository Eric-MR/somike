# Building, testing and debugging

## Prerequisites

- Windows 11 x64 (or Windows 10 build 20348+)
- Visual Studio 2022/2026 **or** Build Tools with the *Desktop development
  with C++* workload (MSVC v14.3x+, Windows SDK 10.0.22621+; developed with
  MSVC 14.52 and SDK 10.0.26100)
- CMake 3.25+ and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`)
- Network access on first configure (GoogleTest is fetched and hash-checked)

## Build

From any PowerShell (the script imports the MSVC environment itself):

```powershell
./scripts/build.ps1                    # Debug, build + tests
./scripts/build.ps1 -Preset release    # Release
./scripts/build.ps1 -Preset ci         # Release, warnings as errors (as in CI)
./scripts/build.ps1 -NoTests -Clean
```

Or from a *Developer PowerShell for VS*:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Output: `build/<preset>/bin/` holds `somike.exe`, `somike-apoctl.exe` and
`somike_apo.dll` side by side. `cmake --install build/release` produces a
portable folder.

## Smart App Control / WDAC

If Smart App Control is in *enforce* mode (check
`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`,
where `1` = enforce), Windows refuses to run unsigned binaries. That includes
`somike_tests.exe` (so `ctest` fails with "Error running test executable") and
the APO inside `audiodg.exe`. Options: build and test in CI (GitHub-hosted
runners don't enforce SAC), sign your builds, or develop on a machine/VM
without SAC. Code Integrity events are logged under
*Applications and Services Logs > Microsoft > Windows > CodeIntegrity*.

## Trying the injector

```powershell
cd build/release/bin
./somike-apoctl list                          # find your microphone
./somike-apoctl install --dry-run             # see what would change
# elevated prompt:
./somike-apoctl install --restart-audio
./somike-apoctl status                        # "consumer (APO): last seen N ms ago"
```

The consumer heartbeat only updates while some application has the
microphone open (e.g. Windows Sound settings > microphone test, or Discord).
Undo everything with `somike-apoctl uninstall --all --restart-audio`.

## Debugging the APO

- The APO logs non-real-time events (`Initialize`, `LockForProcess`, mapping
  failures) with `OutputDebugString`. View them with Sysinternals
  **DebugView** (*Capture Global Win32* enabled, run as admin).
- To debug interactively, attach an elevated debugger to `audiodg.exe` *after*
  starting a capture stream. Breakpoints in `APOProcess` stall all audio.
- If Windows switched effects off after APO failures, `somike-apoctl status`
  shows "Audio enhancements: DISABLED"; `install` re-enables them.
- `somike.exe` logs to `%LOCALAPPDATA%\Somike\somike.log`.
