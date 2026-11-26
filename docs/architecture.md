# Architecture

## Processes and data flow

```
+--------------------------- user session -----------------------------+
| somike.exe (unelevated)                                              |
|   MainWindow (Win32 view)                                            |
|     -> AppController (UI-free app logic, settings)                   |
|          -> core::Router                                             |
|               |- core::LoopbackCapture  <- WASAPI process loopback   |
|               |     (MMCSS "Audio" thread, 48 kHz stereo float)      |
|               '- dsp::RingWriter ----------------+                   |
+---------------------------------------------------|------------------+
                                                    v
                      %ProgramData%\Somike\ring-v1.bin  (memory-mapped)
                                                    ^
+------------------- audiodg.exe (Windows Audio) ---|------------------+
| per capture stream of the microphone:             |                  |
|   driver -> EFX -> MFX -> SFX: somike_apo.dll     |                  |
|                              InjectorApo (COM)    |                  |
|                                -> dsp::Injector   |                  |
|                                     |- dsp::RingReader  -------------+
|                                     |- dsp::LinearResampler (rate + drift)
|                                     '- dsp::MixInto (channels, gains)
+----------------------------------------------------------------------+

somike-apoctl.exe (elevated via UAC, one-time):
  deploy DLL to %ProgramFiles%\Somike, register COM/APO, create ring file
  with DACL, edit FxProperties with backup, restart Audiosrv.
```

## Modules

| Library / binary | Responsibility | Depends on |
|---|---|---|
| `somike::protocol` (header-only) | Shared-memory layout, constants, APO identity | - |
| `somike::dsp` | Ring writer/reader, resampler, mixer, `Injector`. **No Win32**, fully unit-tested | protocol |
| `somike::common` | RAII handles, HRESULT text, registry, file mapping, paths, logging. No shell32/user32 (linked into the APO) | protocol |
| `somike::installer` | Endpoint IDs, FxProperties snapshot, **pure install planner**, elevated operations with backup/restore | common, dsp |
| `somike::core` | Device + session enumeration, process-loopback capture, `Router`, settings, injector status, elevated-tool launcher | common, dsp, installer |
| `somike_apo.dll` | COM APO shell around `dsp::Injector` | dsp, common |
| `somike-apoctl.exe` | CLI: list/status/install/uninstall | core, installer |
| `somike.exe` | Win32 UI: `MainWindow` (view) + `AppController` (logic) | core |

Rules of thumb: logic goes into the lowest layer that can hold it, and pure
logic (planning, DSP, protocol, settings codec) is separated from I/O so it
can be tested without hardware or administrator rights.

## Threading and real-time rules

- **APO (`APOProcess`)**: real-time thread of the audio engine. No allocation,
  locks, logging, system calls (except `GetTickCount64`) or exceptions.
  Buffers are allocated in `LockForProcess`, and the file is mapped there too.
- **Capture thread** (`LoopbackCapture`): event-driven WASAPI loop on an MMCSS
  "Audio" thread; writes to the ring and heartbeats every <=100 ms.
- **UI thread**: owns `AppController`; `Router` start/stop/gain calls are
  serialised by a mutex. The elevated installer runs on a worker thread and
  reports back with a window message.
- **Cross-process**: one producer, any number of consumers. Each APO instance
  (one per capture stream) keeps its own read position, so no consumer ever
  writes control data. See [ipc-protocol.md](ipc-protocol.md).

## Failure model: "never break the microphone"

| Situation | Behaviour |
|---|---|
| Ring file missing / unmappable / incompatible version | APO passes the mic through |
| App not routing, crashed, or hung (heartbeat > 500 ms) | pass-through at full mic gain (a crash while "mute my voice" was on cannot leave the mic muted) |
| Ring runs dry | silence padding, re-prime to the target latency |
| Consumer far behind (> 150 ms) | jump to the target latency (40 ms) |
| Clock drift between source and microphone | proportional controller nudges the resampling ratio (max +-0.5 %) |
| Garbage in shared memory | indices masked with compile-time constants, gains sanitised, output clamped and NaN-free |
| Unsupported format | APO still locks and passes through (failing would count towards Windows disabling effects) |

## Extension points (roadmap)

- **Code signing + installer** (MSI/WiX): required for Smart App Control
  machines; the portable `cmake --install` layout already matches.
- **Tray icon / start with Windows**: `AppController` is UI-free, so a tray
  front-end is additive.
- **Multiple sources mixed**: several `LoopbackCapture` instances feeding a
  mixer before the `RingWriter`.
- **Better resampler**: replace `LinearResampler` (same interface) with a
  polyphase/windowed-sinc implementation.
- **Wrap OEM effects**: load the original driver APO inside `somike_apo.dll`
  and process it first (Equalizer APO's approach) instead of composite chains.
- **Per-endpoint diagnostics / per-consumer targeting**: protocol v2.
- **Virtual-device fallback**: a different consumer of the same ring and
  `dsp::Injector` (see feasibility study, section 5).
