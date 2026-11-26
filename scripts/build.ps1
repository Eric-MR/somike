<#
.SYNOPSIS
    Configures, builds and tests Somike from any PowerShell (no Developer shell needed).

.DESCRIPTION
    Locates Visual Studio / Build Tools via vswhere, imports the x64 MSVC
    environment and runs the CMake preset workflow.

.EXAMPLE
    ./scripts/build.ps1                    # debug build + tests
    ./scripts/build.ps1 -Preset release    # release build + tests
    ./scripts/build.ps1 -NoTests -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'ci')]
    [string] $Preset = 'debug',
    [switch] $NoTests,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Windows PowerShell 5.1 turns any stderr output of a native tool into a
# terminating error under 'Stop'. Judge native tools by exit code only.
function Invoke-Native([string] $Description, [scriptblock] $Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Command 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code) { throw "$Description failed (exit code $code)" }
}

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.'
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }

    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    Write-Host "Using MSVC environment from $vs"
    # vcvars64.bat itself calls vswhere.exe and expects to find it on PATH.
    $env:Path = "$(Split-Path -Parent $vswhere);$env:Path"
    $envDump = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE) { throw "vcvars64.bat failed ($LASTEXITCODE)" }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

Import-MsvcEnvironment

Push-Location $root
try {
    if ($Clean -and (Test-Path "build/$Preset")) {
        Remove-Item -Recurse -Force "build/$Preset"
    }
    Invoke-Native 'CMake configure' { cmake --preset $Preset }
    Invoke-Native 'Build' { cmake --build --preset $Preset }
    if (-not $NoTests) {
        Invoke-Native 'Tests' { ctest --preset $Preset }
    }
    Write-Host "Binaries: $root\build\$Preset\bin" -ForegroundColor Green
}
finally {
    Pop-Location
}
