# Somike

[![CI](https://github.com/Eric-MR/somike/actions/workflows/ci.yml/badge.svg)](https://github.com/Eric-MR/somike/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Eric-MR/somike?include_prereleases&sort=semver&label=release)](https://github.com/Eric-MR/somike/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Windows 10/11 x64](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4)

Route the audio of any Windows application into your **real, physical
microphone**, so Discord, games and other voice chats receive it as mic
input. No virtual audio device is created, and the microphone selection in
your other apps stays exactly as it is.

> [!WARNING]
> **Beta, not finished.** Somike is in public beta (`0.2.0-beta.1`) for
> testing and feedback. The full pipeline works end to end on real hardware
> (Windows 11 25H2, USB headset microphone; Spotify into Discord), but expect
> rough edges, and the binaries are not code-signed yet.
> See [Limitations](#limitations) and please
> [report what you find](https://github.com/Eric-MR/somike/issues/new/choose).

## How it works

```
 Source app (Spotify, browser, game...)
        |  WASAPI process loopback (per app, or everything)
        v
 somike.exe  --writes-->  %ProgramData%\Somike\ring-v1.bin  (shared ring)
                                         |
                                         v reads
 Physical mic --> audiodg.exe [ ... -> somike_apo.dll (capture effect) ] --> Discord, games, ...
```

1. **Capture**: `somike.exe` records the selected application's output with
   the WASAPI *process loopback* API (Windows 11 / Windows 10 build 20348+).
2. **Transport**: audio goes into a lock-free ring buffer in a shared
   memory-mapped file.
3. **Inject**: a small *Audio Processing Object* (APO), registered as the
   **endpoint effect** of your existing microphone, runs inside the Windows
   audio engine. It mixes the ring audio into the microphone signal before
   Windows hands it to the apps, so every app that records from the mic
   receives the mix, including apps that record in *raw* mode such as Discord.

The full investigation, including why this works without a virtual device and
what the trade-offs are, is in [docs/feasibility.md](docs/feasibility.md).

## Quick start

Requirements: Windows 11 (or Windows 10 build 20348+), x64.

1. Download `somike-<version>-win-x64.zip` from the
   [latest release](https://github.com/Eric-MR/somike/releases), extract it
   anywhere and start `somike.exe`. The binaries are not code-signed yet, so
   SmartScreen may warn ("More info → Run anyway"); with *Smart App Control*
   on, Windows blocks them (see [Limitations](#limitations)).
   The first time, it asks to **Set up** your microphone
   (one click + the Windows permission prompt; sound pauses for a second).
2. Click the app you want to hear in your mic. Apps that are playing show up
   automatically with their icon; *All system audio* is always there.
3. Press **Go live** (or **Ctrl+Alt+M** anywhere, e.g. in a game).

It is plug and play: if the chosen app is not running yet, Somike shows
*Waiting* and connects the moment it starts; if the app or Windows Audio
restarts, it reconnects by itself. Volume, "mute my voice" and the microphone
are on the same screen; your choices are remembered.

![Somike live with Spotify](docs/images/app-live.png)

To remove Somike from the microphone again, choose **Remove Somike from this
microphone** in the **⋯** menu, then delete the folder.

The command-line tool covers the same and more:

```powershell
somike-apoctl list                      # microphones + injector state
somike-apoctl status                    # detailed diagnostics (live APO heartbeat)
somike-apoctl install --dry-run         # show exactly what would change
somike-apoctl install --restart-audio   # elevated
somike-apoctl uninstall --all --restart-audio
somike-selftest --modes                 # end-to-end check on the default mic (plays a test tone)
```

## Building from source

Visual Studio 2022/2026 or the Build Tools with the C++ workload, CMake 3.25+
and Ninja:

```powershell
./scripts/build.ps1 -Preset release        # builds + runs unit tests
```

The binaries end up in `build/release/bin`. Details, debugging the APO and
the release process: [docs/building.md](docs/building.md).

## Repository layout

| Path | What |
|------|------|
| `src/protocol` | Header-only shared-memory layout and APO identity (single source of truth) |
| `src/dsp` | Portable, unit-tested core: ring writer/reader, resampler, mixer, `Injector` |
| `src/common` | Small Win32 helpers (handles, registry, mapping, logging) |
| `src/installer` | Endpoint inspection, install planning, elevated system changes with backup |
| `src/core` | App engine: device/session discovery, loopback capture, router, settings |
| `src/apo` | `somike_apo.dll`, the capture APO (thin COM shell around `dsp::Injector`) |
| `src/apoctl` | `somike-apoctl.exe`, CLI installer and diagnostics |
| `src/app` | `somike.exe`: Win32 window + WebView2 hosting the HTML UI in `src/app/ui` |
| `tools` | `somike-selftest` (hardware check), `ui-dev` (UI preview server, app driver, icon) |
| `tests` | GoogleTest unit tests |
| `docs` | Feasibility study, architecture, IPC spec, build guide, ADRs |
| `.github` | CI and release workflows, issue and pull-request templates |

## Documentation

- [Feasibility study](docs/feasibility.md): the technical investigation and alternatives
- [Architecture](docs/architecture.md): components, threading, failure model, roadmap
- [IPC protocol](docs/ipc-protocol.md): the shared ring buffer specification
- [Building & debugging](docs/building.md)
- [Architecture decision records](docs/adr)
- [Changelog](CHANGELOG.md)

## Contributing and feedback

Bug reports, [compatibility reports](https://github.com/Eric-MR/somike/issues/new?template=compatibility.yml)
(which microphones and apps work) and pull requests are welcome; see
[CONTRIBUTING.md](CONTRIBUTING.md) and the [code of conduct](CODE_OF_CONDUCT.md).
Please report security problems privately as described in
[SECURITY.md](SECURITY.md).

## Limitations

- **Admin once**: registering an effect on a microphone needs administrator
  rights and an audio-service restart. Routing itself runs unelevated.
- **Code signing**: with *Smart App Control* or WDAC enforcing, Windows blocks
  unsigned binaries, including the APO inside `audiodg.exe`. Release builds
  must be signed.
- **Exclusive-mode streams** bypass all effects, so apps that open the mic
  that way (rare for voice apps) don't get the routed audio. Raw-mode capture,
  which Discord uses, is covered.
- **Upgrading from 0.1.0** (pre-release builds only): that version registered
  as a stream effect, which raw-mode apps skip. The app shows **Update**; one
  click (and the Windows prompt) moves it.
- **Driver updates** may reset the microphone's effect configuration; the app
  detects this and offers **Repair**.
- Apps' own noise suppression (Discord, Teams...) may filter music; turn it
  down for best results.
- Choosing *All system audio* while in a voice chat feeds the other
  participants' voices back to them; prefer routing a single app.

## License

[MIT](LICENSE)
