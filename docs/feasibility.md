# Feasibility study: feeding application audio into the *physical* microphone

**Question.** Can Windows route the audio output of an arbitrary application
into the existing microphone input, so that every app recording from that
microphone (Discord, games, browsers...) receives it, without creating a
virtual audio device and without users changing their microphone settings?

**Answer: yes**, through a capture-side *system effect Audio Processing
Object (APO)* registered on the physical microphone's endpoint. It comes with
a set of real limitations (below), the most important being one-time
administrator rights, code signing for distribution, and that apps opening
the microphone in exclusive mode are not reached. (Raw mode is reached
because the APO is the *endpoint* effect; see the table in section 4.)

## 1. Where audio can be intercepted

```
mic hardware -> driver (kernel, KS) -> audiodg.exe (Windows audio engine)          -> WASAPI -> app
                                        EFX (endpoint) -> MFX (mode) -> SFX (stream)
                                        ^ system effect APOs: user-mode COM DLLs
```

Every shared-mode capture stream passes through the audio engine
(`audiodg.exe`), which runs the *system effect* APOs configured for that
endpoint. The configuration lives in the endpoint's registry key
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{endpoint}\FxProperties`.
An APO listed there is loaded by `audiodg.exe` and sees, and may modify,
the audio of **every** capture stream of that microphone, after the driver
and before the application. This is the only *user-mode*, *per-device*,
*all-applications* hook into the capture path, and it doesn't touch the
device list.

Equalizer APO uses exactly this mechanism to process real microphones
system-wide (its device selector supports capture devices), which shows it
works in practice on Windows 10/11.

## 2. Options evaluated

| # | Approach | Apps keep their mic setting | Admin | Signing | Main risks | Verdict |
|---|----------|:---:|:---:|---|---|---|
| 1 | **Capture system-effect APO (endpoint effect) on the physical endpoint** | yes | once | Authenticode for SAC/WDAC machines | unsupported config route, exclusive mode bypass, driver updates reset it | **Chosen** |
| 2 | Virtual audio device (virtual cable / custom driver) | no (must pick the new device, or it becomes default) | install | kernel driver: attestation signing (EV cert + Partner Center) | kernel code, user reconfiguration | Fallback |
| 3 | Kernel filter driver on the mic stack | yes | install | kernel signing | BSODs, enormous effort | Rejected |
| 4 | DLL injection / API hooking into Discord, games... | yes | no | - | breaks with app updates, **anti-cheat bans**, per-app work | Rejected |
| 5 | Windows 11 AEC "auxiliary input" (`IApoAuxiliaryInputConfiguration`) | yes | once | as #1 | only delivers the render *loopback reference* to AEC mode effects; no arbitrary/per-app source | Not applicable |

## 3. The chosen design in one paragraph

`somike.exe` captures the selected app with the **WASAPI process-loopback**
API (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, include or exclude a
process tree) and writes canonical 48 kHz stereo float audio into a ring
buffer in a shared memory-mapped file. `somike_apo.dll`, registered as the
microphone's **endpoint effect (EFX)**, maps the same file inside
`audiodg.exe`, converts the audio to the microphone's format (rate, channels,
clock drift), and mixes it into the microphone signal in place, before the
audio engine splits it into the default, communications and raw streams the
apps receive. If anything is
missing or stale, it passes the microphone through unchanged. Details:
[architecture.md](architecture.md), [ipc-protocol.md](ipc-protocol.md).

## 4. Limitations and how the foundation handles them

| Limitation | Consequence | Mitigation in Somike |
|---|---|---|
| `MMDevices` keys are owned by TrustedInstaller | even admins cannot write FxProperties normally | the elevated installer enables `SeRestorePrivilege` and opens keys with `REG_OPTION_BACKUP_RESTORE`: no ownership/ACL changes left behind |
| Effects load when the audio graph is built | install/uninstall needs an audio-service restart (or reboot) | `--restart-audio`; the UI asks first |
| Writing FxProperties directly is **not an officially supported** installation route (drivers are expected to declare APOs in their INF) | driver reinstall/update can overwrite the configuration | lossless backup of every touched value; status check in the UI; **Repair** button |
| Existing driver (OEM) endpoint effects, e.g. array beamforming | replacing them would degrade the mic | `auto` strategy never displaces them; `composite` chains after them (Win10 1803+ composite keys, interaction with the single key undocumented, so marked experimental); `replace` only on explicit request, restored on uninstall. OEM stream/mode effects are not touched |
| Legacy LFX/GFX-only devices | adding a modern effect makes Windows ignore LFX/GFX | `auto` refuses; explicit strategy required |
| "Audio enhancements" switched off (`PKEY_AudioEndpoint_Disable_SysFx`, stored in `FxProperties`) | no APO runs at all | detected; install re-enables it (and backs up the old value) |
| Windows disables system effects after APO failures or hangs | one buggy APO can switch off all enhancements of the device; **observed**: Windows set `Disable_SysFx = 1` within seconds when the APO refused aggregation, and when it blocked in `OutputDebugString` | the APO supports aggregation and `IAudioSystemEffects3`, never blocks or fails for tolerable conditions, treats shared memory as untrusted, and passes through on any error |
| **RAW mode** capture | Windows skips stream (SFX) and mode (MFX) effects for raw streams. **Observed**: Discord's voice engine records in raw mode, so version 0.1.0 (a stream effect) was silent in Discord | Somike registers as the **endpoint effect**, which Microsoft documents as "always applied, even to raw streams"; verified with `somike-selftest --modes` (default, communications, raw, communications+raw all receive the tone). 0.1.0 installations are upgraded by `install` / **Update** |
| Exclusive-mode capture | no effects at all | inherent; documented. Voice apps use shared mode |
| App-side processing (noise suppression, AGC) | music may be filtered after injection | user setting in the consuming app |
| **Code integrity: Smart App Control / WDAC** | unsigned binaries are blocked, including DLLs loaded into `audiodg.exe`. Observed on the development machine, where SAC blocked the freshly built executables | sign release builds (Authenticode via Azure Trusted Signing or an OV/EV certificate). Per Microsoft, an unsigned APO also fails to load when the graph runs protected (DRM) content |
| Process loopback needs build 20348+ | Windows 10 (last build 19045) cannot capture per app; in practice Windows 11 only (or Windows Server 2022+) | documented; the setup program refuses older builds |
| ARM64 Windows | the audio engine is native ARM64 and cannot load an x64 APO | the setup program refuses ARM64; an ARM64(EC) APO build is a future option |
| *All system audio* includes voice-chat output | other participants hear themselves | per-app routing is the default recommendation; UI wording warns |
| Diagnostics block is shared by all APO instances | status shows "the injector is alive somewhere", not per microphone | sufficient for v0.1; per-endpoint slots are a protocol v2 candidate |

## 5. If the APO route is not available: the best alternative

On machines where effects on the physical microphone are impossible (managed
devices with policies that forbid it, apps that capture in exclusive mode, or no
code-signing certificate for SAC machines), the most robust alternative is a
**virtual microphone that mirrors the physical one plus the injected audio**
and is set as the **default (communications) capture device**. Apps that use
"Default" follow it automatically; apps with an explicit device must be
switched once. It needs a signed kernel-mode audio driver (attestation
signing), which is substantially more effort.

The architecture keeps this option open: capture, ring transport and
injection logic (`dsp::Injector`) are independent of the APO shell, so a
virtual-device consumer could reuse them unchanged.

## 6. Sources

- Microsoft Learn: [Windows Audio Processing Objects](https://learn.microsoft.com/windows-hardware/drivers/audio/windows-audio-processing-objects),
  [Implementing Audio Processing Objects](https://learn.microsoft.com/windows-hardware/drivers/audio/implementing-audio-processing-objects),
  [Windows 11 APIs for APOs](https://learn.microsoft.com/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects),
  [Audio signal processing modes](https://learn.microsoft.com/windows-hardware/drivers/audio/audio-signal-processing-modes),
  [PKEY_FX_StreamEffectClsid](https://learn.microsoft.com/windows-hardware/drivers/audio/pkey-fx-streameffectclsid),
  [PKEY_CompositeFX_StreamEffectClsid](https://learn.microsoft.com/windows-hardware/drivers/audio/pkey-compositefx-streameffectclsid)
- Microsoft Learn: [AUDIOCLIENT_ACTIVATION_TYPE](https://learn.microsoft.com/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-audioclient_activation_type),
  [ActivateAudioInterfaceAsync](https://learn.microsoft.com/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync)
- Windows SDK 10.0.26100.0 headers `audioenginebaseapo.h`, `mmdeviceapi.h`, `ksmedia.h` (property keys and mode GUIDs used verbatim)
- Etienne Dechamps, [notes on Windows APOs](https://github.com/dechamps/apo) (FxProperties layout, TrustedInstaller ownership, `NT SERVICE\AudioSrv` file access)
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) and its issue tracker (capture-device support, "enhancements must be enabled", service-restart side effects)
