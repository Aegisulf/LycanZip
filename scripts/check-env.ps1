# Checks whether this computer can build / test / package LycanZip and lists what is missing and how to fix it. Read only: installs and changes nothing.
# Usage: .\scripts\check-env.ps1 [-Quiet]
#   [FAIL] = missing, cannot build; [WARN] = affects only some functions (tests, packaging, running); [ OK ] = fine.
#   Exit code = number of FAILs (0 = can build). See the README, section Build.
param([switch]$Quiet)
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$script:fail = 0; $script:warn = 0

function Report($level, $what, $hint = "") {
    $tag = switch ($level) { "ok" { "[ OK ]" } "warn" { "[WARN]" } "fail" { "[FAIL]" } default { "[ .. ]" } }
    $color = switch ($level) { "ok" { "Green" } "warn" { "Yellow" } "fail" { "Red" } default { "Gray" } }
    if ($level -eq "fail") { $script:fail++ } elseif ($level -eq "warn") { $script:warn++ }
    if ($Quiet -and $level -eq "ok") { return }
    Write-Host "$tag $what" -ForegroundColor $color
    if ($hint -and $level -ne "ok") { Write-Host "       -> $hint" -ForegroundColor DarkGray }
}
function Section($title) { if (-not $Quiet) { Write-Host ""; Write-Host "== $title ==" -ForegroundColor Cyan } }
function Find-Exe($name, [string[]]$extraDirs = @()) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($d in $extraDirs) { $p = Join-Path $d $name; if (Test-Path $p) { return $p } }
    return $null
}

# ---------------------------------------------------------------- Basics
Section "Basics"
Report "ok" "PowerShell $($PSVersionTable.PSVersion) (the build scripts run in Windows PowerShell 5.1 and 7)"
if (-not (Test-Path (Join-Path $root "VERSION"))) { Report "fail" "VERSION not found: this folder does not look like the root of the LycanZip project" "Run it from scripts\ in the project root" }
$git = Find-Exe "git.exe"
if ($git) {
    Push-Location $root
    $remote = (& git remote 2>$null)
    $hasRepo = Test-Path (Join-Path $root ".git")
    Pop-Location
    if (-not $hasRepo) { Report "warn" "No .git folder: the history is not visible, and git diff / log do not work" "When moving to another machine take .git along, or use git bundle / a remote repository (the README, section Build)" }
    elseif (-not $remote) { Report "warn" "This repository has no remote: the commits exist only in this one folder" "Make a private remote repository and push, or make git bundle backups now and then" }
    else { Report "ok" "git and remote repository: $($remote -join ', ')" }
} else { Report "warn" "git not found" "Install Git for Windows (used only to look at the history and for backups, the build does not need it)" }

# ---------------------------------------------------------------- Visual Studio / SDK
Section "Visual Studio Build Tools and the Windows SDK"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (-not (Test-Path $vswhere)) {
    Report "fail" "vswhere not found (Visual Studio / Build Tools are not installed)" "Install the Visual Studio 2022 Build Tools: choose the workload Desktop development with C++, and tick the Windows 11 SDK"
} else {
    $vsPath = (& $vswhere -latest -products * -property installationPath) | Select-Object -First 1
    $vsVer = (& $vswhere -latest -products * -property catalog_productDisplayVersion) | Select-Object -First 1
    if (-not $vsPath) { Report "fail" "vswhere found no Visual Studio installation" "Install the Visual Studio 2022 Build Tools" }
    else {
        Report "ok" "Visual Studio: $vsPath ($vsVer)"
        $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvars) { Report "ok" "MSVC tools (vcvars64.bat)" }
        else { Report "fail" "No C++ compiler (vcvars64.bat not found)" "Add the workload Desktop development with C++ in the Visual Studio Installer" }
        $msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path $msbuild) { Report "ok" "MSBuild" } else { Report "fail" "MSBuild.exe not found" "Add MSBuild in the Visual Studio Installer (it comes with the C++ workload)" }
        # The XAML targets of native WinUI (only in the full VS): this project connects the XAML compiler itself, and having both can conflict
        $xamlTargets = Join-Path $vsPath "MSBuild\Microsoft\WindowsXaml"
        if (Test-Path $xamlTargets) { Report "warn" "Found MSBuild\Microsoft\WindowsXaml (full Visual Studio)" "The project detects by itself whether to use its own XAML connection (custom) or the native one of Visual Studio (native), and prints XAML compile connection: ... when it builds; if that fails set it by hand with .\scripts\build-app.ps1 -XamlMode custom or native (see the README, section Build)" }
    }
}
$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
$sdkVers = if (Test-Path (Join-Path $kits "Include")) { Get-ChildItem (Join-Path $kits "Include") -Directory | Where-Object { $_.Name -like "10.*" } | Sort-Object Name -Descending } else { @() }
if ($sdkVers) { Report "ok" "Windows SDK: $($sdkVers[0].Name)" } else { Report "fail" "Windows 10/11 SDK not found" "Add the Windows 11 SDK in the Visual Studio Installer" }
$makeappx = Get-ChildItem (Join-Path $kits "bin") -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName "x64\makeappx.exe") } | Select-Object -First 1
if ($makeappx) { Report "ok" "makeappx (for MSIX packaging)" } else { Report "warn" "makeappx.exe not found" "Affects only MSIX packaging; make sure the Windows SDK has the MSIX packaging tools installed" }

