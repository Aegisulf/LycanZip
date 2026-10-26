# Builds the WinUI 3 app: first the Core with CMake, then app\LycanZip.vcxproj with MSBuild.
# Usage: .\scripts\build-app.ps1 [-Config Debug|Release] [-Restore] [-SelfContained] [-XamlMode auto|custom|native]
#   -SelfContained: the Windows App Runtime goes into the output folder (app\bin\<Config>SC), for the installer and the zip packages
#   -XamlMode: who connects the XAML compiler. auto (default) = detect; custom = the project's own (Build Tools); native = that of Visual Studio (full VS). See app\LycanZip.vcxproj.
param(
    [ValidateSet("Debug", "Release")][string]$Config = "Debug",
    [switch]$Restore,
    [switch]$SelfContained,
    [ValidateSet("auto", "custom", "native")][string]$XamlMode = "auto"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

if ($Restore -or -not (Test-Path (Join-Path $root "app\packages.props"))) { & (Join-Path $root "app\restore.ps1") }

# Core (CMake) - also loads the MSVC environment
& (Join-Path $PSScriptRoot "build.ps1") -Config $Config
$msbuild = Join-Path (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath) "MSBuild\Current\Bin\MSBuild.exe"
& $msbuild /nologo /nr:false /v:minimal /p:Configuration=$Config /p:Platform=x64 $(if ($XamlMode -ne "auto") { "/p:LycanXamlMode=$XamlMode" }) $(if ($SelfContained) { "/p:LycanSelfContained=true" }) (Join-Path $root "app\LycanZip.vcxproj")
if ($LASTEXITCODE) { throw "app build failed" }
Write-Host "OK: app\bin\$Config$(if ($SelfContained) { "SC" })\LycanZip.exe"
