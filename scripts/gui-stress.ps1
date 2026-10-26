# GUI stress tests (the "big test", part 2): archives with 100 000 and 400 000 entries, a flood of right-click actions, other display scales.
# Usage: .\scripts\gui-stress.ps1 -Part big|flood|dpi [-Config Release] [-Quick]
# It operates the real windows (UI Automation, keyboard): do not touch the keyboard or the mouse while it runs. Results go to the screen and to
# build\gui-stress-<part>.txt. The "dpi" part changes the display scale of the main monitor for a while and puts it back (try / finally).
param([ValidateSet("big", "flood", "dpi", "cancel")][string]$Part = "big", [ValidateSet("Debug", "Release")][string]$Config = "Release", [switch]$Quick, [string[]]$Only = @())
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "app\bin\$Config\LycanZip.exe"
if (-not (Test-Path $exe)) { throw "no $exe" }
$exeDir = Split-Path $exe
$cliExe = Join-Path $exeDir "lycanzip-cli.exe"
$env:LYCAN_NO_STALE_CHECK = "1"; $env:LYCAN_TEST_STORE_INSTALLED = "0";$env:LYCAN_LANG = "zh-TW"; $env:LYCAN_NO_WELCOME = "1"; $env:LYCAN_TEST_MUTE = "1"
$portable = Join-Path $exeDir "LycanZip.portable"
$settingsFile = Join-Path $exeDir "settings.ini"
$shots = Join-Path $root "build\gui-stress-shots"
New-Item -ItemType Directory -Force $shots, (Join-Path $root "build") | Out-Null
. (Join-Path $PSScriptRoot "ui-helpers.ps1")
$report = New-Object System.Collections.Generic.List[string]
function Say($m) { Write-Host $m; $report.Add($m) }
function Save-Report { Set-Content -Path (Join-Path $root "build\gui-stress-$Part.txt") -Value $report -Encoding UTF8 }

$hadPortable = Test-Path $portable
if (-not $hadPortable) { Set-Content $portable "" }
$oldSettings = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw -Encoding UTF8 } else { $null }
function Restore-State {
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $oldSettings) { Set-Content $settingsFile $oldSettings -Encoding UTF8 } else { Remove-Item $settingsFile -Force -ErrorAction SilentlyContinue }
    if (-not $hadPortable) { Remove-Item $portable -Force -ErrorAction SilentlyContinue }
}

