# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0-beta.1] - 2026-11-27

First public pre-release. **Beta: not finished**, made for testing and
feedback. Validated on real hardware (Windows 11 build 26200, Corsair HS80
USB microphone) with Spotify as the source and Discord as the receiver.
Binaries are not code-signed yet.

### Fixed
- **Discord did not hear the routed audio.** Discord records in raw mode, and
  Windows skips stream effects (SFX) for raw streams. Somike now installs as
  the microphone's endpoint effect (EFX), which every capture stream passes
  (default, communications, raw). `install` / the app's **Update** button
  upgrades existing installations and keeps the original backup. See
  ADR 0005.
- APO: support COM aggregation. The audio engine creates every APO with
  `CreateInstance(outer, IID_IUnknown)`; refusing it made Windows switch off
  "Audio enhancements" for the microphone.
- APO: implement `IAudioSystemEffects3` (probed by Windows 11) and accept the
  `APOInitSystemEffects3` init structure.
- Installer/status: `PKEY_AudioEndpoint_Disable_SysFx` lives in
  `FxProperties`, not `Properties`; disabled enhancements were not detected or
  re-enabled.
- APO: no `OutputDebugString` inside `audiodg.exe`: an attached debug-output
  listener blocks each call, which Windows treats as a hung effect.

### Changed
- New desktop app UI (replaces the Win32 control window): HTML/CSS UI in
  WebView2, dark design inspired by Voicemod and T3 Code. One screen: app tiles
  with real icons, a big **Go live** button with level meter, volume,
  "mute my voice", microphone; a guided one-click first-time setup.
- Plug and play: apps are grouped per executable and captured from the root of
  their process tree (Spotify, Chrome, Discord are multi-process); live routing
  waits for an app that is not running yet and re-attaches when the app or
  Windows Audio restarts; apps seen earlier stay listed while running; first
  run preselects whatever is playing; app names come from version resources.

### Added
- `somike.exe --source <app.exe|system> --live` for shortcuts; window title and
  Ctrl+Alt+M reflect/toggle the live state from anywhere.
- `somike-selftest.exe`: hardware end-to-end check (plays a tone, routes it,
  records the microphone, measures level and residual per phase); `--listen`
  and `--tone` modes for real-app scenarios; `--modes` checks every capture
  processing mode (default, communications, raw) and `--listen --mode raw`
  records like Discord.
- UI tooling: `tools/ui-dev/serve.mjs` (browser preview with mock backend),
  `drive.mjs` (drives the real window via the WebView2 DevTools port),
  `make-icon.ps1` (app icon).
- Tests for process-tree resolution and the UI JSON contract.
- APO diagnostics log `%ProgramData%\Somike\apo.log` (created by the installer,
  written only by `audiodg.exe`, capped at 1 MB).
- COM contract tests for `somike_apo.dll` (aggregation, `IAudioSystemEffects3`,
  `Initialize`).
- Open-source project setup: MIT license, security policy, code of conduct,
  issue and pull-request templates, Dependabot for GitHub Actions.
- Release workflow: a pushed `v*` tag builds, tests and publishes the portable
  zip (with `somike-selftest.exe`), a symbols zip and `SHA256SUMS.txt` with
  build provenance attestations; versions with a suffix become pre-releases.

## [0.1.0] - 2026-11-26

Internal foundation; never published.

### Added
- Feasibility study: injecting into the physical microphone through a capture
  stream-effect APO, without a virtual device.
- `somike_apo.dll`: capture SFX APO (in place, float32, any rate and channel
  count) that mixes routed audio into microphone streams and falls back to
  pass-through on any error.
- Shared-memory ring protocol v1 with per-consumer read positions, priming,
  overrun/underrun handling, heartbeat timeout and diagnostics block.
- Drift-compensating linear resampler and channel-mapping mixer.
- `somike-apoctl.exe`: `list`, `status`, `install` (auto/composite/replace
  strategies, dry run, lossless registry backup), `uninstall`.
- `somike.exe`: Win32 UI with source/microphone selection, volume, "mute my
  voice", start/stop button, global hotkey Ctrl+Alt+M, live injector status.
- CMake presets, build script, GitHub Actions CI, unit tests.

[Unreleased]: https://github.com/Eric-MR/somike/compare/v0.2.0-beta.1...HEAD
[0.2.0-beta.1]: https://github.com/Eric-MR/somike/releases/tag/v0.2.0-beta.1