# ---------------------------------------------------------------- CMake / Ninja
Section "CMake and Ninja"
$vsCmakeDirs = @()
if ($vsPath) { $vsCmakeDirs += (Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"); $vsCmakeDirs += (Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") }
$cmake = Find-Exe "cmake.exe" $vsCmakeDirs
$ninja = Find-Exe "ninja.exe" $vsCmakeDirs
if ($cmake) { Report "ok" "CMake: $cmake" } else { Report "fail" "cmake not found" "Install CMake and add it to PATH (or tick C++ CMake tools for Windows in the Visual Studio Installer)" }
if ($ninja) { Report "ok" "Ninja: $ninja" } else { Report "fail" "ninja not found" "Install Ninja and add it to PATH (or tick C++ CMake tools for Windows, which includes Ninja)" }
if ($cmake -and $vsCmakeDirs -contains (Split-Path $cmake -Parent) -and -not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    Report "warn" "cmake / ninja are only in a folder of Visual Studio, not in PATH" "scripts\build.ps1 loads vcvars64 first (which adds them to PATH), so this is normally fine; if not, add them to PATH yourself"
}
# An old build folder: CMake caches absolute paths, so it fails after the folder was moved
$cache = Join-Path $root "build\debug\CMakeCache.txt"
if (Test-Path $cache) {
    $cacheHome = (Select-String -Path $cache -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$" | Select-Object -First 1)
    if ($cacheHome) {
        $cached = $cacheHome.Matches[0].Groups[1].Value
        $now = (Resolve-Path $root).Path
        if ($cached.TrimEnd('\','/').Replace('/', '\') -ne $now.TrimEnd('\')) {
            Report "fail" "build\ was built in another path ($cached), now it is $now" "Delete the whole build\ folder and build again (CMake cached the absolute path)"
        } else { Report "ok" "The CMake cache path of build\ matches the current place" }
    }
}

# ---------------------------------------------------------------- Third-party files and NuGet
Section "Third-party files (not in Git, get them again on a new computer)"
$hdr = Join-Path $root "third_party\7zip\CPP\7zip\Archive\IArchive.h"
if (Test-Path $hdr) { Report "ok" "7-Zip interface headers (third_party\7zip)" } else { Report "fail" "The 7-Zip interface headers are missing" "Run .\scripts\fetch-7zip.ps1 (downloads and verifies SHA256)" }
$dll = Join-Path $root "third_party\7zip-bin\7z.dll"
if (Test-Path $dll) { Report "ok" "7z.dll (third_party\7zip-bin)" } else { Report "warn" "7z.dll is missing (third_party\7zip-bin)" "Run .\scripts\fetch-7zip.ps1 -Binaries (needed for packaging and running; or set LYCAN_7Z_DLL, or install 7-Zip)" }
$pkgs = Join-Path $root "app\packages"
$props = Join-Path $root "app\packages.props"
if ((Test-Path $pkgs) -and (Test-Path $props)) { Report "ok" "NuGet packages (app\packages, packages.props)" }
else { Report "warn" "The NuGet packages of WinUI 3 are not restored yet" "Run .\scripts\build-app.ps1 -Restore (or app\restore.ps1; needs the network)" }

# ---------------------------------------------------------------- Test tools
Section "Test tools (only for the automated tests)"
$py = Find-Exe "python.exe"
if ($py) {
    $pyVer = (& $py --version 2>&1) -join " "
    Report "ok" "Python: $pyVer"
    & $py -c "import PIL" 2>$null
    if ($LASTEXITCODE -eq 0) { Report "ok" "Pillow (the UI smoke test uses it to make test pictures)" } else { Report "warn" "Python has no Pillow" "pip install pillow (affects only the picture / animation / EXIF sections of the UI smoke test)" }
} else { Report "warn" "Python 3 not found" "Install Python 3 (the Core tests and the UI smoke test use it)" }
$sevenZ = Find-Exe "7z.exe" @((Join-Path $env:ProgramFiles "7-Zip"), (Join-Path $root "third_party\7zip-bin"))
if ($sevenZ) {
    if (-not (Get-Command 7z.exe -ErrorAction SilentlyContinue)) { Report "warn" "7z.exe is in $sevenZ, but not in PATH" "The UI smoke test calls 7z directly: add its folder to PATH" }
    else { Report "ok" "7z command line: $sevenZ" }
} else { Report "warn" "7z.exe not found" "Install 7-Zip and add it to PATH (the UI smoke test uses it to make test archives)" }

# ---------------------------------------------------------------- Run time
Section "Run time"
$rt = Get-AppxPackage -Name "Microsoft.WindowsAppRuntime.2" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($rt) { Report "ok" "Windows App Runtime 2.x: $($rt.Version)" } else { Report "warn" "Windows App Runtime 2.x is not installed" "Needed to run the standard / portable version; install it from the runtime installer of Windows App SDK 2.5 (the build itself does not need it)" }
$build = [Environment]::OSVersion.Version.Build
if ($build -ge 19041) { Report "ok" "Windows build $build (needs Windows 10 2004 or newer)" } else { Report "fail" "Windows build $build is too old" "Windows App SDK needs Windows 10 version 2004 (build 19041) or newer" }
Report "info" "The UI smoke test needs an interactive desktop, and the result depends on the display scale (DPI); after changing machines run .\scripts\ui-smoke.ps1 again on that machine"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "Ready to build (FAIL 0, WARN $($script:warn)). Next: .\scripts\build.ps1 -Test, then .\scripts\build-app.ps1 -Config Release" -ForegroundColor Green }
else { Write-Host "$($script:fail) item(s) missing (WARN $($script:warn)): fix them as hinted above and build again." -ForegroundColor Red }
exit $script:fail
