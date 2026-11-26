# ADR 0004: WASAPI process loopback for per-application capture

- Status: accepted
- Date: 2026-11-26

## Context

Users want to route a single application (music player, browser, game) or
everything they hear.

## Decision

Use `ActivateAudioInterfaceAsync` with `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`
and `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`:

- one application: `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` with
  the app's PID (child processes, e.g. browser audio services, are included);
- everything: `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE` with
  Somike's own PID, which is one code path and never captures ourselves.

The stream is requested directly in the canonical format (48 kHz stereo
float, `AUTOCONVERTPCM`, 16-bit PCM fallback), so the engine does any
conversion and the producer does no DSP.

## Consequences

- Requires Windows 10 build 20348+ / Windows 11.
- Sources are discovered from audio sessions on all playback devices and
  remembered by executable name (PIDs change between runs).
- "Everything" includes voice-chat output, so the UI recommends per-app routing.
