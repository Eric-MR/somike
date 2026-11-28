# ADR 0006: Soundboard as a second producer source, with CC0 sounds from Openverse

- Status: accepted
- Date: 2026-11-28

## Context

Users want to play short sounds (their own files, and common effects) into
the microphone, with global hotkeys, while an app is routed or not.

The shared ring (ADR 0003) has exactly **one producer**, and the APO
consumer re-primes to its 40 ms target latency whenever the ring runs dry or
a new session starts. Until now the producer was the capture thread of
`LoopbackCapture`, whose pace is set by the audio engine's clock. Sounds have
no clock of their own, and while no app plays (or nothing is routed) there is
no capture thread at all.

A second writer process or a second ring would need protocol changes
(protocol v2, multiple consumers' positions per producer) and a mixer inside
`audiodg.exe`, where every extra code path counts against Windows disabling
the effect.

Downloading "common sound effects" needs a source that allows reuse in a
voice chat without the user handling licences, has a stable public API and
needs no account or API key in the binary.

## Decision

**Mix in the app, keep one writer.** `core::Router` gets a second source,
`dsp::VoiceMixer` (up to 16 voices, one per sound, 5 ms fades):

- While app audio flows, the capture thread mixes the voices into each block
  before writing it, so sounds follow the capture clock.
- Otherwise a pump thread (10 ms high-resolution waitable timer) writes the
  voices at the pace of the system clock (`dsp::FramePacer` on QPC time). It
  takes over 30 ms after the last captured block and backfills the gap. After
  a longer pause it writes 30 ms of silence first, so the consumer's
  re-priming skips silence rather than the start of a sound. It keeps the
  session open for 1.5 s after the last sound, then ends it. These timing
  decisions live in the pure `dsp::PumpScheduler`, unit-tested with a fake
  clock; the router only executes its steps.
- `write_mutex_` guarantees exactly one of both writes at a time; control
  calls take `control_mutex_` first (fixed lock order).
- Gains move to the producer: the app volume is applied to the captured
  block, the soundboard volume to the voices; the ring's injection gain stays
  1.0. "Mute my voice" still uses the ring's microphone gain, and only while
  an app is routed.

Files are decoded once with Media Foundation (`IMFSourceReader`, every format
Windows can play) and converted offline to the canonical 48 kHz stereo float
with a windowed-sinc resampler, so the real-time paths only add samples.

**Downloads: Openverse, CC0 and public domain only.** The catalogue searches
the anonymous Openverse API (`api.openverse.org/v1/audio`) with
`license=cc0,pdm` and short lengths, which covers Freesound and Wikimedia
Commons. Downloads are restricted to https URLs on `freesound.org` and
`wikimedia.org`, size-capped (25 MB) and decoded before they enter the
library; title, creator, licence and source link are stored with the sound.

## Consequences

- No protocol, APO or installer change: soundboard audio reaches every
  microphone stream exactly like routed app audio (ADR 0005).
- Sounds and app audio are mixed sample-accurately while an app is live;
  without one, the pump's system-clock pace drifts against the microphone
  clock by a few ppm, which the APO's drift controller already absorbs.
- A sound started after silence reaches the mic about 30 ms later than it
  would mid-session (the lead-in), well below what voice chat notices.
- CC0/PDM limits the catalogue: well-known meme sounds are usually
  copyrighted and are not offered; users can import their own files instead.
  Sites without an API or licence information (e.g. MyInstants) are not
  scraped.
- Hotkeys use `RegisterHotKey`; keys another program already owns fail to
  register and are shown as such. Bare keys are limited to F-keys, numpad,
  media and a few others, so typing is never swallowed.
