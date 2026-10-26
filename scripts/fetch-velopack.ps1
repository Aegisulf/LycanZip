# Downloads the Velopack C/C++ library (the installer and update framework) and verifies it against a fixed SHA256 (the hash comes from the asset
# digest of the official GitHub release). The headers and the x64 DLL with its import library go to third_party/velopack (not in the repository).
# Also installs the packer, the .NET tool `vpk`, of the same version into build/tools/vpk (needs the .NET SDK) with -Packer.
param(
    [string]$Version = "1.2.161",
    [switch]$Packer
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

# version -> SHA256 of velopack_libc_<version>.zip. To upgrade add a row (hash: the asset digest at https://github.com/velopack/velopack/releases)
$pins = @{
    "1.2.161" = "31DE8F2CF05A47BD5328A9F98B0C41AEC1582156F6075F1058B26941F7F17C20"
}
if (-not $pins.ContainsKey($Version)) { throw "There is no pinned hash for $Version, add it to `$pins in scripts\fetch-velopack.ps1 first" }

if ($Packer) {
    $dest = Join-Path $root "build\tools\vpk"
    $exe = Join-Path $dest "vpk.exe"
    if (Test-Path $exe) { Write-Host "vpk already present: $exe"; return }
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { throw "The .NET SDK is needed for the packer (winget install Microsoft.DotNet.SDK.9)" }
    New-Item -ItemType Directory -Force $dest | Out-Null
    & dotnet tool install vpk --version $Version --tool-path $dest | Out-Host
    if (-not (Test-Path $exe)) { throw "vpk was not installed" }
    return
}

$dest = Join-Path $root "third_party\velopack"
if ((Test-Path (Join-Path $dest "include\Velopack.hpp")) -and (Test-Path (Join-Path $dest "lib\velopack_libc_win_x64_msvc.dll")) -and
    ((Get-Content (Join-Path $dest "VERSION.txt") -ErrorAction SilentlyContinue) -like "*$Version*")) { Write-Host "Velopack library already present."; return }

$name = "velopack_libc_$Version.zip"
$tmp = Join-Path $env:TEMP $name
if (-not (Test-Path $tmp) -or (Get-FileHash $tmp -Algorithm SHA256).Hash -ne $pins[$Version]) {
    Invoke-WebRequest "https://github.com/velopack/velopack/releases/download/$Version/$name" -OutFile $tmp -UseBasicParsing
}
$hash = (Get-FileHash $tmp -Algorithm SHA256).Hash
if ($hash -ne $pins[$Version]) { throw "SHA256 does not match ($name): $hash" }

$x = Join-Path $env:TEMP "velopack_libc_$Version"
Remove-Item $x -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $tmp -DestinationPath $x -Force
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $dest "include"), (Join-Path $dest "lib") | Out-Null
Copy-Item (Join-Path $x "include\*") (Join-Path $dest "include")
foreach ($f in "velopack_libc_win_x64_msvc.dll", "velopack_libc_win_x64_msvc.dll.lib") { Copy-Item (Join-Path $x "lib\$f") (Join-Path $dest "lib") }
Set-Content (Join-Path $dest "VERSION.txt") "Velopack $Version ($name, SHA256 $hash)"
Remove-Item $x -Recurse -Force
Write-Host "Extracted to $dest"
