# Contributing

Thanks for helping! Somike is in beta, so reports from real setups are the
most valuable contribution:

- **Something broken?** Open a [bug report](https://github.com/Eric-MR/somike/issues/new?template=bug_report.yml)
  with the output of `somike-apoctl status`.
- **Works (or not) with your microphone or app?** A
  [compatibility report](https://github.com/Eric-MR/somike/issues/new?template=compatibility.yml)
  takes a minute and helps everyone.
- **Security problem?** Report it privately, see [SECURITY.md](SECURITY.md).

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

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

- `main` is always releasable. Work on a branch (`feat/...`, `fix/...`,
  `docs/...`) and open a pull request against `main`; CI (Debug and
  Release with warnings as errors, plus unit tests) must pass.
- Use [Conventional Commits](https://www.conventionalcommits.org/)
  (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `build:`, `ci:`).
- One logical change per pull request, with tests for new logic.
- Record significant design decisions as a new ADR in `docs/adr/`.
- Update `CHANGELOG.md` under *Unreleased*.

## Labels

| Label | Meaning |
|-------|---------|
| `bug`, `enhancement`, `documentation`, `compatibility` | Kind of issue |
| `area: apo`, `area: app`, `area: routing`, `area: installer`, `area: build` | Part of the code |
| `needs-triage`, `needs-info`, `confirmed` | Triage state |
| `good first issue`, `help wanted` | Good places to start |

## Releases

Versions follow [Semantic Versioning](https://semver.org/); `-beta.N` and
`-rc.N` versions are published as GitHub pre-releases. Pushing a tag
`v<version>` builds and publishes the release automatically, see
[docs/building.md](docs/building.md#releases).
