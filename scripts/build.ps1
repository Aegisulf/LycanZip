# Loads the MSVC environment and builds / tests with CMake + Ninja.
# Usage: .\scripts\build.ps1 [-Config Debug|Release] [-Test] [-Clean]
param(
    [ValidateSet("Debug", "Release")][string]$Config = "Debug",
    [switch]$Test,
    [switch]$Clean,
    [string]$BuildName = "",        # build folder name under build\ (default: the config name); lets a second build tree coexist
    [string[]]$CMakeArgs = @(),     # extra cmake configure arguments, e.g. -DLYCAN_SANITIZE=ON
    [string]$Target = ""            # build only this target
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -property installationPath
$vcvars = if ($vsPath) { Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat" }
if (-not $vcvars -or -not (Test-Path $vcvars)) { throw "No Visual Studio / Build Tools with MSVC found" }

# Import the environment variables of vcvars64 into the current PowerShell
$env:Path += ";${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$ErrorActionPreference = "Continue"   # the stderr noise of vcvars must not stop the script
# The app (app\LycanZip.vcxproj) is built by MSBuild with the platform toolset v143 (MSVC 14.4x). Core must come from the same toolset: when the
# newest MSVC is a later one (Visual Studio 2026 has 14.5x next to 14.4x) the static library asks for runtime functions that v143 does not have
# (LNK2001 __std_find_not_ch_2 ...). So take the newest 14.4x when it is installed; otherwise the default.
$vcArg = ""
$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
if (Test-Path $msvcRoot) {
    $v143 = Get-ChildItem $msvcRoot -Directory | Where-Object { $_.Name -match '^14\.4\d\.' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($v143) { $vcArg = " -vcvars_ver=$($v143.Name)" }
}
$envDump = cmd /c "`"$vcvars`"$vcArg >nul 2>nul && set"
$ErrorActionPreference = "Stop"
$envDump | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "Loading the MSVC environment failed (cl.exe not found)" }

$build = Join-Path $root ("build\" + $(if ($BuildName) { $BuildName } else { $Config.ToLower() }))
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" @CMakeArgs
if ($LASTEXITCODE) { throw "cmake configure failed" }
if ($Target) { cmake --build $build --target $Target } else { cmake --build $build }
if ($LASTEXITCODE) { throw "build failed" }
if ($Test) {
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw "tests failed" }
}
