# Makes ISO images with the CLI, mounts them with Windows itself (Mount-DiskImage, read-only, no registration of anything), compares every file
# with the source (names, sizes, SHA-256) and dismounts. Needs no administrator rights for an ISO. Usage: .\scripts\iso-mount-check.ps1 [-Config Release|Debug]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $root ("build\" + $Config.ToLower() + "\bin\lycanzip-cli.exe")
$work = Join-Path $env:TEMP "lycan_isomount"; Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
$src = Join-Path $work "src"; New-Item -ItemType Directory $src | Out-Null
$failures = 0
function Fail($m) { Write-Host "   *** $m"; $script:failures++ }

# the source: nested folders, long and non-ASCII names, an empty folder, an empty file, a file over 4 MB, 8.3 clashes
$rnd = New-Object Random 9
function Rand-File($p, $n) { $d = Split-Path $p; New-Item -ItemType Directory $d -Force | Out-Null; $b = New-Object byte[] $n; $rnd.NextBytes($b); [IO.File]::WriteAllBytes($p, $b) }
Rand-File "$src\readme.txt" 1234
Rand-File "$src\big.bin" (7MB + 123)
Rand-File "$src\empty.dat" 0
Rand-File "$src\docs\report final version 2026.docx" 50000
Rand-File ("$src\" + [string]::Join("", [char[]](0x4E2D, 0x6587)) + "\" + [string]::Join("", [char[]](0x6A94, 0x6848)) + ".txt") 999
Rand-File "$src\deep\a\b\c\d\e\f\g\file.bin" 777
Rand-File "$src\clash\longfilename_one.txt" 10
Rand-File "$src\clash\longfilename_two.txt" 11
Rand-File "$src\clash\longfilename_three.txt" 12
New-Item -ItemType Directory "$src\empty folder" | Out-Null

foreach ($case in @("plain")) {
    $iso = Join-Path $work "t-$case.iso"
    Write-Host "case ${case}:"
    $ErrorActionPreference = "Continue"
    $o = & $cli a $iso $src -t iso -y 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $iso)) { Fail "the image was not made: $o"; continue }
    $img = $null
    try {
        $img = Mount-DiskImage -ImagePath $iso -StorageType ISO -Access ReadOnly -PassThru
        $vol = $img | Get-Volume
        $letter = $vol.DriveLetter
        Write-Host ("   mounted as {0}: label '{1}', {2:N1} MB" -f $letter, $vol.FileSystemLabel, ($vol.Size / 1MB))
        $mnt = "${letter}:\"
        # the image holds the folder 'src' (the CLI packs the folder itself) or its content: find where readme.txt is
        $base = (Get-ChildItem $mnt -Recurse -Filter readme.txt -ErrorAction Stop | Select-Object -First 1).DirectoryName
        $a = Get-ChildItem $src -Recurse -Force | ForEach-Object { $_.FullName.Substring($src.Length + 1) }
        $b = Get-ChildItem $base -Recurse -Force | ForEach-Object { $_.FullName.Substring($base.Length + 1) }
        $missing = @($a | Where-Object { $b -notcontains $_ }); $extra = @($b | Where-Object { $a -notcontains $_ })
        if ($missing.Count) { Fail "missing in the mounted image: $($missing -join ', ')" }
        if ($extra.Count) { Fail "extra in the mounted image: $($extra -join ', ')" }
        $bad = 0; $n = 0
        foreach ($f in Get-ChildItem $src -Recurse -File) {
            $rel = $f.FullName.Substring($src.Length + 1); $m = Join-Path $base $rel
            if (-not (Test-Path -LiteralPath $m)) { continue }
            $n++
            if ((Get-Item -LiteralPath $m).Length -ne $f.Length -or (Get-FileHash -LiteralPath $m).Hash -ne (Get-FileHash -LiteralPath $f.FullName).Hash) { $bad++; Fail "different content: $rel" }
        }
        Write-Host "   $n files compared, $bad different"
        # the long names with 8.3 clashes come out as they were (Joliet): the three names are all there
        if (@(Get-ChildItem "$base\clash").Count -ne 3) { Fail "the clash folder does not have its 3 files" }
        # read-only: writing must fail
        $wrote = $true; try { [IO.File]::WriteAllText("$mnt\x.txt", "x") } catch { $wrote = $false }
        if ($wrote) { Fail "the image is writable?" }
    } catch { Fail "error: $($_.Exception.Message)" }
    finally { if ($img) { Dismount-DiskImage -ImagePath $iso | Out-Null } }
}
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failures) { Write-Host "RESULT: $failures problem(s)"; exit 1 } else { Write-Host "RESULT: ok" }
