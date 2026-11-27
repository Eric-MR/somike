## Install

Requirements: Windows 11 (or Windows 10 build 20348+), x64, a microphone.

1. Download **`somike-{{VERSION}}-win-x64.zip`** below, extract it to a folder
   of your choice and start `somike.exe`.
2. The first time, click **Set up**. Windows asks for administrator rights
   once, and sound pauses for a second while the audio service restarts.
3. Pick the app you want to hear in your mic and press **Go live** (or
   **Ctrl+Alt+M** anywhere).

> [!NOTE]
> The binaries are **not code-signed yet**. SmartScreen may show "Windows
> protected your PC": choose **More info → Run anyway**. If *Smart App
> Control* is on, Windows blocks unsigned apps completely.

To uninstall, open the **⋯** menu in Somike and choose **Remove Somike from
this microphone**, then delete the folder. From an administrator terminal,
`./somike-apoctl uninstall --all --restart-audio` does the same for every
microphone.

Something not working? `somike-selftest.exe --modes` (in the zip) checks the
whole chain on your default microphone. It plays a quiet test tone into the
mic for about ten seconds, so leave voice calls first. Please attach its
output and
`somike-apoctl status` to a [bug report](https://github.com/Eric-MR/somike/issues/new/choose).

| File | Contents |
|------|----------|
| `somike-{{VERSION}}-win-x64.zip` | The app: `somike.exe`, `somike-apoctl.exe`, `somike_apo.dll`, `somike-selftest.exe` |
| `somike-{{VERSION}}-win-x64-symbols.zip` | Debug symbols (PDB), only needed for crash analysis |
| `SHA256SUMS.txt` | SHA-256 checksums |

Verify a download with `gh attestation verify <file> --repo Eric-MR/somike`
(build provenance) or compare `Get-FileHash <file>` with `SHA256SUMS.txt`.

## Changes

{{CHANGES}}
