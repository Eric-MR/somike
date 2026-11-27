## What and why

<!-- What does this change, and why? Link the issue: "Fixes #123". -->

## How it was tested

- [ ] `./scripts/build.ps1 -Preset ci` (Release, warnings as errors, unit tests)
- [ ] On real hardware (`somike-selftest`, `somike-selftest --modes`), if APO,
      routing or installer code changed

## Checklist

- [ ] `CHANGELOG.md` updated under *Unreleased* for user-visible changes
- [ ] Docs updated; an ADR in `docs/adr/` for design decisions
- [ ] Nothing that allocates, locks, logs or does I/O is reachable from
      `APOProcess`
