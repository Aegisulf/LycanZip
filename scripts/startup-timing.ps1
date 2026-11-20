# Measures where a start of LycanZip spends its time: starts the app several times, reads the "startup:" lines of its log
# (app\StartupTiming.h: milliseconds since the process was created) and prints the median of each step.
# Opens a window on the screen each time (nothing to click); uses its own settings and log folders, never the user's.
#   .\scripts\startup-timing.ps1 [-Archive <file>] [-Runs 7] [-Config Release|Debug]
#   -Forward: LycanZip is running already (with -PrimaryArchive open, or the home page); measures a second start that opens -Archive,
#   which hands the request to the running one: the time from the start of the second process to each step in the running one.
param(
    [string]$Archive = "",
    [int]$Runs = 7,
    [ValidateSet("Release", "Debug", "ReleaseSC")][string]$Config = "Release",
    [switch]$Forward,
    [string]$PrimaryArchive = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "app\bin\$Config\LycanZip.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe" }
$work = Join-Path ([IO.Path]::GetTempPath()) "lycan-startup-timing"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$work\settings", "$work\log" | Out-Null
$env:LYCAN_SETTINGS_DIR = "$work\settings"; $env:LYCAN_LOG_DIR = "$work\log"; $env:LYCAN_LOG = "debug"
$env:LYCAN_NO_WELCOME = "1"; $env:LYCAN_NO_STALE_CHECK = "1"; $env:LYCAN_NO_DIALOGS = "1"; $env:LYCAN_TEST_STORE_INSTALLED = "0"
$last = if ($Archive) { "archive drawn" } else { "first frame" }
$all = @{}
$order = New-Object System.Collections.Generic.List[string]
function Read-Log { @(Get-ChildItem "$work\log" -File | ForEach-Object { Get-Content $_.FullName -Encoding UTF8 }) }
function Add-Value($k, $v) { if (-not $all.ContainsKey($k)) { $all[$k] = New-Object System.Collections.Generic.List[int]; $order.Add($k) }; $all[$k].Add([int]$v) }

if ($Forward) {
    if (-not $Archive) { throw "-Forward needs -Archive" }
    $primaryLast = if ($PrimaryArchive) { "archive drawn" } else { "first frame" }
    for ($run = 1; $run -le $Runs; $run++) {
        Get-ChildItem "$work\log" -File | Remove-Item
        $p1 = if ($PrimaryArchive) { Start-Process $exe -ArgumentList "`"$PrimaryArchive`"" -PassThru } else { Start-Process $exe -PassThru }
        $end = (Get-Date).AddSeconds(30)
        while (-not ((Read-Log) -match "startup: $primaryLast at") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 200 }
        Start-Sleep -Seconds 2
        $t0 = Get-Date
        $p2 = Start-Process $exe -ArgumentList "`"$Archive`"" -PassThru
        $end = (Get-Date).AddSeconds(30)
        $tail = @()
        do {
            Start-Sleep -Milliseconds 200
            $tail = @(Read-Log | Where-Object { $_ -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})' -and [datetime]::ParseExact($Matches[1], "yyyy-MM-dd HH:mm:ss.fff", $null) -ge $t0 })
        } while (-not ($tail -match "view: archive drawn|startup: archive drawn") -and (Get-Date) -lt $end)
        $p2.WaitForExit(5000) | Out-Null
        if ($p2.HasExited) { Add-Value "second process ended" ($p2.ExitTime - $t0).TotalMilliseconds }
        foreach ($l in $tail) {
            $at = ([datetime]::ParseExact($l.Substring(0, 23), "yyyy-MM-dd HH:mm:ss.fff", $null) - $t0).TotalMilliseconds
            if ($l -match "forward: a request arrived") { Add-Value "request arrived" $at }
            elseif ($l -match "window: shown") { Add-Value "window shown" $at }
            elseif ($l -match "INFO\s+opened .+ ms$") { Add-Value "archive opened" $at }
            elseif ($l -match "view: archive drawn|startup: archive drawn") { Add-Value "archive drawn" $at }
        }
        if (-not ($tail -match "archive drawn")) { Write-Warning "run $run did not draw the archive in 30 seconds" }
        Get-Process LycanZip -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force; Start-Sleep -Milliseconds 700
    }
    Remove-Item Env:LYCAN_SETTINGS_DIR, Env:LYCAN_LOG_DIR, Env:LYCAN_LOG
    "{0,-30} {1,8} {2,8} {3,8}   ({4} runs, {5}, forwarded; ms since the second process was started)" -f "step", "median", "min", "max", $Runs, $Config
    foreach ($k in $order) { $v = @($all[$k] | Sort-Object); "{0,-30} {1,8} {2,8} {3,8}" -f $k, $v[[int][Math]::Floor($v.Count / 2)], $v[0], $v[-1] }
    return
}
for ($run = 1; $run -le $Runs; $run++) {
    Get-ChildItem "$work\log" -File | Remove-Item
    $p = if ($Archive) { Start-Process $exe -ArgumentList "`"$Archive`"" -PassThru } else { Start-Process $exe -PassThru }
    $end = (Get-Date).AddSeconds(30); $lines = @()
    do {
        Start-Sleep -Milliseconds 200
        $lines = @(Get-ChildItem "$work\log" -File | ForEach-Object { Get-Content $_.FullName -Encoding UTF8 } | Where-Object { $_ -match "startup: " })
    } while (-not ($lines -match "startup: $last at") -and (Get-Date) -lt $end)
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
    foreach ($l in $lines) {
        if ($l -match "startup: (.+) at (-?\d+) ms") {
            $k = $Matches[1]; if (-not $all.ContainsKey($k)) { $all[$k] = New-Object System.Collections.Generic.List[int]; $order.Add($k) }
            $all[$k].Add([int]$Matches[2])
        }
    }
    if (-not ($lines -match "startup: $last at")) { Write-Warning "run $run did not reach '$last' in 30 seconds" }
}
Remove-Item Env:LYCAN_SETTINGS_DIR, Env:LYCAN_LOG_DIR, Env:LYCAN_LOG
"{0,-30} {1,8} {2,8} {3,8}   ({4} runs, {5})" -f "step", "median", "min", "max", $Runs, $Config
foreach ($k in $order) {
    $v = @($all[$k] | Sort-Object)
    "{0,-30} {1,8} {2,8} {3,8}" -f $k, $v[[int][Math]::Floor($v.Count / 2)], $v[0], $v[-1]
}
