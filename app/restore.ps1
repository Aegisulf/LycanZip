# Restores the NuGet packages that the WinUI 3 project needs (packages.config mode) and makes the MSBuild import files.
# The vcxproj of the VS Build Tools does not support PackageReference, and WindowsAppSDK is a metapackage split into several dependency packages,
# so every dependency package is installed explicitly here and the props / targets of each package are written into packages.pre.props / packages.props / packages.targets.
param([string]$NuGet = "")
$ErrorActionPreference = "Stop"
# works in the folder of the app and goes back to where it was called from (build-app.ps1 calls it and then goes on with relative paths)
Push-Location $PSScriptRoot
try {

$WinAppSdk = "2.5.1"
$CppWinRT = "2.0.250303.1"
$BuildTools = "10.0.28000.2705"

if (-not $NuGet) {
    $NuGet = (Get-Command nuget -ErrorAction SilentlyContinue).Source
    if (-not $NuGet) {
        $tools = Join-Path $PSScriptRoot ".tools"
        New-Item -ItemType Directory -Force $tools | Out-Null
        $NuGet = Join-Path $tools "nuget.exe"
        if (-not (Test-Path $NuGet)) { Invoke-WebRequest "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $NuGet -UseBasicParsing }
    }
}

function Install($id, $ver) { & $NuGet install $id -Version $ver -OutputDirectory packages -NonInteractive | Out-Null; if ($LASTEXITCODE) { throw "nuget install $id failed" } }
Install "Microsoft.Windows.CppWinRT" $CppWinRT
Install "Microsoft.Windows.SDK.BuildTools" $BuildTools
Install "Microsoft.WindowsAppSDK" $WinAppSdk

# dependency packages of the metapackage (exact versions)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$nupkg = Join-Path "packages\Microsoft.WindowsAppSDK.$WinAppSdk" "Microsoft.WindowsAppSDK.$WinAppSdk.nupkg"
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $nupkg))
$entry = $zip.Entries | Where-Object Name -like "*.nuspec"
$sr = New-Object IO.StreamReader($entry.Open()); $nuspec = $sr.ReadToEnd(); $sr.Close(); $zip.Dispose()
$deps = [regex]::Matches($nuspec, '<dependency id="([^"]+)" version="\[?([^\],"]+)\]?"') | ForEach-Object { , @($_.Groups[1].Value, $_.Groups[2].Value) } | Sort-Object { $_[0] } -Unique
# The AI, ML and Search components of the metapackage are not used by LycanZip: left out (a self-contained package would carry 45 MB of them).
# NuGet installs them as dependencies of the metapackage anyway, so they are deleted again below (and WindowsAppSDKVerifyTransitiveDependencies is off in the project).
$unused = '^(Microsoft\.WindowsAppSDK\.(AI|ML|Search)|Microsoft\.Windows\.AI\.MachineLearning|System\.Numerics\.Tensors|Microsoft\.Bcl\.Numerics)\.'
foreach ($d in $deps) { if ($d[0] -match '\.(AI|ML|Search)$') { continue }; Install $d[0] $d[1] }
Get-ChildItem packages -Directory | Where-Object { $_.Name -match $unused } | ForEach-Object { Remove-Item $_.FullName -Recurse -Force }

# a transitive dependency may pull in an old version: keep only the highest version of each package, and remove the old one of BuildTools
$byId = Get-ChildItem packages -Directory | Group-Object { $_.Name -replace '\.\d+\.\d+\.\d+.*$', '' }
foreach ($g in $byId) {
    if ($g.Count -le 1) { continue }
    $keep = $g.Group | Sort-Object { [version](($_.Name -replace '^.*?\.(\d+\.\d+\.\d+(\.\d+)?).*$', '$1')) } -Descending | Select-Object -First 1
    if ($g.Name -eq 'Microsoft.Windows.SDK.BuildTools') { $keep = $g.Group | Where-Object Name -like "*.$BuildTools" }
    foreach ($d in $g.Group) { if ($d.FullName -ne $keep.FullName) { Remove-Item -LiteralPath $d.FullName -Recurse -Force } }
}

# packages.config: WindowsAppSDK.targets checks that it lists all dependency packages
$cfg = @('<?xml version="1.0" encoding="utf-8"?>', '<packages>')
foreach ($d in (Get-ChildItem packages -Directory | Sort-Object Name)) {
    if ($d.Name -match '^(.+?)\.(\d+\.\d+\.\d+.*)$') { $cfg += ('  <package id="{0}" version="{1}" targetFramework="native" />' -f $Matches[1], $Matches[2]) }
}
$cfg += '</packages>'
$cfg | Set-Content packages.config -Encoding UTF8

# make the import files
$props = @(); $targets = @()
foreach ($d in (Get-ChildItem packages -Directory | Sort-Object Name)) {
    $id = $d.Name -replace '\.\d+\.\d+\.\d+.*$', ''
    foreach ($kind in "props", "targets") {
        $cand = @("packages\$($d.Name)\build\native\$id.$kind", "packages\$($d.Name)\build\$id.$kind") | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $cand) { continue }
        if ($kind -eq "props") { $props += $cand } else { $targets += $cand }
    }
}
function Proj($items) {
    $lines = @()
    foreach ($i in $items) { $lines += ('  <Import Project="{0}" />' -f $i) }
    $head = '<?xml version="1.0" encoding="utf-8"?>' + "`n" + '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
    return $head + "`n" + ($lines -join "`n") + "`n</Project>"
}
$pre = $props | Where-Object { $_ -match 'CppWinRT' }
$rest = $props | Where-Object { $_ -notmatch 'CppWinRT' }
Proj $pre  | Set-Content packages.pre.props -Encoding UTF8
Proj $rest | Set-Content packages.props -Encoding UTF8
Proj $targets | Set-Content packages.targets -Encoding UTF8
"restored: $((Get-ChildItem packages -Directory).Count) packages"
} finally { Pop-Location }
