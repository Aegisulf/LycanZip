# Passwords: zips made by LycanZip, extracted by File Explorer's own zip support (zipfldr, through Shell.Application).
#   .\tests\robust\explorer_password.ps1 -Cli <lycanzip-cli.exe>
# Explorer cannot make encrypted zips, so this is one direction only. Its password dialog is filled in by UI Automation:
# do not touch the keyboard or the mouse while it runs. Exit code = number of unexpected results.
param([Parameter(Mandatory)][string]$Cli)
$ErrorActionPreference = "Continue"   # the CLI writes its progress to stderr
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$AE = [Windows.Automation.AutomationElement]; $TS = [Windows.Automation.TreeScope]

$work = Join-Path $env:TEMP ("lycan_explorer_pw_" + $PID)
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory "$work\src" | Out-Null
$content = "the quick brown fox`r`n" * 50
[IO.File]::WriteAllText("$work\src\data.txt", $content)

$passwords = [ordered]@{
    "plain" = "secret123"; "space inside" = "two words here"; "punctuation" = '^%&!$#@()[]{}<>|;,.=+-_~`'
    "single quote" = "it's"; "double quote" = 'say "hi" now'; "backslashes" = 'C:\path\to\'; "64 characters" = ("k" * 64)
}
$formats = @(@{ Label = "ZipCrypto"; Args = @("--profile", "compatible"); Expect = $true }, @{ Label = "AES-256"; Args = @(); Expect = $false })

# Extracts $zip into $out with Shell.Application in a child process; types $pw into the password dialog when it comes.
# Returns "ok", "no dialog", "refused" (Explorer shows an error), or "wrong" (the file did not come out right)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PwUi {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
function ById($root, $id) { $root.FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id))) }

# Extracts $zip into $out with Shell.Application in a child process (flags 0: with flags like "no confirmation" Explorer
# fails with 0x80004005 instead of asking). The password dialog (field 1051, OK 1) is filled in with window messages.
# Returns "ok", "no dialog", "asked again" (the password was not taken), "error" (Explorer showed an error) or "wrong".
function Try-Explorer($zip, $out, $pw) {
    New-Item -ItemType Directory $out | Out-Null
    $code = "`$s = New-Object -ComObject Shell.Application; `$s.Namespace('$out').CopyHere(`$s.Namespace('$zip').Items(), 0); Start-Sleep -Seconds 3"
    $p = Start-Process powershell -ArgumentList "-NoProfile", "-STA", "-Command", $code -PassThru
    $state = "no dialog"; $end = (Get-Date).AddSeconds(30); $typed = 0
    $cond = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
    while (-not $p.HasExited -and (Get-Date) -lt $end) {
        Start-Sleep -Milliseconds 400
        foreach ($w in $AE::RootElement.FindAll($TS::Children, $cond)) {
            $field = ById $w "1051"
            if ($field) {
                if ($typed -ge 1) { $state = "asked again"; $c = ById $w "2"; if ($c) { [PwUi]::PostMessageW([IntPtr]$c.Current.NativeWindowHandle, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }; continue }
                [PwUi]::SendMessageW([IntPtr]$field.Current.NativeWindowHandle, 0x000C, [IntPtr]::Zero, $pw) | Out-Null   # WM_SETTEXT
                Start-Sleep -Milliseconds 200
                $ok = ById $w "1"
                [PwUi]::PostMessageW([IntPtr]$ok.Current.NativeWindowHandle, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK
                $typed++; $state = "typed"
                Start-Sleep -Milliseconds 800
            } elseif ((ById $w "16779") -or (ById $w "65535")) {
                # an error ("unexpected error", "cannot copy"): close it
                $script:lastError = ($w.FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name }) -join " / "
                $c = ById $w "2"; if ($c) { [PwUi]::PostMessageW([IntPtr]$c.Current.NativeWindowHandle, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
                if ($state -ne "asked again") { $state = "error" }
            }
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    $f = Get-ChildItem $out -Recurse -Filter data.txt | Select-Object -First 1
    if ($f -and [IO.File]::ReadAllText($f.FullName) -eq $content) { return "ok" }
    if ($state -eq "typed") { return "wrong" }
    return $state
}

$bad = 0; $n = 0
"{0,-15} | {1,-12} | {2,-12}" -f "", "ZipCrypto", "AES-256"
foreach ($k in $passwords.Keys) {
    $cells = @()
    foreach ($f in $formats) {
        $n++
        $zip = "$work\t$n.zip"
        $pwArg = $passwords[$k] -replace '"', '\"'   # Windows PowerShell 5.1 drops the quotes inside an argument otherwise
        & $Cli a $zip "$work\src" -p $pwArg -y @($f.Args) 2>$null | Out-Null
        if ($LASTEXITCODE) { $cells += "NOCREATE"; $bad++; continue }
        $r = Try-Explorer $zip "$work\out$n" $passwords[$k]
        $expected = if ($f.Expect) { $r -eq "ok" } else { $r -ne "wrong" }   # AES: Explorer may not support it, but must not produce a wrong file
        if (-not $expected) { $bad++ }
        $cells += $(if ($expected) { $r } else { "UNEXPECTED $r" })
    }
    "{0,-15} | {1,-12} | {2,-12}" -f $k, $cells[0], $cells[1]
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
"unexpected: $bad"
exit $bad
