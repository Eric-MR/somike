# ADR 0001: Inject through a capture APO instead of a virtual audio device

- Status: accepted
- Date: 2026-11-26

## Context

The product requirement is that other applications receive routed audio
**through the microphone they already use**, without users changing device
settings, and preferably without creating a virtual audio device.

## Decision

Register a capture **stream effect (SFX) APO** on the physical microphone's
endpoint (`FxProperties`), loaded by `audiodg.exe`, and feed it from the app
through shared memory.

## Consequences

- No new device, so apps need no reconfiguration, and the hook applies to
  every shared-mode capture stream of that microphone.
- One-time admin installation and an audio-service restart.
- Not an officially supported installation route; driver updates can reset
  it, which needs detection and repair.
- RAW/exclusive capture streams are not reached.
- Distribution requires code signing on Smart App Control/WDAC systems.
- A virtual-device fallback remains possible by reusing `dsp::Injector`
  (see [feasibility study](../feasibility.md), section 5).

Rejected alternatives: virtual device (violates the core requirement and
needs a signed kernel driver), kernel filter driver (risk/effort), injecting
into consumer apps (fragile, anti-cheat).