# the time until the process is quiet (its CPU use stays under 8% of a core for 1.5 s), at most $maxSec
$script:probeWin = $null; $script:maxLatency = 0.0   # while Busy-Time waits it asks the window now and then: how long it takes to answer (a frozen window = a long time)
function Busy-Time($proc, [double]$maxSec = 120) {
    $sw = [Diagnostics.Stopwatch]::StartNew(); $quiet = 0
    $proc.Refresh(); $last = $proc.TotalProcessorTime; $lastT = $sw.Elapsed.TotalMilliseconds
    while ($sw.Elapsed.TotalSeconds -lt $maxSec) {
        Start-Sleep -Milliseconds 250
        if ($script:probeWin) { $t = [Diagnostics.Stopwatch]::StartNew(); try { $null = $script:probeWin.Current.Name } catch { }; $script:maxLatency = [Math]::Max($script:maxLatency, $t.Elapsed.TotalSeconds) }
        $proc.Refresh()
        if ($proc.HasExited) { return -1 }
        $cpu = $proc.TotalProcessorTime; $now = $sw.Elapsed.TotalMilliseconds
        $use = ($cpu - $last).TotalMilliseconds / [Math]::Max(1, ($now - $lastT))
        $last = $cpu; $lastT = $now
        if ($use -lt 0.08) { $quiet += 250 } else { $quiet = 0 }
        if ($quiet -ge 1500) { return [Math]::Round($sw.Elapsed.TotalSeconds - 1.5, 1) }
    }
    return $maxSec
}
# how long the window takes to answer (a hung UI thread makes UI Automation wait)
Add-Type -Namespace GS -Name Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);' -ErrorAction SilentlyContinue
function Focus-Window($win) { try { [GS.Win]::SetForegroundWindow([IntPtr]$win.Current.NativeWindowHandle) | Out-Null } catch { }; Start-Sleep -Milliseconds 300 }
function Probe($win) { $sw = [Diagnostics.Stopwatch]::StartNew(); try { $null = $win.Current.Name; $null = Find $win "檢視" } catch { }; return [Math]::Round($sw.Elapsed.TotalSeconds, 2) }
function Mem-MB($proc) { $proc.Refresh(); return [int]($proc.WorkingSet64 / 1MB) }
function Fresh-App($archive, $ini) {
    $script:probeWin = $null
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 600
    if ($env:LYCAN_LOG_DIR) { Get-ChildItem $env:LYCAN_LOG_DIR -File -ErrorAction SilentlyContinue | ForEach-Object { try { [IO.File]::Delete($_.FullName) } catch { } } }
    [IO.File]::WriteAllText($settingsFile, $ini, (New-Object Text.UTF8Encoding($false)))   # no BOM: the first key would be lost
    if ($archive) { return Start-Process $exe -ArgumentList "`"$archive`"" -PassThru } else { return Start-Process $exe -PassThru }
}

try {
    if ($Part -eq "big") {
        $zips = Join-Path $env:TEMP "lycan_bigzips"
        $make = Join-Path $root "tests\robust\make_big_zip.py"
        New-Item -ItemType Directory -Force $zips | Out-Null
        foreach ($c in @(@("flat100k", 100000, "flat"), @("nested100k", 100000, "nested"), @("deep100k", 100000, "deep"), @("flat400k", 400000, "flat"), @("nested400k", 400000, "nested"))) {
            if (-not (Test-Path (Join-Path $zips "$($c[0]).zip"))) { & python $make (Join-Path $zips "$($c[0]).zip") $c[1] $c[2] | Out-Null }
        }
        Say "GUI stress: big archives ($Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
        Say ("{0,-12} {1,-8} {2,8} {3,8} {4,8} {5,8} {6,8} {7,8} {8,8} {9,8} {10,7}" -f "archive", "view", "open s", "ready s", "MB idle", "search s", "sort s", "all s", "scroll s", "MB max", "probe s")
        $cases = @(
            @("flat100k", "details"), @("nested100k", "details"), @("deep100k", "details"), @("flat100k", "icons"), @("nested100k", "icons"),
            @("flat400k", "details"), @("nested400k", "details"), @("flat400k", "icons"))
        if ($Quick) { $cases = @(@("flat100k", "details"), @("nested100k", "icons")) }
        if ($Only.Count) { $cases = @($cases | Where-Object { $Only -contains $_[0] }) }
        $env:LYCAN_LOG = "debug"; $env:LYCAN_LOG_DIR = Join-Path $env:TEMP "lycan_stress_log"
        New-Item -ItemType Directory -Force $env:LYCAN_LOG_DIR | Out-Null
        foreach ($case in $cases) {
            $name = $case[0]; $view = $case[1]
            $ini = "view_mode=$view`nthumb_size=3`nicon_thumbnails=true`ncompact_close_when_done=true`n[state]`nwelcome_done=true`n"
            $p = Fresh-App (Join-Path $zips "$name.zip") $ini
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $win = $null; $end = (Get-Date).AddSeconds(240)
            while (-not $win -and (Get-Date) -lt $end) { try { $w = Get-Win $p; if ($w -and (Find $w "解壓縮到此")) { $win = $w } } catch { }; if (-not $win) { Start-Sleep -Milliseconds 300 } }
            $tOpen = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            if (-not $win) { Say ("{0,-12} {1,-8} NOT OPEN after {2} s" -f $name, $view, $tOpen); $failures++; continue }
            $script:probeWin = $win; $script:maxLatency = 0.0
            $tReady = Busy-Time $p 240
            $memIdle = Mem-MB $p; $memMax = $memIdle
            $probe = Probe $win
            $tSearch = $tSort = $tAll = $tScroll = "-"
            try {
                Focus-Window $win
                # sort by name, the other way round (click the header; the size column is not always shown)
                $hd = Find $win "依名稱排序"
                if ($hd) { Click $hd; $tSort = Busy-Time $p 120; $memMax = [Math]::Max($memMax, (Mem-MB $p)) }
                # search (Ctrl+F, a name that exists once)
                [System.Windows.Forms.SendKeys]::SendWait("^f"); Start-Sleep -Milliseconds 300
                [System.Windows.Forms.SendKeys]::SendWait("file0099"); $tSearch = Busy-Time $p 120; $memMax = [Math]::Max($memMax, (Mem-MB $p))
                [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 500; $null = Busy-Time $p 120
                # list all (Ctrl+Shift+F), only where it is not already flat
                [System.Windows.Forms.SendKeys]::SendWait("^+f"); $tAll = Busy-Time $p 180; $memMax = [Math]::Max($memMax, (Mem-MB $p))
                # scroll to the end and back
                $lst = Find $win "壓縮檔內容"
                if ($lst) { try { $lst.SetFocus() } catch { } }
                [System.Windows.Forms.SendKeys]::SendWait("{END}"); Start-Sleep -Milliseconds 300
                [System.Windows.Forms.SendKeys]::SendWait("{PGUP 10}"); [System.Windows.Forms.SendKeys]::SendWait("{HOME}")
                $tScroll = Busy-Time $p 120; $memMax = [Math]::Max($memMax, (Mem-MB $p))
                $probe = [Math]::Max($probe, (Probe $win))
                $probe = [Math]::Max($probe, [Math]::Round($script:maxLatency, 2))   # the longest the window did not answer during all of it
            } catch { Say "   error: $($_.Exception.Message)" }
            Say ("{0,-12} {1,-8} {2,8} {3,8} {4,8} {5,8} {6,8} {7,8} {8,8} {9,8} {10,7}" -f $name, $view, $tOpen, $tReady, $memIdle, $tSearch, $tSort, $tAll, $tScroll, $memMax, $probe)
            Get-ChildItem $env:LYCAN_LOG_DIR -File -ErrorAction SilentlyContinue | ForEach-Object { Get-Content $_.FullName -Encoding UTF8 -ErrorAction SilentlyContinue | Select-String "list: " | ForEach-Object { $x = ($_ -split "\]", 2)[-1].Trim(); Say "   $x" } }
            if ($p.HasExited) { Say "   *** the program ended (crashed?)"; $failures++ }
            elseif ($memMax -gt 3000) { Say "   *** more than 3 GB"; $failures++ }
            elseif ($probe -gt 10) { Say "   *** the window did not answer for $probe s"; $failures++ }
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($Part -eq "flood") {
        Say "GUI stress: a flood of right-click actions ($Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $t = Join-Path $env:TEMP "lycan_flood"; Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $t | Out-Null
        # the archives to extract (each its own folder), the 200 files to compress, one big file
        function New-Zip($dir, $name) {
            New-Item -ItemType Directory "$dir\src" -Force | Out-Null
            Set-Content "$dir\src\a.txt" ("hello " * 200); Set-Content "$dir\src\b.txt" ("world " * 200)
            [IO.Compression.ZipFile]::CreateFromDirectory("$dir\src", "$dir\$name.zip"); Remove-Item "$dir\src" -Recurse -Force
        }
        function Count-Windows($pid2) { @([Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children, (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $pid2)))).Count }
        New-Zip $t "base"
        $baseZip = "$t\base.zip"
        $ini = "compact_close_when_done=true`nview_mode=details`n[state]`nwelcome_done=true`n"
        $p = Fresh-App $baseZip $ini
        $win = Get-Win $p; Wait-Find $win "解壓縮到此" 30 | Out-Null; Start-Sleep -Seconds 2
        $null = Busy-Time $p 30
        $memStart = Mem-MB $p
        Say "the main window shows base.zip: $memStart MB, $(Count-Windows $p.Id) window(s)"

        # F1: 30 extractions, one every 2 s (more than the time that makes requests share a window): each has a compact window of its own
        $rounds = if ($Quick) { 1 } else { 3 }
        for ($round = 1; $round -le $rounds; ++$round) {
            $n = 30; $peak = 1
            $dirs = 1..$n | ForEach-Object { $d = "$t\r$round\x$_"; New-Item -ItemType Directory $d -Force | Out-Null; New-Zip $d "z"; $d }
            $sw = [Diagnostics.Stopwatch]::StartNew()
            foreach ($d in $dirs) {
                Start-Process $exe -ArgumentList "--extract-here", "`"$d\z.zip`"" -WindowStyle Hidden
                $until = $sw.Elapsed.TotalSeconds + 2
                while ($sw.Elapsed.TotalSeconds -lt $until) { $peak = [Math]::Max($peak, (Count-Windows $p.Id)); Start-Sleep -Milliseconds 200 }
            }
            $end = (Get-Date).AddSeconds(60)
            while ((Count-Windows $p.Id) -gt 1 -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
            $left = Count-Windows $p.Id
            $done = @($dirs | Where-Object { Test-Path "$_\z\a.txt" }).Count
            $null = Busy-Time $p 30
            $mem = Mem-MB $p
            Say ("F1 round {0}: {1} requests, {2} extracted, most windows at once {3}, windows left {4}, memory {5} MB (start {6} MB)" -f $round, $n, $done, $peak, $left, $mem, $memStart)
            if ($done -ne $n) { Say "   *** not every archive was extracted"; $failures++ }
            if ($left -ne 1) { Say "   *** compact windows are left"; $failures++ }
            if ($mem -gt $memStart + 300) { Say "   *** the memory grew by more than 300 MB"; $failures++ }
        }

        # F2: a selection of 200 files: Explorer starts one process per file; the requests of one selection share one window and make one archive
        $src = "$t\sel"; New-Item -ItemType Directory $src | Out-Null
        1..200 | ForEach-Object { Set-Content "$src\file$_.txt" ("data $_ " * 20) }
        $peak = 1
        $sw = [Diagnostics.Stopwatch]::StartNew()
        1..200 | ForEach-Object {
            Start-Process $exe -ArgumentList "--compress", "zip", "`"$src\file$_.txt`"" -WindowStyle Hidden
            $peak = [Math]::Max($peak, (Count-Windows $p.Id)); Start-Sleep -Milliseconds 25
        }
        $zipFile = "$src\sel.zip"
        $end = (Get-Date).AddSeconds(90); while (-not (Test-Path $zipFile) -and (Get-Date) -lt $end) { $peak = [Math]::Max($peak, (Count-Windows $p.Id)); Start-Sleep -Milliseconds 300 }
        Start-Sleep -Seconds 3
        $entries = 0
        if (Test-Path $zipFile) { $z = [IO.Compression.ZipFile]::OpenRead($zipFile); $entries = $z.Entries.Count; $z.Dispose() }
        $zips = @(Get-ChildItem $src -Filter *.zip).Count
        Say ("F2: 200 requests in {0} s: {1} zip(s), {2} entries in sel.zip, most windows at once {3}" -f [int]$sw.Elapsed.TotalSeconds, $zips, $entries, $peak)
        if ($entries -ne 200) { Say "   *** the archive does not have the 200 files"; $failures++ }
        if ($peak -gt 6) { Say "   *** too many windows for one selection"; $failures++ }

        # F3: a long compression while more actions arrive: both finish
        $bigDir = "$t\big"; New-Item -ItemType Directory $bigDir | Out-Null
        $rnd = New-Object Random 5; $buf = New-Object byte[] (64MB)
        1..3 | ForEach-Object { $rnd.NextBytes($buf); [IO.File]::WriteAllBytes("$bigDir\b$_.bin", $buf) }
        $dirs = 1..6 | ForEach-Object { $d = "$t\f3\x$_"; New-Item -ItemType Directory $d -Force | Out-Null; New-Zip $d "z"; $d }
        Start-Process $exe -ArgumentList "--compress", "7z", "`"$bigDir\b1.bin`"", "`"$bigDir\b2.bin`"", "`"$bigDir\b3.bin`"" -WindowStyle Hidden
        Start-Sleep -Seconds 3
        foreach ($d in $dirs) { Start-Process $exe -ArgumentList "--extract-here", "`"$d\z.zip`"" -WindowStyle Hidden; Start-Sleep -Milliseconds 2200 }
        $end = (Get-Date).AddSeconds(240)
        while ((-not (Test-Path "$bigDir\bigDir.7z") -and -not (Get-ChildItem $bigDir -Filter *.7z -ErrorAction SilentlyContinue)) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 500 }
        Start-Sleep -Seconds 3
        $doneX = @($dirs | Where-Object { Test-Path "$_\z\a.txt" }).Count
        $big = @(Get-ChildItem $bigDir -Filter *.7z -ErrorAction SilentlyContinue)
        Say ("F3: the long compression {0}, the {1} extractions that came meanwhile: {2} done" -f $(if ($big.Count) { "finished (" + [int]($big[0].Length / 1MB) + " MB)" } else { "DID NOT FINISH" }), $dirs.Count, $doneX)
        if (-not $big.Count) { Say "   *** the long compression did not finish"; $failures++ }
        if ($doneX -ne $dirs.Count) { Say "   *** some extractions did not finish"; $failures++ }
        if ($p.HasExited) { Say "   *** the main window process ended"; $failures++ }
        Get-Process LycanZip -ErrorAction SilentlyContinue | Measure-Object | ForEach-Object { Say "LycanZip processes at the end: $($_.Count)" }
    }
    if ($Part -eq "dpi") {
        . (Join-Path $PSScriptRoot "dpi-helper.ps1")
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class DpiShot {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
}
"@ -ErrorAction SilentlyContinue
        [DpiShot]::SetProcessDPIAware() | Out-Null
        Say "GUI stress: other display scales ($Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $t = Join-Path $env:TEMP "lycan_dpi"; Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$t\src\docs" -Force | Out-Null
        $cols = @([System.Drawing.Color]::SteelBlue, [System.Drawing.Color]::OrangeRed, [System.Drawing.Color]::SeaGreen)
        for ($i = 1; $i -le 3; $i++) { $bmp = New-Object System.Drawing.Bitmap 400, 300; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear($cols[$i - 1]); $g.FillEllipse([System.Drawing.Brushes]::White, 150, 100, 100, 100); $g.Dispose(); $bmp.Save("$t\src\photo$i.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }
        foreach ($n in "notes.txt", "report.docx", "song.mp3", "run.exe", "data.json", "a very long file name that needs two lines in the icon view.txt") { Set-Content "$t\src\$n" ("x" * 100) }
        [IO.Compression.ZipFile]::CreateFromDirectory("$t\src", "$t\sample.zip")
        function Snap($proc, $name, $scale) {
            $hwnd = [IntPtr]0
            $end = (Get-Date).AddSeconds(15)
            while ($hwnd -eq 0 -and (Get-Date) -lt $end) {
                foreach ($w in [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children, (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)))) {
                    if ($w.Current.Name -like $name) { $hwnd = [IntPtr]$w.Current.NativeWindowHandle }
                }
                if ($hwnd -eq 0) { Start-Sleep -Milliseconds 300 }
            }
            if ($hwnd -eq 0) { return $null }
            Start-Sleep -Milliseconds 1500
            $r = New-Object DpiShot+RECT; [DpiShot]::GetWindowRect($hwnd, [ref]$r) | Out-Null
            $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
            $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [DpiShot]::PrintWindow($hwnd, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
            $file = Join-Path $shots ("dpi{0}-{1}.png" -f $scale, $name.Replace("*", "").Replace(" ", ""))
            $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            return [int][DpiShot]::GetDpiForWindow($hwnd)
        }
        $oldScale = Get-DisplayScale
        Say "the display scale is $oldScale% now; it is put back at the end"
        try {
            foreach ($scale in @(100, 125, 150, 175, 200)) {
                if (-not (Set-DisplayScale $scale)) { Say "   the scale $scale% could not be set here"; continue }
                $now = Get-DisplayScale
                $line = "scale $scale% (now $now%):"
                # the archive in the icon view (medium and huge) and in the details view
                foreach ($cfg in @(@("icons", 1, "archive-icons-m"), @("icons", 3, "archive-icons-huge"), @("details", 1, "archive-details"))) {
                    $ini = "view_mode=$($cfg[0])`nthumb_size=$($cfg[1])`nicon_thumbnails=true`n[state]`nwelcome_done=true`n"
                    $p = Fresh-App "$t\sample.zip" $ini
                    $dpi = Snap $p "LycanZip*" "$scale-$($cfg[2])" $scale
                    if ($null -eq $dpi) { $line += " [$($cfg[2]): no window]"; $failures++ } else { $line += " $($cfg[2]) dpi=$dpi"; if ($dpi -ne [int]($now * 0.96)) { $line += "(!)" } }
                    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                }
                # the home page, the compress page, the settings page and the welcome window (its three steps)
                $ini = "view_mode=icons`n[state]`nwelcome_done=true`n"
                $p = Fresh-App $null $ini; $null = Snap $p "LycanZip*" "$scale-home" $scale; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                $null = Fresh-App $null $ini; Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 400   # (only to write the settings)
                $p = Start-Process $exe -ArgumentList "--add", "`"$t\src\photo1.png`"" -PassThru
                $null = Snap $p "LycanZip*" "$scale-compress" $scale; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                $env:LYCAN_FORCE_WELCOME = "1"; $env:LYCAN_TEST_REG_ROOT = "Software\LycanZipTest_dpi"
                $p = Fresh-App $null "[state]`nwelcome_done=false`n"
                $null = Snap $p "歡迎*" "$scale-welcome1" $scale
                foreach ($stepNo in 2, 3) {
                    $w = [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children, (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id))) | Where-Object { $_.Current.Name -like "歡迎*" } | Select-Object -First 1
                    if ($w) { $b = Find $w "下一步"; if ($b) { Click $b; Start-Sleep -Milliseconds 800 } }
                    $null = Snap $p "歡迎*" "$scale-welcome$stepNo" $scale
                }
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                Remove-Item Env:LYCAN_FORCE_WELCOME, Env:LYCAN_TEST_REG_ROOT -ErrorAction SilentlyContinue
                Say $line
            }
        } finally {
            $r = Set-DisplayScale $oldScale
            Say "the display scale is back at $(Get-DisplayScale)% ($r)"
        }
    }
    if ($Part -eq "cancel") {
        Say "GUI stress: cancelling a compression in the window, every format ($Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
        $t = Join-Path $env:TEMP "lycan_cancel"; Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $t | Out-Null
        $rnd = New-Object Random 17; $buf = New-Object byte[] (8MB)
        function Snap-Temp { @(Get-ChildItem $env:TEMP -Force -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike "$t*" -and $_.Name -notlike "lycan_*" -and $_.Name -ne "claude" } | ForEach-Object { $_.Name }) }
        $p = Fresh-App $null "compact_close_when_done=true`n[state]`nwelcome_done=true`n"
        Start-Sleep -Seconds 3
        foreach ($fmt in "zip", "7z", "tgz", "txz", "tbz2", "iso") {   # the plain tar is not a format of --compress
            $d = Join-Path $t "src"
            if (-not (Test-Path "$d\data.bin")) {
                New-Item -ItemType Directory $d | Out-Null
                $fs = [IO.File]::Create("$d\data.bin"); 1..400 | ForEach-Object { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Dispose()   # 3.2 GB, does not shrink
            }
            $before = Snap-Temp
            # --compress <fmt> from the window of the running app (the request is forwarded: its own compact window)
            $appFmt = @{ tgz = "tar.gz"; txz = "tar.xz"; tbz2 = "tar.bz2" }[$fmt]; if (-not $appFmt) { $appFmt = $fmt }
            Start-Process $exe -ArgumentList "--compress", $appFmt, "`"$d\data.bin`"" -WindowStyle Hidden
            $w = $null; $end = (Get-Date).AddSeconds(30)
            while (-not $w -and (Get-Date) -lt $end) {
                foreach ($x in [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children, (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)))) {
                    if ($x.Current.Name -like "LycanZip*" -and (Find $x "取消")) { $w = $x }
                }
                if (-not $w) { Start-Sleep -Milliseconds 300 }
            }
            if (-not $w) { Say "$fmt : no window with a cancel button"; $failures++; continue }
            Start-Sleep -Milliseconds 1500
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $b = Find $w "取消"
            if ($b) { Click $b }
            # the window answers and goes away (or says it is cancelled); the worker process ends
            $end = (Get-Date).AddSeconds(30)
            while ((Get-Date) -lt $end) {
                $workers = @(Get-CimInstance Win32_Process -Filter "Name='lycanzip-cli.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*--worker*" })
                if (-not $workers.Count) { break }
                Start-Sleep -Milliseconds 300
            }
            $stopAfter = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            Start-Sleep -Seconds 2
            $outputs = @(Get-ChildItem $d -Force | Where-Object { $_.Name -ne "data.bin" })
            $after = Snap-Temp
            $newTemp = @($after | Where-Object { $before -notcontains $_ })
            $workers = @(Get-CimInstance Win32_Process -Filter "Name='lycanzip-cli.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*--worker*" })
            $line = "{0,-5} cancelled; the work stopped after {1} s; files left next to the source: {2}; new items in TEMP: {3}; workers left: {4}" -f $fmt, $stopAfter, $outputs.Count, $newTemp.Count, $workers.Count
            Say $line
            if ($outputs.Count) { Say "   *** left: $($outputs.Name -join ', ')"; $failures++ }
            if ($newTemp.Count) { Say "   *** TEMP: $($newTemp -join ', ')"; $failures++ }
            if ($workers.Count) { Say "   *** a worker process is still there"; $failures++; $workers | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } }
            if ($stopAfter -gt 20) { Say "   *** the cancel took too long"; $failures++ }
            Get-ChildItem $d -Force | Where-Object { $_.Name -ne "data.bin" } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        }
        Say ("the app is {0}" -f $(if ($p.HasExited) { "GONE" } else { "still running" }))
        if ($p.HasExited) { $failures++ }
    }
    Save-Report
}
finally {
    Restore-State
}
if ($failures) { Say "FAILURES: $failures"; Save-Report; exit 1 }
Say "done (no crash, no hang, memory under 3 GB)"
Save-Report
