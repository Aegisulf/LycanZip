# Real disk-full test: a small virtual disk (VHD, NTFS, 100 MB) is made and attached as V:, the CLI writes to it until it is full,
# and the script checks that every failure is clean (non-zero exit, no leftover files, an archive that was there is untouched,
# the free space comes back). Needs an elevated PowerShell (diskpart); started without elevation it asks for it (UAC) and waits.
# Usage: .\scripts\disk-full.ps1 [-Config Release|Debug]   Log: build\disk-full-log.txt   The VHD is detached and deleted at the end.
param([string]$Config = "Release", [switch]$Elevated)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root "build\disk-full-log.txt"
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Remove-Item $log -ErrorAction SilentlyContinue
    Write-Host "Asking for administrator rights (diskpart needs them): click Yes in the UAC window."
    Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Config", $Config, "-Elevated"
    if (Test-Path $log) { Get-Content $log -Encoding UTF8 } else { Write-Host "no log: the elevation was refused?"; exit 1 }
    exit $(if ((Get-Content $log -Encoding UTF8 | Select-String "^RESULT: ok")) { 0 } else { 1 })
}

New-Item -ItemType Directory (Split-Path $log) -Force | Out-Null
Set-Content $log "" -Encoding UTF8
function Say($m) { Write-Host $m; for ($i = 0; $i -lt 5; $i++) { try { Add-Content $log $m -Encoding UTF8 -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 200 } } }
$cli = Join-Path $root "build\release\bin\lycanzip-cli.exe"
if ($Config -eq "Debug") { $cli = Join-Path $root "build\debug\bin\lycanzip-cli.exe" }
if (-not (Test-Path $cli)) { Say "no CLI at $cli"; Say "RESULT: failed"; exit 1 }
$work = Join-Path $env:TEMP "lycan_diskfull"; Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $work | Out-Null
$vhd = Join-Path $work "small.vhd"
$drive = "V"
$failures = 0
function Fail($m) { Say "   *** $m"; $script:failures++ }

function Run-Diskpart($lines) {
    $f = Join-Path $work "dp.txt"; Set-Content $f $lines -Encoding ASCII
    $o = & diskpart /s $f 2>&1 | Out-String
    return $o
}

