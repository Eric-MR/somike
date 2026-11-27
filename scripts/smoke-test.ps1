<#
.SYNOPSIS
    End-to-end check of the Somike setup program on a clean Windows machine:
    install, files and registration, start the app and drive its UI, update
    over the running app, uninstall, and check that nothing is left behind.

.DESCRIPTION
    CI runs it on fresh GitHub-hosted VMs (several Windows versions, no
    microphone); it works on any test VM too. Needs an elevated shell and
    Node.js 22+ (tools/ui-dev/drive.mjs). It UNINSTALLS Somike at the end, so
    don't run it on a machine where you use Somike.

.EXAMPLE
    ./scripts/smoke-test.ps1 -Setup build/installer/Somike-Setup-0.2.0-beta.2.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Setup,
    [string]$OutDir = "smoke-results",
    [int]$DevToolsPort = 9222,
    # ARM64 Windows: the setup program must refuse to install (x64-only APO).
    [switch]$ExpectRefusal
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Setup = (Resolve-Path $Setup).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$drive = Join-Path $root "tools\ui-dev\drive.mjs"
$appDir = Join-Path $env:ProgramFiles "Somike"
$uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{88DD7E1A-5478-441D-8444-9CAE45D20948}_is1"
$apoClsidKey = "HKLM:\SOFTWARE\Classes\CLSID\{B5F655D4-FC22-4DDB-9240-494757B5BF74}"
$shortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Somike.lnk"
$expectedVersion = if ((Split-Path $Setup -Leaf) -match '^Somike-Setup-(.+)\.exe$') { $Matches[1] } else { "" }

$results = [System.Collections.Generic.List[object]]::new()
function Test-Step([string]$Name, [bool]$Ok, [string]$Detail = "") {
    $results.Add([pscustomobject]@{ Check = $Name; Ok = $Ok; Detail = $Detail })
    $mark = if ($Ok) { "PASS" } else { "FAIL" }
    $suffix = if ($Detail) { " - $Detail" } else { "" }
    Write-Host "[$mark] $Name$suffix"
}
function Invoke-Drive([string[]]$Arguments) {
    $output = & node $drive $DevToolsPort @Arguments 2>&1
    [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Output = (($output | ForEach-Object { "$_" }) -join "`n").Trim() }
}
function Wait-Until([scriptblock]$Condition, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return [bool](& $Condition)
}
function Invoke-Setup([string]$LogName, [string[]]$Extra = @()) {
    $arguments = @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/LOG=`"$(Join-Path $OutDir $LogName)`"") + $Extra
    (Start-Process $Setup -ArgumentList $arguments -Wait -PassThru).ExitCode
}
function Test-WebView2 {
    $key = "SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
    foreach ($path in "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", "HKCU:\$key") {
        $pv = (Get-ItemProperty $path -ErrorAction SilentlyContinue).pv
        if ($pv -and $pv -ne "0.0.0.0") { return $pv }
    }
    return $null
}
function Write-Summary($environment) {
    $results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutDir "results.json") -Encoding utf8
    $failed = @($results | Where-Object { -not $_.Ok }).Count
    $lines = @("### Somike smoke test: $($environment.Windows)", "")
    $lines += $environment.GetEnumerator() | ForEach-Object { "- **$($_.Key)**: $($_.Value)" }
    $lines += "", "| | Check | Detail |", "|---|---|---|"
    $lines += $results | ForEach-Object {
        $mark = if ($_.Ok) { "✅" } else { "❌" }
        "| $mark | $($_.Check) | $(($_.Detail -replace '\r?\n', ' ' -replace '\|', '/')) |"
    }
    $lines += "", "**$($results.Count - $failed) of $($results.Count) checks passed.**"
    $lines | Set-Content (Join-Path $OutDir "summary.md") -Encoding utf8
    if ($env:GITHUB_STEP_SUMMARY) { $lines | Add-Content $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
    return $failed
}

