# Builds the AddressSanitizer + libFuzzer targets (build\fuzz) and runs them for a fixed time each.
# Usage: .\scripts\fuzz.ps1 [-Seconds 300] [-Targets paths,encoding,open] [-NoBuild] [-CorpusDir D:\path]
# A crash leaves a crash-<hash> file in the corpus folder of that target; rerun the target with that file to reproduce.
param(
    [int]$Seconds = 300,
    [string[]]$Targets = @("paths", "encoding", "open", "iso", "update"),
    [switch]$NoBuild,
    [string]$CorpusDir = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $CorpusDir) { $CorpusDir = Join-Path $root "build\fuzz-corpus" }

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Config Release -BuildName fuzz -CMakeArgs "-DLYCAN_FUZZ=ON"
    if ($LASTEXITCODE) { throw "fuzz build failed" }
}
$bin = Join-Path $root "build\fuzz\bin"

# the ASan runtime DLL must sit next to the executables
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$asan = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll") | Select-Object -Last 1
if (-not $asan) { throw "AddressSanitizer runtime not found (install the 'C++ AddressSanitizer' component)" }
Copy-Item $asan.FullName $bin -Force

& python (Join-Path $root "tests\robust\make_fuzz_seeds.py") $CorpusDir
# the hand-built hostile archives of test_hostile (links, reserved names ...) also go into the corpus of "open",
# with the byte in front that makes fuzz_open give the file the right extension
$hostile = Join-Path $CorpusDir "_hostile"
$env:LYCAN_HOSTILE_OUT = $hostile
& (Join-Path $bin "test_hostile.exe") links names | Out-Null
Remove-Item Env:LYCAN_HOSTILE_OUT
foreach ($f in Get-ChildItem $hostile -File -ErrorAction SilentlyContinue) {
    $sel = if ($f.Extension -eq ".tar") { 2 } else { 0 }
    [IO.File]::WriteAllBytes((Join-Path $CorpusDir "open\hostile-$($f.Name)"), [byte[]](@($sel) + [IO.File]::ReadAllBytes($f.FullName)))
}
$env:Path += ";C:\Program Files\7-Zip"
$failed = @()
$ErrorActionPreference = "Continue"   # libFuzzer writes its progress to stderr
foreach ($t in $Targets) {
    $dir = Join-Path $CorpusDir $t
    Write-Host "=== fuzz_$t for $Seconds s (corpus $dir) ==="
    Push-Location $dir
    & (Join-Path $bin "fuzz_$t.exe") "-max_total_time=$Seconds" "-timeout=30" "-rss_limit_mb=3072" "-max_len=65536" "-print_final_stats=1" "-artifact_prefix=$dir\" $dir
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc) { $failed += "$t (exit $rc)"; Write-Host "FUZZ FAILURE in $t, exit code $rc" }
}
if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "fuzzing finished without findings"
