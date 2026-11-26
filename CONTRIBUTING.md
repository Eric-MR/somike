# Contributing

## Build and test

```powershell
./scripts/build.ps1                 # debug build + unit tests
./scripts/build.ps1 -Preset ci      # what CI runs: Release, warnings as errors
```

See [docs/building.md](docs/building.md) for prerequisites and for debugging
the APO inside `audiodg.exe`.

## Ground rules

1. **Keep the layers clean.** Dependencies point one way:
   `protocol` <- `dsp` <- `common` <- `installer` <- `core` <- `app`/`apoctl`.
   `src/apo` depends only on `protocol`, `dsp` and `common`.
   Put new logic in the lowest layer that can hold it, and write it so it can
   be unit-tested (pure functions, injected clocks).
2. **The APO is sacred.** Code reachable from `APOProcess` must not allocate,
   lock, log, do I/O or throw. Everything the APO reads from shared memory is
   untrusted. When in doubt, pass the microphone through untouched.
   Repeated APO failures make Windows disable all effects on the device.
3. **Protocol changes** to `src/protocol/include/somike/protocol/shared_ring.h`
   that alter the layout or semantics must bump `kVersion` (which also renames
   the ring file) and update [docs/ipc-protocol.md](docs/ipc-protocol.md).
4. **Never touch the system silently.** Anything that changes the registry or
   system files goes through `somike-apoctl`, backs up what it changes, and
   supports `--dry-run`.
5. Warnings are errors in CI (`/W4 /permissive-`). Format with the repository
   `.clang-format`.

## Commits and pull requests

- Use [Conventional Commits](https://www.conventionalcommits.org/)
  (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `build:`, `ci:`).
- One logical change per pull request, with tests for new logic.
- Record significant design decisions as a new ADR in `docs/adr/`.
- Update `CHANGELOG.md` under *Unreleased*.
