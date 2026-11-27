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
portable folder (the same content as the release zip).

## Releases

Releases are built by GitHub Actions
([`.github/workflows/release.yml`](../.github/workflows/release.yml)) when a
tag `v<version>` is pushed:

1. Set the version in `CMakeLists.txt` (`project(... VERSION x.y.z)` and
   `SOMIKE_VERSION_SUFFIX`, e.g. `-beta.2`, or `""` for a final release).
2. Move the *Unreleased* entries in `CHANGELOG.md` under a new
   `## [x.y.z-suffix] - YYYY-MM-DD` heading and update the links at the end.
3. Commit, then tag and push: `git tag -a v0.2.0-beta.2 -m "Somike 0.2.0-beta.2"`
   and `git push origin main v0.2.0-beta.2`.

The workflow refuses a tag that differs from the CMake version, builds the
`ci` preset, runs the tests, and publishes the zip, a symbols zip (PDBs) and
`SHA256SUMS.txt` with build provenance attestations. Versions with a suffix
(`-beta.N`, `-rc.N`) are published as **pre-releases**; the release notes are
the matching `CHANGELOG.md` section.

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

- `somike-selftest` (in the build output and the release zip) checks the whole
  chain on the default microphone: it plays a quiet 1 kHz tone, routes it,
  records the microphone and prints the tone level per phase. Expect
  `RESULT: PASS` with the tone at about -26 dBFS while routing.
- `somike-selftest --modes` records once per capture processing mode
  (default, communications, raw, communications+raw). Voice apps differ here:
  Discord records in raw mode. Every mode must show the tone; if raw fails,
  Somike is still registered as a stream effect (run `install` to upgrade).
  `--listen <s> --mode raw` listens like such an app.
- The APO logs non-real-time events (instance creation, `Initialize`, format
  negotiation, `LockForProcess`, ring mapping) to `%ProgramData%\Somike\apo.log`.
  It deliberately does **not** use `OutputDebugString` inside `audiodg.exe`:
  with a listener such as DebugView attached, each call can block for seconds,
  Windows then treats the effect as hung and switches "Audio enhancements" off.
- To debug interactively, attach an elevated debugger to `audiodg.exe` *after*
  starting a capture stream. Breakpoints in `APOProcess` stall all audio.
- If Windows switched effects off after APO failures, `somike-apoctl status`
  shows "Audio enhancements: DISABLED"; `install` re-enables them.
- `somike.exe` logs to `%LOCALAPPDATA%\Somike\somike.log`.

## Working on the UI

The UI is plain HTML/CSS/JS in `src/app/ui`, embedded into `somike.exe` as
resources. Two ways to iterate quickly:

- **In a browser, with a mock backend**: `node tools/ui-dev/serve.mjs`, then
  open `http://localhost:5799/?scenario=ready` (also `setup`, `live`,
  `waiting`, `enhancements`, `empty`; add `&still` for screenshots).
- **In the real app, without rebuilding**:
  `somike.exe --ui-dir <repo>\src\app\ui` loads the files from disk (reload
  with F5; DevTools enabled).

Automated UI tests drive the real window through the WebView2 DevTools port:

```powershell
somike.exe --remote-debugging-port 9222 --source spotify.exe
node tools/ui-dev/drive.mjs 9222 click "#live-btn"
node tools/ui-dev/drive.mjs 9222 wait "state.live.phase === 'live'"
node tools/ui-dev/drive.mjs 9222 screenshot live.png
somike-selftest --listen 5    # what a voice app hears from the microphone
somike-selftest --listen 5 --mode raw   # ...an app recording in raw mode (Discord)
somike-selftest --tone 30     # a stand-in "app" playing a 1 kHz tone
```

`tools/ui-dev/make-icon.ps1` regenerates `src/app/somike.ico`.
