<#
.SYNOPSIS
    Builds the Somike setup program (Somike-Setup-<version>.exe) with Inno Setup.

.DESCRIPTION
    Takes an existing CMake build (run scripts/build.ps1 first), installs it
    into build/<preset>/package and compiles packaging/somike.iss. The version
    comes from the build's generated version.h.

.EXAMPLE
    ./scripts/build.ps1 -Preset release
    ./scripts/build-installer.ps1 -DownloadWebView2
#>
[CmdletBinding()]
param(
    [string]$Preset = "release",
    [string]$OutputDir,
    # MicrosoftEdgeWebview2Setup.exe to bundle; setup runs it when the
    # WebView2 Runtime is missing (e.g. on Windows Server).
    [string]$WebView2Bootstrapper,
    # Download the bootstrapper from Microsoft (signature is verified).
    [switch]$DownloadWebView2
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$build = Join-Path $root "build\$Preset"
if (-not $OutputDir) { $OutputDir = Join-Path $root "build\installer" }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)

$header = Join-Path $build "generated\somike\version.h"
if (-not (Test-Path $header)) {
    throw "No build in $build. Run ./scripts/build.ps1 -Preset $Preset first."
}
if ((Get-Content $header -Raw) -notmatch '#define SOMIKE_VERSION_STRING "([^"]+)"') {
    throw "SOMIKE_VERSION_STRING not found in $header"
}
$version = $Matches[1]
$numeric = ($version -split '-')[0]

# The files that go into the setup program, exactly as `cmake --install` lays them out.
$payload = Join-Path $build "package"
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
cmake --install $build --prefix $payload | Out-Null
if ($LASTEXITCODE) { throw "cmake --install failed" }

if ($DownloadWebView2 -and -not $WebView2Bootstrapper) {
    $WebView2Bootstrapper = Join-Path $build "MicrosoftEdgeWebview2Setup.exe"
    Write-Host "Downloading the WebView2 bootstrapper..."
    Invoke-WebRequest "https://go.microsoft.com/fwlink/p/?LinkId=2124703" -OutFile $WebView2Bootstrapper -UseBasicParsing
}
if ($WebView2Bootstrapper) {
    $WebView2Bootstrapper = (Resolve-Path $WebView2Bootstrapper).Path
    $signature = Get-AuthenticodeSignature $WebView2Bootstrapper
    if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Subject -notmatch "O=Microsoft Corporation") {
        throw "WebView2 bootstrapper is not signed by Microsoft: $($signature.Status) $($signature.SignerCertificate.Subject)"
    }
}

$iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) {
    throw "Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup"
}

$defines = @(
    "/DAppVersion=$version",
    "/DAppVersionNumeric=$numeric",
    "/DPayloadDir=$payload",
    "/DOutputDir=$OutputDir"
)
if ($WebView2Bootstrapper) { $defines += "/DWebView2Bootstrapper=$WebView2Bootstrapper" }

& $iscc /Qp @defines (Join-Path $root "packaging\somike.iss")
if ($LASTEXITCODE) { throw "ISCC failed with exit code $LASTEXITCODE" }

$setup = Join-Path $OutputDir "Somike-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "Expected $setup was not created" }
Write-Host "Setup program: $setup"
