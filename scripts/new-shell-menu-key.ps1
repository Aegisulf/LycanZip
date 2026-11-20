# Makes the certificate that signs the Windows 11 menu package of the installer and zip versions (LycanZipShell.msix).
# The Store version does not need it (the Store signs its package).
#   - A self-signed code signing certificate, Subject "CN=Aegisulf LycanZip" (the Publisher of shellext\AppxManifest.xml.in), valid 10 years.
#   - The private key goes to %USERPROFILE%\.lycanzip-signing\shell-menu.pfx with a random password in shell-menu-password.txt next to it
#     (only the current user can read the folder). Back both up offline: without them the next versions cannot be signed with the same key
#     (users would have to trust a new certificate), and whoever has them can sign packages that the computers which trust it accept.
#     Never committed, never printed. The certificate is removed from the store of the user again: only the .pfx keeps the key.
#   - The public certificate goes to packaging\shell-menu\LycanZipShell.cer (committed: the program asks the user to trust it).
# Usage: .\scripts\new-shell-menu-key.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $env:USERPROFILE ".lycanzip-signing"
$pfx = Join-Path $dir "shell-menu.pfx"
$pwFile = Join-Path $dir "shell-menu-password.txt"
$cer = Join-Path $root "packaging\shell-menu\LycanZipShell.cer"
if ((Test-Path $pfx) -and -not $Force) { throw "A key exists already: $pfx (a new one makes every user trust a new certificate; use -Force only on purpose)" }

$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=Aegisulf LycanZip" -FriendlyName "LycanZip Windows 11 menu package" `
    -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10) `
    -CertStoreLocation "Cert:\CurrentUser\My" -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$bytes = New-Object byte[] 24
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$pw = [Convert]::ToBase64String($bytes).Replace("+", "A").Replace("/", "B").TrimEnd("=")

New-Item -ItemType Directory -Force $dir | Out-Null
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $pw -AsPlainText -Force) | Out-Null
[IO.File]::WriteAllText($pwFile, $pw)
& icacls $dir /inheritance:r /grant:r "${env:USERNAME}:(OI)(CI)F" | Out-Null
& icacls $pfx /inheritance:r /grant:r "${env:USERNAME}:F" | Out-Null
& icacls $pwFile /inheritance:r /grant:r "${env:USERNAME}:F" | Out-Null
New-Item -ItemType Directory -Force (Split-Path $cer) | Out-Null
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey

Write-Host "Private key: $pfx and its password in $pwFile (back both up offline)"
Write-Host "Public certificate: $cer (commit it)"
Write-Output "Thumbprint: $($cert.Thumbprint), valid until $($cert.NotAfter.ToString('yyyy-MM-dd'))"
