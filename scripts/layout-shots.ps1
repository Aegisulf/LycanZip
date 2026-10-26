# Screenshots of the pages of LycanZip in several languages, for looking at the layout (text that is cut off, overlaps or does not fit).
# Usage: .\scripts\layout-shots.ps1 [-Languages pseudo,en-US] [-Config Debug|Release] [-OutDir build\layout-shots]
# pseudo = the test language (accented English, 40 % longer, in brackets: LYCAN_LANG=pseudo), the real languages are the files of app\Assets\Lang.
# It uses a temporary settings folder (LYCAN_SETTINGS_DIR): the settings of the user are not touched. It operates windows: do not touch the mouse and keyboard while it runs.
param([string[]]$Languages = @("pseudo", "en-US"), [ValidateSet("Debug", "Release")][string]$Config = "Debug", [string]$OutDir = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "app\bin\$Config\LycanZip.exe"
if (-not (Test-Path $exe)) { throw "$exe was not found: scripts\build-app.ps1 -Config $Config" }
if (-not $OutDir) { $OutDir = Join-Path $root "build\layout-shots" }
$shots = $OutDir
New-Item -ItemType Directory -Force $shots | Out-Null
. (Join-Path $PSScriptRoot "ui-helpers.ps1")

$t = Join-Path $env:TEMP "lycan_layout_$(Get-Date -Format HHmmss)"
New-Item -ItemType Directory "$t\src\folder\sub", "$t\settings" | Out-Null
("hello" * 200) | Set-Content "$t\src\folder\a.txt"; "x" | Set-Content "$t\src\readme.txt"
[IO.File]::WriteAllBytes("$t\src\folder\sub\b.bin", [byte[]](1..255 * 600))
Push-Location "$t\src"; 7z a -tzip "$t\sample.zip" ".\folder" ".\readme.txt" | Out-Null; Pop-Location

function ById($id) { New-Object Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id) }
function Wait-Id($win, $id, [int]$sec = 8) {
    $end = (Get-Date).AddSeconds($sec)
    do { $e = $win.FindFirst($TS::Descendants, (ById $id)); if ($e) { return $e }; Start-Sleep -Milliseconds 200 } while ((Get-Date) -lt $end)
    return $null
}
function Start-Lycan([string]$lang, [string[]]$argList) {
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $env:LYCAN_LANG = $lang
    $env:LYCAN_SETTINGS_DIR = "$t\settings"
    $env:LYCAN_NO_STALE_CHECK = "1"
    $env:LYCAN_TEST_STORE_INSTALLED = "0"
    Set-Content "$t\settings\settings.ini" "welcome_done=true`nview_mode=details`nicon_thumbnails=false" -Encoding ascii
    Remove-Item "$t\settings\presets.ini" -ErrorAction SilentlyContinue
    if ($argList -and $argList.Count) { Start-Process $exe -ArgumentList $argList -PassThru } else { Start-Process $exe -PassThru }
}
function Q($s) { "`"$s`"" }

foreach ($lang in $Languages) {
    Write-Host "== $lang =="
    # the home page, an archive, and the settings (every tab, scrolled page by page)
    $p = Start-Lycan $lang @(); $win = Get-Win $p; Start-Sleep -Seconds 2; Shot $p "$lang-01-home"
    $gear = Wait-Id $win "SettingsButton"
    if ($gear) {
        Click $gear; Start-Sleep -Milliseconds 800
        $n = 0
        foreach ($tab in "TabItemGeneral", "TabItemCompress", "TabItemView", "TabItemShell", "TabItemKeys", "TabItemAbout") {
            $item = Wait-Id $win $tab
            if (-not $item) { Write-Host "  tab not found: $tab"; continue }
            $sp = $null
            if ($item.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sp)) { $sp.Select() } else { Click $item }
            Start-Sleep -Milliseconds 700
            $scroll = $win.FindFirst($TS::Descendants, (ById "SettingsScroll"))
            $sc = $null
            if ($scroll -and $scroll.TryGetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern, [ref]$sc) -and $sc.Current.VerticallyScrollable) {
                $view = $sc.Current.VerticalViewSize; $pages = [Math]::Ceiling(100.0 / $view)
                for ($i = 0; $i -lt $pages; $i++) {
                    $pos = if ($pages -le 1) { 0 } else { 100.0 * $i / ($pages - 1) }
                    $sc.SetScrollPercent([Windows.Automation.ScrollPattern]::NoScroll, $pos); Start-Sleep -Milliseconds 400
                    Shot $p ("{0}-02-{1}-{2}" -f $lang, $tab.Replace("TabItem", "").ToLower(), ($i + 1))
                }
            } else { Shot $p ("{0}-02-{1}-1" -f $lang, $tab.Replace("TabItem", "").ToLower()) }
        }
    } else { Write-Host "  the settings button was not found" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    $p = Start-Lycan $lang @((Q "$t\sample.zip")); $win = Get-Win $p; Start-Sleep -Seconds 3; Shot $p "$lang-03-archive"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the small windows of the context menu: adding (the options), the encrypted archive, a finished job, the test
    $one = "$t\src\readme.txt"
    foreach ($case in @(@("04-add", @("--add", (Q $one))), @("05-addenc", @("--add-encrypted", (Q $one))), @("06-compress", @("--compress", "zip", (Q $one))), @("07-test", @("--test", (Q "$t\sample.zip"))), @("08-extract", @("--extract-here", (Q "$t\sample.zip"))))) {
        Remove-Item "$t\src\readme*.zip", "$t\sample" -Recurse -Force -ErrorAction SilentlyContinue   # no "file already exists" question
        $p = Start-Lycan $lang $case[1]; $win = Get-Win $p; Start-Sleep -Seconds 3; Shot $p ("{0}-{1}" -f $lang, $case[0])
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    # the welcome window: the three steps
    $env:LYCAN_FORCE_WELCOME = "1"
    $p = Start-Lycan $lang @(); $win = Get-Win $p; Start-Sleep -Seconds 3
    $wel = $null
    foreach ($w in $AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)))) { if ($w.Current.NativeWindowHandle -ne $win.Current.NativeWindowHandle) { $wel = $w } }
    if ($wel) {
        Add-Type -Namespace Native2 -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }' -ErrorAction SilentlyContinue
        for ($step = 1; $step -le 3; $step++) {
            $h = [IntPtr]$wel.Current.NativeWindowHandle; $r = New-Object Native2.W+R; [Native2.W]::GetWindowRect($h, [ref]$r) | Out-Null
            $bmp = New-Object System.Drawing.Bitmap ($r.Rr - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
            [Native2.W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose(); $bmp.Save((Join-Path $shots ("{0}-09-welcome-{1}.png" -f $lang, $step))); $bmp.Dispose()
            $buttons = @($wel.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button))) | Where-Object { -not $_.Current.IsOffscreen })
            $next = $buttons | Sort-Object { $_.Current.BoundingRectangle.Bottom * 10000 + $_.Current.BoundingRectangle.Right } -Descending | Select-Object -First 1
            if ($next -and $step -lt 3) { Click $next; Start-Sleep -Milliseconds 900 }
        }
    } else { Write-Host "  the welcome window was not found" }
    Remove-Item Env:LYCAN_FORCE_WELCOME -ErrorAction SilentlyContinue
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Screenshots: $shots"
