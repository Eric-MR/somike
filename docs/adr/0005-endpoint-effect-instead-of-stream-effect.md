# ADR 0005: Install as the endpoint effect (EFX), not the stream effect (SFX)

- Status: accepted (amends [ADR 0001](0001-capture-apo-instead-of-virtual-device.md))
- Date: 2026-11-27

## Context

Version 0.1.0 registered `somike_apo.dll` as the microphone's stream effect
(`PKEY_FX_StreamEffectClsid`). On real hardware (Windows 11 build 26200,
Corsair HS80) routing worked for apps that open the microphone in the default
or communications processing mode, but **Discord received nothing**:

- Discord had an active capture session on the microphone, yet the APO log
  showed no `LockForProcess` for it: its stream never contained the effect.
- `somike-selftest --modes` reproduced it: the routed tone arrived at
  -26 dBFS in the default and communications modes and was absent
  (about -115 dBFS) in raw and communications+raw. The microphone reports
  `System.Devices.AudioDevice.RawProcessingSupported = true`.

Windows skips stream effects for raw-mode streams. Voice apps that do their
own processing (Discord's noise suppression and echo cancellation) request
raw mode to avoid OEM processing.

Microsoft's APO architecture documentation: "Endpoint effects (EFX) are
applied to all streams that use the same endpoint. An endpoint effect is
always applied, even to raw streams." On capture, the EFX runs before the
audio engine splits the signal into the processing modes.

## Decision

Register Somike as the endpoint effect (`PKEY_FX_EndpointEffectClsid`, with
`PKEY_EFX_ProcessingModes_Supported_For_Streaming = DEFAULT`), or append it
to `PKEY_CompositeFX_EndpointEffectClsid` with the `composite` strategy. The
install strategies (`auto` never displaces an OEM effect, `composite`,
`replace`) now apply to the endpoint slot.

`install` on an endpoint that still has the 0.1.0 stream-effect registration
plans an **upgrade**: it removes Somike from the stream-effect slot (or chain)
and adds the endpoint effect, merging the backup so that uninstall still
restores the state from before the first installation. The app shows an
**Update** banner for such endpoints.

## Consequences

- Every shared-mode capture stream gets the routed audio, whatever processing
  mode it requests (verified: default, communications, raw,
  communications+raw).
- One APO instance per microphone instead of one per stream: less work in
  `audiodg.exe`, one consumer of the ring.
- The routed audio passes through OEM mode/stream effects (e.g. noise
  suppression of the communications mode) after injection, like the
  microphone's own signal.
- Microsoft recommends EFX "with care". Raw capture is meant to carry no
  adaptive processing; Somike's injection is not adaptive, and the user turns
  it on explicitly.
- Microphones whose driver already has an endpoint effect need
  `--strategy composite` or `replace` (the app offers composite when `auto`
  refuses).
- Exclusive-mode capture is still not reached.
