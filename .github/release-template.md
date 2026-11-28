## Install

Requirements: **Windows 11** (x64) and a microphone. Windows 10 is not
supported: per-app capture needs Windows build 20348 or newer. ARM64 PCs are
not supported yet. On Windows N editions the soundboard needs the *Media
Feature Pack* (Settings → System → Optional features).

1. Download **`Somike-Setup-{{VERSION}}.exe`** below and run it.
2. Keep **Connect Somike to my default microphone** checked. Windows asks
   for administrator rights once, and sound pauses for a second while Windows
   Audio restarts.
3. Somike starts: pick the app you want to hear in your mic and press
   **Go live** (or **Ctrl+Alt+M** anywhere).

> [!NOTE]
> The setup program is **not code-signed yet**. SmartScreen may show "Windows
> protected your PC": choose **More info → Run anyway**. If *Smart App
> Control* is on, Windows blocks unsigned programs completely.

Updating: run the new setup program; it closes Somike, replaces it and keeps
your settings. Uninstalling: *Settings → Apps → Installed apps → Somike →
Uninstall*. This also restores every microphone exactly as it was.

Something not working? Run `somike-selftest --modes` from
`C:\Program Files\Somike` in a terminal. It checks the whole chain on your
default microphone and plays a quiet test tone into it for about ten seconds,
so leave voice calls first. Please attach its output and
`somike-apoctl status` to a [bug report](https://github.com/Eric-MR/somike/issues/new/choose).

| File | Contents |
|------|----------|
| `Somike-Setup-{{VERSION}}.exe` | Setup program (installs to `C:\Program Files\Somike`) |
| `Somike-{{VERSION}}-symbols.zip` | Debug symbols (PDB), only needed for crash analysis |
| `SHA256SUMS.txt` | SHA-256 checksums |

Every build is tested automatically on fresh Windows Server 2022 and 2025
machines (install, start, update, uninstall) before it is published. Verify a
download with `gh attestation verify <file> --repo Eric-MR/somike` (build
provenance) or compare `Get-FileHash <file>` with `SHA256SUMS.txt`.

## Changes

{{CHANGES}}