# ---------------------------------------------------------------- environment
$os = Get-CimInstance Win32_OperatingSystem
$audio = Get-Service Audiosrv -ErrorAction SilentlyContinue
if ($audio -and $audio.Status -ne "Running") {
    # Hosted VMs may ship with Windows Audio stopped; real PCs always run it.
    try { Set-Service Audiosrv -StartupType Automatic; Start-Service Audiosrv } catch { Write-Host "Could not start Windows Audio: $_" }
    $audio.Refresh()
}
$webviewBefore = Test-WebView2
$environment = [ordered]@{
    Windows        = "$($os.Caption) (build $($os.BuildNumber))"
    Architecture   = $env:PROCESSOR_ARCHITECTURE
    WindowsAudio   = if ($audio) { "$($audio.Status)" } else { "not present" }
    WebView2Before = if ($webviewBefore) { $webviewBefore } else { "not installed" }
    Setup          = Split-Path $Setup -Leaf
}
$environment.GetEnumerator() | ForEach-Object { Write-Host "$($_.Key): $($_.Value)" }

if ($ExpectRefusal) {
    $code = Invoke-Setup "install.log"
    Test-Step "setup refuses to install on this architecture" ($code -ne 0) "exit code $code"
    Test-Step "nothing was installed" (-not (Test-Path $appDir))
    exit [int]((Write-Summary $environment) -gt 0)
}

# ---------------------------------------------------------------- install
$code = Invoke-Setup "install.log" @("/TASKS=setupmic")
Test-Step "setup exits with 0" ($code -eq 0) "exit code $code"
foreach ($file in "somike.exe", "somike-apoctl.exe", "somike_apo.dll", "somike-selftest.exe", "LICENSE.txt", "unins000.exe") {
    Test-Step "installed $file" (Test-Path (Join-Path $appDir $file))
}
$entry = Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue
Test-Step "listed in Apps (Settings)" ($null -ne $entry) $(if ($entry) { "$($entry.DisplayName), $($entry.Publisher)" } else { "" })
if ($expectedVersion) {
    Test-Step "registered version is $expectedVersion" ($entry -and $entry.DisplayVersion -eq $expectedVersion) "$($entry.DisplayVersion)"
}
Test-Step "Start menu shortcut" (Test-Path $shortcut)
$webview = Test-WebView2
Test-Step "WebView2 Runtime available" ($null -ne $webview) "$webview"
$environment.WebView2After = if ($webview) { $webview } else { "not installed" }

# ---------------------------------------------------------------- command-line tools
$apoctl = Join-Path $appDir "somike-apoctl.exe"
$usage = (& $apoctl 2>&1 | Out-String)
Test-Step "somike-apoctl reports version $expectedVersion" ($usage -match [regex]::Escape("somike-apoctl $expectedVersion")) ($usage -split "`n")[0].Trim()
$list = (& $apoctl list 2>&1 | Out-String); $listCode = $LASTEXITCODE
$list | Set-Content (Join-Path $OutDir "apoctl-list.txt") -Encoding utf8
Test-Step "somike-apoctl list runs" ($listCode -in 0, 1) "exit code $listCode"
$environment.Microphones = ([regex]::Matches($list, "(?m)^\s+id:")).Count
$status = (& $apoctl status 2>&1 | Out-String); $statusCode = $LASTEXITCODE
$status | Set-Content (Join-Path $OutDir "apoctl-status.txt") -Encoding utf8
Test-Step "somike-apoctl status runs" ($statusCode -in 0, 1) "exit code $statusCode"
if (Test-Path (Join-Path $appDir "setup-mic.log")) {
    Copy-Item (Join-Path $appDir "setup-mic.log") $OutDir
}

$selftest = Start-Process (Join-Path $appDir "somike-selftest.exe") -ArgumentList "--modes" -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $OutDir "selftest.txt") -RedirectStandardError (Join-Path $OutDir "selftest-err.txt")
$finished = $selftest.WaitForExit(90000)
if (-not $finished) { $selftest.Kill() }
# Without audio devices it must fail politely, never crash (NTSTATUS exit codes are negative).
Test-Step "somike-selftest finishes without crashing" ($finished -and $selftest.ExitCode -ge 0) $(if ($finished) { "exit code $($selftest.ExitCode)" } else { "timed out" })

