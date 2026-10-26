# Downloads 7-Zip and verifies it against a fixed SHA256 (the hashes come from the asset digest of the official GitHub release).
#   Default: the sources (only the headers are used) -> third_party/7zip
#   -Binaries: the x64 installer, unpacks 7z.dll and the license documents -> third_party/7zip-bin (shipped by the packaging scripts, same version as the headers)
param(
    [string]$Version = "26.05",
    [switch]$Binaries
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

# version -> (file name, SHA256). To upgrade 7-Zip add a row here (hashes: see the asset digest at https://github.com/ip7z/7zip/releases)
$pins = @{
    "26.05" = @{
        src = @{ name = "7z2605-src.7z";  sha256 = "FA081A1A33186158D71E9E24F494ED852EF8D0C9D06E21A86FB4713D94B55AE9" }
        x64 = @{ name = "7z2605-x64.exe"; sha256 = "FD99E86F57D8F429EF5ED045BF98315AF962E2309B0F1BA87A147A44C71647D0" }
    }
    "26.04" = @{
        src = @{ name = "7z2604-src.7z";  sha256 = "6BCFD2679D178174DF06D09D50147DD7B5980D7BEAC896136689DFC9D51E0FD9" }
        x64 = @{ name = "7z2604-x64.exe"; sha256 = "D54BF805F9F3704D1E8DB2FA3498AE7EF2DF0312B40B558E7C71C734430A665D" }
    }
    "26.03" = @{
        src = @{ name = "7z2603-src.7z";  sha256 = "41E2A7C0E9F838351C625E01F0F581A2188BB3DDA10C8E0B4A852DA26546FFE2" }
        x64 = @{ name = "7z2603-x64.exe"; sha256 = "0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD" }
    }
}
if (-not $pins.ContainsKey($Version)) { throw "There is no pinned hash for $Version, add it to `$pins in scripts\fetch-7zip.ps1 first" }
$pin = $pins[$Version]

function Get-Verified($asset) {
    $url = "https://github.com/ip7z/7zip/releases/download/$Version/$($asset.name)"
    $tmp = Join-Path $env:TEMP $asset.name
    if (-not (Test-Path $tmp) -or (Get-FileHash $tmp -Algorithm SHA256).Hash -ne $asset.sha256) {
        Invoke-WebRequest $url -OutFile $tmp -UseBasicParsing
    }
    $hash = (Get-FileHash $tmp -Algorithm SHA256).Hash
    if ($hash -ne $asset.sha256) { throw "SHA256 does not match ($($asset.name)): $hash" }
    return $tmp
}

$sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) { $sevenZip = "$env:ProgramFiles\7-Zip\7z.exe" }
if (-not (Test-Path $sevenZip)) { throw "A 7z.exe is needed to unpack the downloaded file (install 7zip with Scoop / winget)" }

if ($Binaries) {
    $dest = Join-Path $root "third_party\7zip-bin"
    if (Test-Path (Join-Path $dest "7z.dll")) { Write-Host "7-Zip binaries already present."; return }
    $exe = Get-Verified $pin.x64
    & $sevenZip x $exe "-o$dest" -y 7z.dll 7z.exe License.txt readme.txt History.txt | Out-Null
    if (-not (Test-Path (Join-Path $dest "7z.dll"))) { throw "Unpacking failed" }
    Set-Content (Join-Path $dest "VERSION.txt") "7-Zip $Version x64 ($($pin.x64.name), SHA256 $($pin.x64.sha256))"
    Write-Host "Extracted to $dest"
    return
}

$dest = Join-Path $root "third_party\7zip"
if (Test-Path (Join-Path $dest "CPP\7zip\Archive\IArchive.h")) { Write-Host "7-Zip source already present."; return }
$archive = Get-Verified $pin.src
& $sevenZip x $archive "-o$dest" -y | Out-Null
Write-Host "Extracted to $dest"
