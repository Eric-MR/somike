# Somike

Route the audio of any Windows application into your **real, physical
microphone**, so Discord, games and other voice chats receive it as mic
input. No virtual audio device is created, and the microphone selection in
your other apps stays exactly as it is.

> **Status: technical foundation (v0.1.0).** The full pipeline is implemented
> and builds cleanly, but has not been validated end-to-end on real hardware
> yet. Binaries are unsigned. See [Limitations](#limitations).

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
3. **Inject**: a small *Audio Processing Object* (APO), registered as a
   capture **stream effect** on your existing microphone, runs inside the
   Windows audio engine. It mixes the ring audio into every capture stream of
   that microphone. Any app that records from the mic receives the mix.

The full investigation, including why this works without a virtual device and
what the trade-offs are, is in [docs/feasibility.md](docs/feasibility.md).

## Quick start

Requirements: Windows 11 (or Windows 10 build 20348+) x64, and Visual Studio
2022/2026 or the Build Tools with the C++ workload, CMake 3.25+ and Ninja.

```powershell
./scripts/build.ps1 -Preset release        # builds + runs unit tests
```

Then, from `build/release/bin`:

1. Start `somike.exe`.
2. Pick your microphone and click **Install...** (one-time, needs admin;
   the Windows Audio service restarts briefly).
3. Pick an audio source (an app, or *All system audio*) and click **Start
   routing**, or press **Ctrl+Alt+M** anywhere.

The command-line tool covers the same and more:

```powershell
somike-apoctl list                      # microphones + injector state
somike-apoctl status                    # detailed diagnostics (live APO heartbeat)
somike-apoctl install --dry-run         # show exactly what would change
somike-apoctl install --restart-audio   # elevated
somike-apoctl uninstall --all --restart-audio
```

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
| `src/app` | `somike.exe`, the Win32 desktop UI |
| `tests` | GoogleTest unit tests |
| `docs` | Feasibility study, architecture, IPC spec, build guide, ADRs |

## Documentation

- [Feasibility study](docs/feasibility.md): the technical investigation and alternatives
- [Architecture](docs/architecture.md): components, threading, failure model, roadmap
- [IPC protocol](docs/ipc-protocol.md): the shared ring buffer specification
- [Building & debugging](docs/building.md)
- [Architecture decision records](docs/adr)
- [Contributing](CONTRIBUTING.md)

## Limitations

- **Admin once**: registering an effect on a microphone needs administrator
  rights and an audio-service restart. Routing itself runs unelevated.
- **Code signing**: with *Smart App Control* or WDAC enforcing, Windows blocks
  unsigned binaries, including the APO inside `audiodg.exe`. Release builds
  must be signed.
- **RAW/exclusive streams** bypass all effects, so apps that open the mic that
  way don't get the routed audio.
- **Driver updates** may reset the microphone's effect configuration; the app
  detects this and offers **Repair**.
- Apps' own noise suppression (Discord, Teams...) may filter music; turn it
  down for best results.
- Choosing *All system audio* while in a voice chat feeds the other
  participants' voices back to them; prefer routing a single app.

## License

No license has been chosen yet. Until one is added, all rights are reserved
by the author.