# ---------------------------------------------------------------- the app
$app = Start-Process (Join-Path $appDir "somike.exe") -ArgumentList "--remote-debugging-port", $DevToolsPort -PassThru
$ready = Invoke-Drive @("wait", "typeof state === 'object' && state !== null && !!state.view", "60000")
Test-Step "app window loads the UI" $ready.Ok $ready.Output
if ($ready.Ok) {
    $raw = Invoke-Drive @("eval", "JSON.stringify(state)")
    $state = ($raw.Output | ConvertFrom-Json) | ConvertFrom-Json
    $state | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutDir "ui-state.json") -Encoding utf8
    Test-Step "UI shows a known view" ($state.view -in "setup", "main") "view '$($state.view)', $(@($state.mics).Count) microphone(s), $(@($state.sources).Count) source(s)"
    if (@($state.mics).Count -eq 0) {
        $message = Invoke-Drive @("eval", "document.getElementById('setup-msg').textContent")
        Test-Step "no microphone: the setup screen says so" ($state.view -eq "setup" -and $message.Output -match "No microphone") $message.Output
        $disabled = Invoke-Drive @("eval", "document.getElementById('setup-btn').disabled")
        Test-Step "no microphone: Set up is disabled" ($disabled.Output -eq "true")
    }
    $overflow = Invoke-Drive @("eval", "document.documentElement.scrollWidth <= innerWidth + 1")
    Test-Step "no horizontal overflow" ($overflow.Output -eq "true")
    $menu = Invoke-Drive @("eval", "(() => { document.getElementById('more').click(); return [...document.querySelectorAll('#menu button')].filter((b) => !b.hidden).map((b) => b.textContent).join(' | '); })()")
    $menuOk = if ($state.view -eq "setup") { $menu.Ok -and $menu.Output -notmatch "Remove|Repair" } else { $menu.Ok }
    Test-Step "menu offers only what applies" $menuOk $menu.Output
    $null = Invoke-Drive @("eval", "document.body.click()")
    $shot = Invoke-Drive @("screenshot", (Join-Path $OutDir "app.png"))
    Test-Step "screenshot of the real window" $shot.Ok
}
$title = (Get-Process -Id $app.Id -ErrorAction SilentlyContinue).MainWindowTitle
Test-Step "window title" ($title -like "Somike*") "'$title'"

$second = Start-Process (Join-Path $appDir "somike.exe") -PassThru
$handedOver = $second.WaitForExit(15000)
Test-Step "a second start hands over to the running window" ($handedOver -and $second.ExitCode -eq 0 -and -not $app.HasExited)

$null = $app.CloseMainWindow()
$closed = $app.WaitForExit(15000)
Test-Step "app closes cleanly" ($closed -and $app.ExitCode -eq 0) $(if ($closed) { "exit code $($app.ExitCode)" } else { "still running" })
if (-not $closed) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
$log = Join-Path $env:LOCALAPPDATA "Somike\somike.log"
if (Test-Path $log) {
    Copy-Item $log $OutDir
    Test-Step "app log shows version $expectedVersion" ((Get-Content $log -Raw) -match [regex]::Escape("Somike $expectedVersion starting"))
}

# ---------------------------------------------------------------- update over a running app
$running = Start-Process (Join-Path $appDir "somike.exe") -PassThru
$null = Wait-Until { (Get-Process -Id $running.Id -ErrorAction SilentlyContinue).MainWindowHandle -ne 0 } 30
$code = Invoke-Setup "update.log"
Test-Step "update over the running app exits with 0" ($code -eq 0) "exit code $code"
Test-Step "update closed the running app first" ($running.HasExited)
if (-not $running.HasExited) { Stop-Process -Id $running.Id -Force -ErrorAction SilentlyContinue }

# ---------------------------------------------------------------- uninstall
Start-Process (Join-Path $appDir "unins000.exe") -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=`"$(Join-Path $OutDir 'uninstall.log')`"" -Wait
# The uninstaller re-launches itself from %TEMP%; wait for the real work.
Test-Step "uninstall removes the program" (Wait-Until { -not (Test-Path $uninstallKey) -and -not (Test-Path (Join-Path $appDir "somike.exe")) } 120)
Test-Step "uninstall removes the folder" (Wait-Until { -not (Test-Path $appDir) } 30)
Test-Step "Start menu shortcut removed" (-not (Test-Path $shortcut))
Test-Step "APO registration removed" (-not (Test-Path $apoClsidKey))
Test-Step "ProgramData\Somike removed" (-not (Test-Path (Join-Path $env:ProgramData "Somike")))

$failed = Write-Summary $environment
Write-Host "$($results.Count - $failed) of $($results.Count) checks passed. Results: $OutDir"
exit [int]($failed -gt 0)
