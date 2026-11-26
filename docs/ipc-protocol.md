# Shared ring protocol (version 1)

Single source of truth in code:
[`src/protocol/include/somike/protocol/shared_ring.h`](../src/protocol/include/somike/protocol/shared_ring.h).

## Transport

- File: `%ProgramData%\Somike\ring-v1.bin`, exactly **266 240 bytes**, created
  by `somike-apoctl install`. The version is part of the name, so an old APO
  and a new app never read each other's layout.
- Both sides map the file (`CreateFileMapping` + `MapViewOfFile`). Views of the
  same file are coherent across processes.
- Why a file and not a named section: see
  [ADR 0003](adr/0003-file-backed-shared-ring.md).

### Access control

| Principal | Rights | Why |
|---|---|---|
| SYSTEM, Administrators | full | management |
| INTERACTIVE (IU) | read + write | the app (producer), unelevated |
| LOCAL SERVICE (LS) | read + write | `audiodg.exe` runs as LOCAL SERVICE |
| NT SERVICE\AudioSrv | read + write | service SID present in `audiodg.exe`'s token |

The directory `%ProgramData%\Somike` grants Users read-only access, so
unprivileged users cannot plant or replace the file. The DACL is protected
(not inherited).

## Layout

All integers are little-endian. Fields marked *atomic* are accessed with
`std::atomic_ref` (lock-free, address-free on x64).

| Offset | Size | Field | Writer | Notes |
|---:|---:|---|---|---|
| 0 | 4 | `magic` = `0x4B4D4F53` ("SOMK") | producer/installer | published last when initialising |
| 4 | 4 | `version` = 1 | producer/installer | |
| 8 | 4 | `header_bytes` = 4096 | " | |
| 12 | 4 | `sample_rate` = 48000 | " | canonical format |
| 16 | 4 | `channels` = 2 | " | interleaved |
| 20 | 4 | `capacity_frames` = 32768 | " | power of two (~683 ms) |
| 24 | 40 | reserved | - | zero |
| **64** | | **ProducerBlock** (64 bytes, cache line) | producer only | |
| 64 | 8 | `write_frame` *atomic* | producer | monotonic count of frames ever written |
| 72 | 8 | `heartbeat_ms` *atomic* | producer | `GetTickCount64()` at last sign of life |
| 80 | 4 | `state` *atomic* | producer | 0 = Idle, 1 = Streaming |
| 84 | 4 | `session` *atomic* | producer | +1 on every (re)start |
| 88 | 4 | `inject_gain_bits` *atomic* | producer | IEEE-754 float, linear |
| 92 | 4 | `mic_gain_bits` *atomic* | producer | IEEE-754 float, linear |
| **128** | | **ConsumerBlock** (64 bytes) | any APO instance | diagnostics only |
| 128 | 8 | `heartbeat_ms` *atomic* | consumers | last processed block (throttled to 1 s while idle) |
| 136 | 8 | `frames_injected` *atomic add* | consumers | |
| 144 | 8 | `underruns` *atomic add* | consumers | |
| 152 | 8 | `resyncs` *atomic add* | consumers | |
| 160 | 4 | `sample_rate` | consumers | endpoint format of last active instance |
| 164 | 4 | `channels` | consumers | |
| 4096 | 262144 | samples | producer | `capacity_frames x 2 x float32` |

Frame `n` lives at sample offset `(n & (capacity - 1)) * 2`.

## Producer algorithm

1. On start: set gains, `session += 1`, `heartbeat = now`, `state = Streaming`
   (release stores).
2. For each captured packet: copy frames at `write_frame` (wrapping), then
   `write_frame += n` (release), `heartbeat = now`.
3. At least every 100 ms, even without audio: `heartbeat = now`.
4. On stop: `state = Idle`.

## Consumer algorithm (per APO instance)

Private state: `phase` (Idle/Priming/Running), `read`, `session`.

1. Inactive unless `magic`/`version`/format match, `state == Streaming` and
   `now - heartbeat <= 500 ms`. Inactive means pass-through with mic gain 1.
2. On first activity, session change, or `write < read`: `read = write`,
   phase Priming. Starting at `write` (not behind it) guarantees no stale
   audio from a previous session is ever replayed.
3. Priming: wait until `write - read >= target` (1920 frames, 40 ms), then
   `read = write - target`, phase Running.
4. Running: if `write - read > max` (7200 frames, 150 ms), resync
   `read = write - target`. Copy `min(wanted, available)` frames. After
   copying, re-read `write_frame`; if the writer lapped the copied region
   (`write' - read > capacity`), discard the block (seqlock-style validation).
5. Fewer frames than wanted means an underrun: pad with silence, phase Priming.

All values read from the file are untrusted: indices are masked with
compile-time constants, never with header values, gains are clamped to
`[0, 4]` (non-finite becomes 0 for inject and 1 for mic), and output samples
are clamped to `[-1, 1]` with NaN replaced by 0.

## Versioning rules

Any change to layout or semantics bumps `kVersion`, which changes the file
name (`ring-v2.bin`) and is installed side by side. A consumer that finds an
incompatible header passes the microphone through.
