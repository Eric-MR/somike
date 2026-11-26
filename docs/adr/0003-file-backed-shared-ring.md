# ADR 0003: File-backed shared ring in %ProgramData%

- Status: accepted
- Date: 2026-11-26

## Context

The producer runs unelevated in the user's session; the consumer runs as
LOCAL SERVICE in session 0 inside `audiodg.exe`. They need shared memory.

- A pagefile-backed **named section** must live in the `Global\` namespace to
  be visible across sessions, and creating `Global\` objects requires
  `SeCreateGlobalPrivilege`, which normal interactive users do not have. The
  object also disappears when its last handle closes, so it would need a
  long-lived privileged owner. Whether `audiodg.exe`'s restricted token keeps
  that privilege is undocumented.
- A **file** has a lifetime, an explicit DACL we control at install time, and
  views of the same file are coherent across processes.

## Decision

Use a fixed-size file `%ProgramData%\Somike\ring-v1.bin` created by the
installer with an explicit DACL (see [ipc-protocol.md](../ipc-protocol.md)).
Both sides map it. The APO maps it in `LockForProcess`, never on the
real-time thread.

## Consequences

- Works without extra privileges at runtime and without a service.
- The memory manager may occasionally write dirty pages back to disk. The
  file is only 260 KB and pages are rewritten in place, so I/O is bounded
  and small.
- Any interactive user of the machine can write to the ring (multi-user
  machines can inject into each other's microphone). This is acceptable for a
  desktop tool; documented. A future version could use per-user rings.
