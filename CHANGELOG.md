# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-11-26

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