try {
    if (Test-Path "${drive}:\") { Say "drive $drive`: is in use already"; Say "RESULT: failed"; exit 1 }
    $null = Run-Diskpart @("create vdisk file=`"$vhd`" maximum=100 type=expandable", "attach vdisk", "create partition primary", "format fs=ntfs quick label=LZFULL", "assign letter=$drive")
    Start-Sleep -Seconds 2
    if (-not (Test-Path "${drive}:\")) { Say "the virtual disk could not be made"; Say "RESULT: failed"; exit 1 }
    $free0 = (Get-PSDrive $drive).Free
    Say ("virtual disk $drive`: ready, {0:N1} MB free" -f ($free0 / 1MB))

    # sources: 120 MB of random data (does not shrink), 400 MB of text (shrinks to almost nothing), a 25 MB random file
    $rnd = New-Object Random 11
    $buf = New-Object byte[] (8MB)
    $src = Join-Path $work "src"; New-Item -ItemType Directory $src | Out-Null
    $fs = [IO.File]::Create("$src\random120.bin"); 1..15 | ForEach-Object { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Dispose()
    $fs = [IO.File]::Create("$src\random25.bin"); 1..3 | ForEach-Object { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Dispose()
    $line = ("lorem ipsum dolor sit amet " * 40) + "`r`n"
    $fs = [IO.File]::Create("$src\text400.txt"); $b = [Text.Encoding]::ASCII.GetBytes($line); 1..([int](400MB / $b.Length)) | ForEach-Object { $fs.Write($b, 0, $b.Length) }; $fs.Dispose()

    function Leftovers($allowed) {
        @(Get-ChildItem "${drive}:\" -Force -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike "*System Volume Information*" -and $_.FullName -notlike "*`$RECYCLE.BIN*" -and ($allowed -notcontains $_.Name) })
    }
    # Windows gives the space of deleted files back a little later (up to ~20 s on this small disk): wait for it
    function Wait-Free([uint64]$want, [int]$seconds = 180) {
        $end = (Get-Date).AddSeconds($seconds)
        while ((Get-PSDrive $drive).Free -lt $want -and (Get-Date) -lt $end) { Start-Sleep -Seconds 1 }
        return ((Get-PSDrive $drive).Free -ge $want)
    }
    function Disk-Report() {
        Start-Sleep -Milliseconds 1000
        $items = Get-ChildItem "${drive}:\" -Force -ErrorAction SilentlyContinue | ForEach-Object { "{0} {1:N1} MB" -f $_.Name, ($(if ($_.PSIsContainer) { (Get-ChildItem $_.FullName -Force -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum } else { $_.Length }) / 1MB) }
        "free {0:N1} MB; top level: {1}" -f ((Get-PSDrive $drive).Free / 1MB), ($items -join "; ")
    }
    function Clean-Disk() { Get-ChildItem "${drive}:\" -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin "System Volume Information", '$RECYCLE.BIN' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
    function RunCli([string[]]$a) {
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        $o = & $cli @a 2>&1 | Out-String
        $code = $LASTEXITCODE
        $ErrorActionPreference = $old
        return @{ code = $code; text = $o.Trim() }
    }

    # A: compress 120 MB of random data into every format: the disk (about 90 MB free) fills up in the middle
    foreach ($case in @(@("zip", "zip", @()), @("7z", "7z", @()), @("tar", "tar", @()), @("tgz", "tar.gz", @()), @("txz", "tar.xz", @()), @("tbz2", "tar.bz2", @()), @("iso", "iso", @()), @("zip-aes", "zip", @("-p", "secret")), @("7z-split", "7z", @("-v", "10m")))) {
        $label = $case[0]; $out = "${drive}:\out.$($case[1])"
        $r = RunCli (@("a", $out, "$src\random120.bin", "-t", $(if ($case[0] -eq "zip-aes" ) { "zip" } elseif ($case[0] -eq "7z-split") { "7z" } else { $case[0] }), "-y") + $case[2])
        $left = Leftovers @()
        $freeNow = (Get-PSDrive $drive).Free
        $ok = ($r.code -ne 0) -and ($left.Count -eq 0)
        Say ("A {0,-8} exit {1}, leftovers {2}, free {3:N1} MB: {4}" -f $label, $r.code, $left.Count, ($freeNow / 1MB), ($r.text -split "`n" | Select-Object -Last 1))
        if ($r.code -eq 0) { Fail "$label : the archive was 'made' on a disk that cannot hold it" }
        if ($left.Count) { Fail "$label : leftovers: $($left.Name -join ', ')"; Clean-Disk }
        if (-not (Wait-Free ($free0 - 2MB))) { Fail "$label : the free space did not come back: $(Disk-Report)" }
        if ($r.text -notmatch "(?i)space|full|disk|磁碟|空間") { Fail "$label : the message does not say the disk is full: $($r.text)" }
    }

    # B: extract: a 400 MB text file inside a tar.gz (no size is known up front) and a zip (sizes known: refused up front)
    $null = RunCli @("a", "$work\text400.tar.gz", "$src\text400.txt", "-t", "tgz", "-y")
    $null = RunCli @("a", "$work\text400.zip", "$src\text400.txt", "-t", "zip", "-y")
    $null = RunCli @("a", "$work\text400.7z", "$src\text400.txt", "-t", "7z", "-y")
    foreach ($a in "text400.tar.gz", "text400.zip", "text400.7z") {
        $null = Wait-Free ($free0 - 2MB)
        $r = RunCli @("x", "$work\$a", "-o", "${drive}:\x", "-y")
        $left = @(Leftovers @() | Where-Object { -not $_.PSIsContainer })   # an empty output folder is fine
        Say ("B extract {0,-14} exit {1}, leftovers {2}, free {3:N1} MB: {4}" -f $a, $r.code, $left.Count, ((Get-PSDrive $drive).Free / 1MB), ($r.text -split "`n" | Select-Object -Last 1))
        if ($r.code -eq 0) { Fail "$a : extracted 400 MB onto a 90 MB disk?" }
        if ($left.Count) { Fail "$a : leftovers after the failed extraction: $(($left | ForEach-Object { $_.FullName + ' (' + $_.Length + ')' }) -join ', ') -- $(Disk-Report)"; Clean-Disk }
    }

    # C: update an archive that sits on the full disk: the old archive stays whole, no temporary file stays
    foreach ($fmt in "zip", "7z") {
        Clean-Disk
        $null = Wait-Free ($free0 - 2MB)
        $arc = "${drive}:\keep.$fmt"
        $r = RunCli @("a", $arc, "$src\random25.bin", "-t", $fmt, "-y")
        if ($r.code -ne 0) { Fail "$fmt : could not make the 25 MB archive: $($r.text)"; continue }
        $h0 = (Get-FileHash $arc -Algorithm SHA256).Hash
        # 25 MB archive + a new 120 MB random file: the new archive (about 145 MB) cannot be written next to the old one
        $r = RunCli @("u", $arc, "--add", "$src\random120.bin")
        $left = Leftovers @("keep.$fmt")
        $h1 = if (Test-Path $arc) { (Get-FileHash $arc -Algorithm SHA256).Hash } else { "missing" }
        Say ("C update {0,-4} exit {1}, archive {2}, leftovers {3}: {4}" -f $fmt, $r.code, $(if ($h0 -eq $h1) { "untouched" } else { "CHANGED" }), $left.Count, ($r.text -split "`n" | Select-Object -Last 1))
        if ($r.code -eq 0) { Fail "$fmt : the update worked on a disk without room?" }
        if ($h0 -ne $h1) { Fail "$fmt : the archive changed or is gone after the failed update" }
        if ($left.Count) { Fail "$fmt : leftovers: $($left.Name -join ', ')"; }
        $t = RunCli @("t", $arc)
        if ($t.code -ne 0) { Fail "$fmt : the archive does not pass the test after the failed update" }
    }

    # D: the disk is still usable and the free space is back
    Clean-Disk
    $null = Wait-Free ($free0 - 2MB)
    $freeEnd = (Get-PSDrive $drive).Free
    Say ("D free space at the end {0:N1} MB (at the start {1:N1} MB)" -f ($freeEnd / 1MB), ($free0 / 1MB))
    if ($freeEnd -lt $free0 - 2MB) { Fail "the free space did not come back" }
    $r = RunCli @("a", "${drive}:\ok.zip", "$src\random25.bin", "-y")
    if ($r.code -ne 0) { Fail "the disk cannot be used after the failures: $($r.text)" }
} catch {
    Say ("ERROR: " + $_.Exception.Message + " at line " + $_.InvocationInfo.ScriptLineNumber); $failures++
} finally {
    $o = Run-Diskpart @("select vdisk file=`"$vhd`"", "detach vdisk")
    Start-Sleep -Seconds 1
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    Say "the virtual disk is detached and deleted: $(-not (Test-Path "${drive}:\"))"
}
if ($failures) { Say "RESULT: $failures problem(s)" } else { Say "RESULT: ok" }
