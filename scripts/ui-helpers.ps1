# Shared functions of the UI Automation tests (loaded by dot-sourcing from ui-smoke.ps1 and the check of the portable package).
# The caller must define first: $exe (the LycanZip.exe to test), $shots (the folder for screenshots), $failures = 0.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
$env:LYCAN_NO_WELCOME = "1"   # the welcome window is for a first start; the tests that look at it clear this (LYCAN_FORCE_WELCOME)
$env:LYCAN_NO_DIALOGS = "1"   # the App under test fails at once on an error, with no CRT / WER dialog (it would block the test)
$AE = [Windows.Automation.AutomationElement]
$TS = [Windows.Automation.TreeScope]
$failures = 0
function Check($cond, $msg) { if ($cond) { Write-Host "ok  : $msg" } else { Write-Host "FAIL: $msg" -ForegroundColor Red; $script:failures++ } }

function ByName($name) { New-Object Windows.Automation.PropertyCondition($AE::NameProperty, $name) }
# The first element of this name that is not (a part of) a tool tip: the buttons of a narrow window have no text, only a tip that says the same
# thing as the name of the button (the tip is a window of the process too, so it would be found first when the pointer is over the button).
function Find($win, $name) {
    $first = $win.FindFirst($TS::Descendants, (ByName $name))
    if (-not $first) { return $null }
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $tipType = [Windows.Automation.ControlType]::ToolTip
    $inTip = {
        param($e)
        $x = $e
        # (the element can go away while it is looked at, a closing window: then it is no tip either)
        try { for ($i = 0; $i -lt 3 -and $x; $i++) { if ($x.Current.ControlType -eq $tipType) { return $true }; $x = $walker.GetParent($x) } } catch { }
        return $false
    }
    if (-not (& $inTip $first)) { return $first }
    foreach ($e in $win.FindAll($TS::Descendants, (ByName $name))) { if (-not (& $inTip $e)) { return $e } }
    return $first
}
function Wait-Find($win, $name, [int]$sec = 10) {
    $end = (Get-Date).AddSeconds($sec)
    do { $e = Find $win $name; if ($e) { return $e }; Start-Sleep -Milliseconds 150 } while ((Get-Date) -lt $end)
    return $null
}
function Click($el) {
    $w = [Windows.Automation.TreeWalker]::ControlViewWalker; $x = $el
    while ($x) {
        $p = $null
        if ($x.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke(); return }
        if ($x.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { $p.Toggle(); return }
        $x = $w.GetParent($x)
    }
    throw "No clickable element found: $($el.Current.Name)"
}
function Open-SettingsTab($win, $name) {   # the settings page shows one tab at a time: the names are the Chinese UI texts (Basic / Compress / View / Explorer / Shortcuts / About)
    $bar = Wait-Find $win "設定分頁" 8
    if (-not $bar) { throw "The settings tab bar was not found" }
    $e = $bar.FindFirst($TS::Descendants, (ByName $name))
    if (-not $e) { throw "Settings tab not found: $name" }
    $sp = $null
    if ($e.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sp)) { $sp.Select() } else { Click $e }
    Start-Sleep -Milliseconds 500
}
function Find-Combo($win, $name) {
    # The label text of the settings page has the same name as the drop-down, so restrict to the ComboBox type
    $cond = New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ComboBox)))
    $win.FindFirst($TS::Descendants, $cond)
}
function Combo-Selected($combo) {
    $s = $combo.GetCurrentPattern([Windows.Automation.SelectionPattern]::Pattern).Current.GetSelection()
    if ($s.Count) { $s[0].Current.Name } else { "" }
}
function Select-Combo($win, $comboName, $itemName) {
    $c = $null; $end = (Get-Date).AddSeconds(8)
    do { $c = Find-Combo $win $comboName; if (-not $c) { Start-Sleep -Milliseconds 250 } } while (-not $c -and (Get-Date) -lt $end)   # right after the page switch the control may not be in the UIA tree yet
    if (-not $c) { throw "Drop-down not found: $comboName" }
    $c.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
    # Only ListItem: the name of an option may equal that of another control (for example "Close" is also the close button of the title bar)
    $itemCond = New-Object Windows.Automation.AndCondition((ByName $itemName), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))
    $i = $null; $end2 = (Get-Date).AddSeconds(3)
    do { $i = $win.FindFirst($TS::Descendants, $itemCond); if (-not $i) { Start-Sleep -Milliseconds 150 } } while (-not $i -and (Get-Date) -lt $end2)
    if (-not $i) { $c.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse(); throw "Item not found in the drop-down: $itemName" }
    $i.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 300
}
# Typing into the windows of the tests must always happen with the English keyboard: with a Chinese input method (Bopomofo, Pinyin) the keys are
# eaten by the IME (the "save as" test lost the path it typed, single-key shortcuts such as P or T turn into Zhuyin). A new window starts with the
# layout of the window that was active when it was made, so every window that Get-Win returns is switched to the US layout (the layout belongs to the
# thread of the program, so the dialogs it opens follow). If it cannot be switched a warning is printed once: then the results of tests that type are
# not to be trusted (switch to English in the taskbar and run again).
Add-Type -Namespace Ime -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern IntPtr LoadKeyboardLayout(string pwszKLID, uint Flags); [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint tid);' -ErrorAction SilentlyContinue
$script:imeWarned = $false
function Use-EnglishInput($hwnd) {
    try {
        $hkl = [Ime.K]::LoadKeyboardLayout("00000409", 1)
        $tp = 0; $tid = [Ime.K]::GetWindowThreadProcessId([IntPtr]$hwnd, [ref]$tp)
        $now = [Ime.K]::GetKeyboardLayout($tid).ToInt64() -band 0xFFFF
        if ($now -eq 0x409) { return }
        [Ime.K]::SendMessage([IntPtr]$hwnd, 0x0050, [IntPtr]::Zero, $hkl) | Out-Null   # WM_INPUTLANGCHANGEREQUEST
        Start-Sleep -Milliseconds 400
        $now = [Ime.K]::GetKeyboardLayout($tid).ToInt64() -band 0xFFFF
        if ($now -ne 0x409 -and -not $script:imeWarned) { $script:imeWarned = $true; Write-Host ("WARNING: the keyboard of the program is 0x{0:X}, not English (0x409): typed keys may be eaten by the input method" -f $now) -ForegroundColor Yellow }
    } catch { }
}
function Get-Win($proc) {
    $end = (Get-Date).AddSeconds(15)
    do {
        # the first top-level element of the process that is a real window (not a transient popup or tooltip)
        $all = $AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)))
        foreach ($w in $all) { try { if ($w.GetCurrentPropertyValue($AE::IsWindowPatternAvailableProperty)) { Use-EnglishInput $w.Current.NativeWindowHandle; Start-Sleep -Milliseconds 150; return $w } } catch { } }
        Start-Sleep -Milliseconds 200
    }while ((Get-Date) -lt $end)
    throw "Window not found"
}
function Shot($proc, $name) {
    Add-Type -Namespace Native -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }' -ErrorAction SilentlyContinue
    [Native.W]::SetProcessDPIAware() | Out-Null
    $proc.Refresh(); $h = $proc.MainWindowHandle
    $r = New-Object Native.W+RECT; [Native.W]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [Native.W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save((Join-Path $shots "$name.png")); $bmp.Dispose()
}
function Start-App($archive) {
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    if ($archive) { Start-Process $exe -ArgumentList "`"$archive`"" -PassThru } else { Start-Process $exe -PassThru }
}
function Type-Into($el, $text) {
    # Prefer setting the text directly with the ValuePattern of UIA: SendKeys goes through the input method, and when the IME of the user is in Chinese mode "b.b" turns into phonetic symbols (this caused a wrong verdict once)
    $vp = $null
    if ($el.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$vp) -and -not $vp.Current.IsReadOnly -and -not $el.Current.IsPassword) {
        try { $vp.SetValue($text); Start-Sleep -Milliseconds 300; return } catch { }
    }
    $el.SetFocus(); Start-Sleep -Milliseconds 500   # SendKeys loses characters when the focus is not ready yet (this caused sporadic failures)
    $escaped = $text -replace '([+^%~(){}\[\]])', '{$1}'
    [System.Windows.Forms.SendKeys]::SendWait($escaped)
    Start-Sleep -Milliseconds 200
}
function All-Elements($win) {
    # While animation / layout is updating, walking the whole UIA tree sometimes throws ElementNotAvailable; retry a few times
    for ($i = 0; $i -lt 6; $i++) {
        try { return $win.FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition) } catch { Start-Sleep -Milliseconds 250 }
    }
    return @()
}
function Wait-Task($win, $pattern, [int]$sec = 15) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $hit = (All-Elements $win) | Where-Object { $_.Current.Name -match $pattern } | Select-Object -First 1
        if ($hit) { return $hit.Current.Name }; Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return $null
}
