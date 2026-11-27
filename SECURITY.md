# Security policy

Somike runs code inside the Windows audio engine (`somike_apo.dll` in
`audiodg.exe`), and its installer changes system registry keys and copies
files with administrator rights. Security reports are therefore very welcome.

## Supported versions

Somike is in beta. Only the latest release (including pre-releases) receives
fixes.

## Reporting a vulnerability

Please **do not open a public issue**. Report it privately through GitHub:
[**Report a vulnerability**](https://github.com/Eric-MR/somike/security/advisories/new)
(the *Security* tab of the repository).

Include the Somike version, the Windows build, steps to reproduce and the
impact you see. You will get an answer as soon as possible. Once a fix is
released, the advisory is published with credit to you, unless you prefer
otherwise.

## Scope

Especially relevant:

- the APO reading the shared ring file (`%ProgramData%\Somike\ring-v1.bin`),
  which it treats as untrusted input;
- the elevated installer (`somike-apoctl install` / `uninstall`): registry
  edits under `MMDevices`, files in `%ProgramFiles%\Somike`, the backup in
  `HKLM\SOFTWARE\Somike`;
- permissions of the files and keys Somike creates.

Out of scope: vulnerabilities in Windows itself, and the fact that release
binaries are not code-signed yet (a known limitation).
