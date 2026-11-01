param([int]$Trials = 8, [Parameter(Mandatory)][string]$Archive)
# Measures the cursor after the file name encoding drop-down opens under a mouse that does not move: WinUI issue
# microsoft/microsoft-ui-xaml#8829 (a windowed popup shows the last non-arrow cursor until the mouse moves). Run it after an
# update of the Windows App SDK; when "After300ms" is ARROW in every trial the issue is fixed upstream.
# Usage: .\scripts\cursor-probe.ps1 -Archive <an old zip with names in a legacy code page> (it uses the real mouse: do not touch it).
# It resizes the window by its right edge (to get the sizing cursor), moves onto the drop-down, waits, clicks without moving and
# reads the cursor; the last column is after the cursor is put where it is (the workaround of app/MenuCursor.h).
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace P -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
[DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr h, int id);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
'@
[P.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$names = @{}
foreach ($c in @(@{n="ARROW";i=32512}, @{n="IBEAM";i=32513}, @{n="WAIT";i=32514}, @{n="SIZEWE";i=32644}, @{n="SIZENS";i=32645}, @{n="SIZENWSE";i=32642}, @{n="APPSTARTING";i=32650}, @{n="HAND";i=32649})) { $names[[int64][P.W]::LoadCursor([IntPtr]::Zero, $c.i)] = $c.n }
function Cur() { $ci = New-Object P.W+CURSORINFO; $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci); [P.W]::GetCursorInfo([ref]$ci) | Out-Null; $h = [int64]$ci.hCursor; if ($names.ContainsKey($h)) { $names[$h] } else { "0x{0:X}" -f $h } }
function Down() { [P.W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero) }
function Up() { [P.W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
function Glide($x0, $y0, $x1, $y1) { for ($i = 1; $i -le 10; $i++) { [P.W]::SetCursorPos([int]($x0 + ($x1 - $x0) * $i / 10), [int]($y0 + ($y1 - $y0) * $i / 10)) | Out-Null; Start-Sleep -Milliseconds 15 } }

$exe = Join-Path (Split-Path $PSScriptRoot -Parent) "app\bin\Debug\LycanZip.exe"
$set = Join-Path $env:TEMP "lycan_cursor_probe"; New-Item -ItemType Directory $set -Force | Out-Null
$env:LYCAN_SETTINGS_DIR = $set; $env:LYCAN_NO_WELCOME = "1"; $env:LYCAN_NO_STALE_CHECK = "1"; $env:LYCAN_TEST_STORE_INSTALLED = "0"; $env:LYCAN_NO_DIALOGS = "1"
Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
$p = Start-Process $exe -ArgumentList "`"$Archive`"" -PassThru
$AE = [Windows.Automation.AutomationElement]
$win = $null; $end = (Get-Date).AddSeconds(20)
do { Start-Sleep -Milliseconds 300; $p.Refresh(); if ($p.MainWindowHandle -ne 0) { $win = $AE::FromHandle($p.MainWindowHandle) } } while (-not $win -and (Get-Date) -lt $end)
$combo = $null; $end = (Get-Date).AddSeconds(15)
do { Start-Sleep -Milliseconds 300; $combo = $win.FindFirst([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::AutomationIdProperty, "EncodingCombo"))) } while ((-not $combo -or $combo.Current.IsOffscreen) -and (Get-Date) -lt $end)
if (-not $combo) { "encoding drop-down not found"; Stop-Process -Id $p.Id -Force; exit 1 }
$h = [IntPtr]$p.MainWindowHandle

$rows = @()
for ($n = 1; $n -le $Trials; $n++) {
    [P.W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
    $r = New-Object P.W+RECT; [P.W]::GetWindowRect($h, [ref]$r) | Out-Null
    # 1. the right edge: the sizing cursor, then a real resize
    $ex = $r.R - 3; $ey = [int](($r.T + $r.B) / 2)
    Glide ($ex - 60) $ey $ex $ey; Start-Sleep -Milliseconds 300
    $edge = Cur
    Down; Glide $ex $ey ($ex + (($n % 2) * 2 - 1) * 30) $ey; Up; Start-Sleep -Milliseconds 400
    # 2. onto the drop-down (it may have moved with the resize)
    $b = $combo.Current.BoundingRectangle
    $cx = [int]($b.X + $b.Width / 2); $cy = [int]($b.Y + $b.Height / 2)
    $p0 = New-Object P.W+POINT; [P.W]::GetCursorPos([ref]$p0) | Out-Null
    Glide $p0.X $p0.Y $cx $cy; Start-Sleep -Milliseconds 400
    $before = Cur
    # 3. click without moving, read at 60 and 300 ms
    Down; Up
    Start-Sleep -Milliseconds 60; $at60 = Cur
    Start-Sleep -Milliseconds 240; $at300 = Cur
    # 4. the workaround of MenuCursor.h: the cursor put where it is
    [P.W]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds 120; $nudged = Cur
    $rows += [pscustomobject]@{ Trial = $n; Edge = $edge; BeforeClick = $before; After60ms = $at60; After300ms = $at300; AfterSetCursorPos = $nudged }
    [P.W]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [P.W]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400   # close the list
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$rows | Format-Table -AutoSize | Out-String -Width 200
