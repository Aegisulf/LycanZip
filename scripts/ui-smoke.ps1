# UI smoke test: operates LycanZip with UI Automation and verifies the main flows.
# Usage: .\scripts\ui-smoke.ps1 [-Config Debug|Release] [-KeepShots] [-Section name,name ...] [-List]
# Without -Section it runs everything; -List lists the names of all sections. Which section belongs to which source, and advice: see docs/TESTING.md.
# It creates LycanZip.portable next to the exe so that the settings are written next to the exe (the settings of the user are not touched); removed at the end.
# Note: needs an interactive desktop (it really operates windows); do not touch the mouse and keyboard while it runs.
param([ValidateSet("Debug", "Release")][string]$Config = "Debug", [switch]$KeepShots, [string[]]$Section = @(), [switch]$List)
$ErrorActionPreference = "Stop"
$AllSections = @("conflict", "settings", "password", "shell", "compress", "instance", "view", "viewmodes", "accent", "open", "listui", "preview", "flicker", "previewmeta", "animated", "fitmodes", "thumbs", "layout", "ctxmenu", "columns", "iconview", "nodecoder", "descend", "encoding", "i18n", "multiwin", "closeguard", "previewleak", "multipreview", "crash", "extractmodes", "compactopts", "afterbuttons", "saveimage", "workerproc", "background", "execopen", "audiopreview", "loadview", "bandfull", "tempdir", "previewpane", "settingssearch", "settingstabs", "titlebar", "paste", "tplmenu", "hash", "checksums", "firstframe", "hashsave", "feedbacka", "diaglog", "win11menu", "stalepath", "compactdone", "dataafterend", "keepdamaged", "dragimage", "previewonly", "shiftopen", "passwords", "masterpw", "repack", "reading", "stripdrag", "lockopen", "updatecheck", "installupdate", "virustotal", "colscroll", "newcols", "solidstream", "convertwall", "shellitems", "macmeta", "dupnames", "dragorder", "dragtemp", "treemenu", "previewfix", "smartstore", "modify", "editfile", "scan", "volumes", "existingout", "isocreate", "contrast", "welcome", "bombcheck")
if ($List) { $AllSections; exit 0 }
foreach ($sec in $Section) { if ($AllSections -notcontains $sec) { throw "未知的區段: $sec（可用：$($AllSections -join ', ')）" } }
function Want($n) { return ($Section.Count -eq 0) -or ($Section -contains $n) }
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "app\bin\$Config\LycanZip.exe"
if (-not (Test-Path $exe)) { throw "找不到 $exe，請先執行 scripts\build-app.ps1 -Config $Config" }
$exeDir = Split-Path $exe
$env:LYCAN_NO_STALE_CHECK = "1"   # no "the menus point to the old folder" prompt (the real registrations point to another build); the stalepath section turns it on
$env:LYCAN_TEST_STORE_INSTALLED = "0"   # as if the Microsoft Store version were not installed on this PC (it would switch the menu switches off)
$env:LYCAN_LANG = "zh-TW"   # the tests find controls by their Chinese names: fix the interface language so the system language of the test machine does not matter (the i18n section overrides it itself)
$portable = Join-Path $exeDir "LycanZip.portable"
$settingsFile = Join-Path $exeDir "settings.ini"
$shots = Join-Path $root "app\shots"
New-Item -ItemType Directory -Force $shots | Out-Null

. (Join-Path $PSScriptRoot "ui-helpers.ps1")


# ---- Prepare the test data ----
$t = Join-Path $env:TEMP "lycan_ui_$(Get-Date -Format HHmmss)"
New-Item -ItemType Directory "$t\src\資料夾\sub" | Out-Null
("hello" * 200) | Set-Content "$t\src\資料夾\a.txt"; "x" | Set-Content "$t\src\readme.txt"
[IO.File]::WriteAllBytes("$t\src\資料夾\sub\b.bin", [byte[]](1..255 * 600))
Push-Location "$t\src"; 7z a -tzip "$t\sample.zip" ".\資料夾" ".\readme.txt" | Out-Null
7z a -tzip -pSecret123 -mem=AES256 "$t\enc.zip" ".\資料夾" ".\readme.txt" | Out-Null
7z a -t7z -pSecret123 "$t\seven.7z" ".\資料夾" ".\readme.txt" | Out-Null
7z a -t7z -pSecret123 -mhe=on "$t\hdr.7z" ".\資料夾" ".\readme.txt" | Out-Null
Pop-Location
$vw = Join-Path $t "view"; New-Item -ItemType Directory $vw | Out-Null; Copy-Item "$t\sample.zip" "$vw\sample.zip"
Set-Content $portable "" ; if (Test-Path $settingsFile) { Remove-Item -LiteralPath $settingsFile -Force }


# ---- Shared helper functions (any section may use them) ----
function Reset-State { Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 400; if (Test-Path $settingsFile) { Remove-Item -LiteralPath $settingsFile -Force }; Remove-Item "$exeDir\presets.ini", "$exeDir\passwords.dat", "$exeDir\reading.tsv" -Force -ErrorAction SilentlyContinue
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details`nicon_thumbnails=false" -Encoding ascii }   # the details list is the view of most tests (the program starts new users in the icon view with thumbnails: the welcome section and the iconview / thumbs sections set their own)   # the compact window stays open by default now (the Done button); most of the tests are of what happens when it closes by itself (the compactdone section tests the default)
function Start-Raw([string[]]$argList) { if ($argList -and $argList.Count) { Start-Process $exe -ArgumentList $argList -PassThru } else { Start-Process $exe -PassThru } }
function Wait-Exit($proc, [int]$sec) { $proc.WaitForExit($sec * 1000) }
function Q($s) { "`"$s`"" }
    function Find-Row($w, $name) {
        foreach ($i in $w.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) {
            if ($i.FindFirst($TS::Descendants, (ByName $name))) { return $i }
        }
        return $null
    }
function Row-Text($w, $name) { (Find-Row $w $name).FindFirst($TS::Descendants, (ByName $name)) }   # the row in the list (the tree has text with the same name too, so Find cannot be used directly)
Add-Type -Namespace Mouse -Name M -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y); [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);' -ErrorAction SilentlyContinue
[Mouse.M]::SetProcessDPIAware() | Out-Null
    function Double-Click($el) {
        $r = $el.Current.BoundingRectangle; [Mouse.M]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 200
        1..2 | ForEach-Object { [Mouse.M]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Mouse.M]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }
        Start-Sleep -Milliseconds 800
    }
Add-Type -Namespace Win -Name D -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y); [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }' -ErrorAction SilentlyContinue
[Win.D]::SetProcessDPIAware() | Out-Null
Add-Type -Namespace Win -Name F -MemberDefinition '[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();' -ErrorAction SilentlyContinue
function Has-Text($w, $pattern, $sec = 8) { $end = (Get-Date).AddSeconds($sec); do { $hit = (All-Elements $w) | Where-Object { $_.Current.Name -like $pattern } | Select-Object -First 1; if (-not $hit) { Start-Sleep -Milliseconds 250 } } while (-not $hit -and (Get-Date) -lt $end); return ($null -ne $hit) }
function Preview-Window { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -like "pic*.png*") { return $w } }; return $null }

try {
    if (Want "conflict") {
        Reset-State
    # ===== 1. Default settings, conflict question =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "解壓縮到此")) "opens an archive from the command line"
    Click (Find $win "解壓縮到此")
    $r = Wait-Task $win "^完成　"; Check ($null -ne $r) "first extraction completes ($r)"
    Check (Test-Path "$t\sample\readme.txt") "smart extract created sample\ wrapper"

    Click (Find $win "解壓縮到此")                          # second time: the file exists already -> the default is "ask me"
    $dlgTitle = Wait-Find $win "檔案已存在" 8
    Check ($null -ne $dlgTitle) "conflict dialog appears (policy = ask)"
    if ($dlgTitle) {
        Shot $p "conflict-dialog"
        # 3 files conflict: tick "apply to all", it should be asked only once
        $cb = Find $win "對之後所有衝突套用相同的選擇"
        $cb.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 300
        Click (Find $win "確定")                              # the default is "keep both"
        Start-Sleep -Seconds 2
        Check (Test-Path "$t\sample\readme (2).txt") "conflict: default choice keeps both (renamed copy exists)"
        Check ((Test-Path "$t\sample\資料夾\a (2).txt") -and (Test-Path "$t\sample\資料夾\sub\b (2).bin")) "conflict: 'apply to all' renamed every conflicting file with one question"
        Check ($null -eq (Find $win "檔案已存在")) "conflict: no second dialog after 'apply to all'"
    }

    # Automatic choice: "keep the newer" - when the content and the time are the same the existing file is kept, no new copy is made, and the whole batch is asked only once
    Click (Find $win "解壓縮到此")
    $dlg2 = Wait-Find $win "檔案已存在" 8
    Check ($null -ne $dlg2) "conflict dialog appears again for the auto-rule test"
    if ($dlg2) {
        Select-Combo $win "自動選擇" "保留較新的"
        Click (Find $win "確定")
        Start-Sleep -Seconds 2
        Check (-not (Test-Path "$t\sample\readme (3).txt")) "auto rule 'keep newer': identical timestamps keep the existing file (no new copy)"
        Check ($null -eq (Find $win "檔案已存在")) "auto rule: asked only once for the whole batch"
    }
    }

    if (Want "settings") {
        Reset-State
    # ===== 2. Settings page =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "設定")
    Check ($null -ne (Wait-Find $win "同時執行的工作數")) "settings page opens"
    # The context menu switch must show the real state of the registry (read-only check; the test never changes the context menu of the user)
    Open-SettingsTab $win "檔案總管"
    $shellCond = New-Object Windows.Automation.AndCondition((ByName "在右鍵選單加入 LycanZip"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    $shellToggle = $null; $end = (Get-Date).AddSeconds(8)
    do { $shellToggle = $win.FindFirst($TS::Descendants, $shellCond); if (-not $shellToggle) { Start-Sleep -Milliseconds 250 } } while (-not $shellToggle -and (Get-Date) -lt $end)   # right after the settings page opens the UIA tree may not have it yet
    $regKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes\*\shell\LycanZipFiles'); $registered = $null -ne $regKey; if ($regKey) { $regKey.Close() }
    $toggleOn = $shellToggle.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On"
    Check ($toggleOn -eq $registered) "settings: shell-menu toggle matches the registry (registered=$registered)"
    # File associations: only check that the control exists, the switch agrees with the registry, and the default program state is shown (no click, no registration)
    $assocCond = New-Object Windows.Automation.AndCondition((ByName "註冊 LycanZip 為壓縮檔程式"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    $assocToggle = $win.FindFirst($TS::Descendants, $assocCond)
    Check ($null -ne $assocToggle) "settings: file-association toggle exists"
    if ($assocToggle) {
        $ra = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\RegisteredApplications'); $assocReg = $ra -and ($null -ne $ra.GetValue('LycanZip')); if ($ra) { $ra.Close() }
        $assocOn = $assocToggle.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On"
        Check ($assocOn -eq [bool]$assocReg) "settings: file-association toggle matches the registry (registered=$([bool]$assocReg))"
    }
    Check ($null -ne (Find $win "開啟預設應用程式設定")) "settings: 'open default apps' button exists"
    $statusEl = (All-Elements $win) | Where-Object { $_.Current.Name -like "*• .zip*" } | Select-Object -First 1
    Check ($null -ne $statusEl) "settings: current default apps for .zip/.7z/.rar are listed"
    Open-SettingsTab $win "壓縮"
    Select-Combo $win "檔案衝突處理" "覆寫"
    Open-SettingsTab $win "基本"
    Select-Combo $win "主題" "深色"
    Select-Combo $win "音效組" "Wave"   # the sound set could not be changed once (the index was clamped to 0)
    # Toggle the switches (on and off again, so that the test does not really open a folder); the label text has the same name as the switch, so restrict to the control type
    Open-SettingsTab $win "壓縮"
    $btnCond = New-Object Windows.Automation.AndCondition((ByName "完成後開啟資料夾"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    for ($k = 0; $k -lt 2; $k++) {
        $tog = $win.FindFirst($TS::Descendants, $btnCond)
        $tp = $tog.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern); $tp.Toggle(); Start-Sleep -Milliseconds 400
        if ($k -eq 0) { Check ($tp.Current.ToggleState -eq "On") "toggle switch turns on" }
    }
    # System notification switch: on by default; after switching it off it is written to the settings file (notifications=false)
    Open-SettingsTab $win "基本"
    $ntCond = New-Object Windows.Automation.AndCondition((ByName "系統通知"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    $ntTog = $win.FindFirst($TS::Descendants, $ntCond)
    Check ($null -ne $ntTog) "settings: a system-notification toggle exists"
    if ($ntTog) {
        $ntp = $ntTog.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
        Check ($ntp.Current.ToggleState -eq "On") "settings: system notifications are on by default"
        $ntp.Toggle(); Start-Sleep -Milliseconds 400
        Check ($ntp.Current.ToggleState -eq "Off") "settings: the notification toggle turns off"
    }
    Shot $p "settings"
    $ini = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw } else { "" }
    Check ($ini -match "conflict=overwrite") "settings persisted: conflict=overwrite"
    Check ($ini -match "theme=dark") "settings persisted: theme=dark"
    Check ($ini -match "sound_set=wave") "settings persisted: sound_set=wave (the sound set can be changed)"
    Check ($ini -match "notifications=false") "settings persisted: notifications=false"
    Click (Find $win "返回")
    Check ($null -ne (Wait-Find $win "解壓縮到此" 3)) "back from settings returns to the archive view"

    # The overwrite policy works: first tamper with the existing file, then extract, the content should be restored
    New-Item -ItemType Directory "$t\sample" -Force | Out-Null   # when this section runs alone, the earlier sections did not extract anything
    Set-Content "$t\sample\readme.txt" "TAMPERED"
    Click (Find $win "解壓縮到此"); Start-Sleep -Seconds 3
    $content = (Get-Content "$t\sample\readme.txt" -Raw).Trim()
    if ($content -ne "x") {
        Shot $p "overwrite-debug"
        (All-Elements $win) | Where-Object { $_.Current.Name -match "完成|失敗|等待|已存在|略過|處理" } | ForEach-Object { Write-Host "   task text: $($_.Current.Name)" }
    }
    Check ($content -eq "x") "overwrite policy: existing file replaced without asking (content='$content')"

    # Restart: the settings are still there
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "設定")
    Wait-Find $win "同時執行的工作數" | Out-Null
    Open-SettingsTab $win "壓縮"
    $sel = Combo-Selected (Find-Combo $win "檔案衝突處理")
    Check ($sel -eq "覆寫") "settings survive a restart (conflict combo shows '$sel')"
    }

    if (Want "password") {
        Reset-State
    # ===== 3. Password (zip with encrypted content): wrong password -> retry -> right =====
    $p = Start-App "$t\enc.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    $pw = Wait-Find $win "密碼" 8
    Check ($null -ne $pw) "password dialog appears for encrypted zip"
    if ($pw) {
        Type-Into (Find $win "密碼") "WRONG"; Click (Find $win "確定")
        $warn = Wait-Find $win "密碼不正確，請再試一次。" 8
        Check ($null -ne $warn) "wrong password is rejected and the dialog asks again"
        Shot $p "password-retry"
        Type-Into (Find $win "密碼") "Secret123"; Click (Find $win "確定")
        $r = Wait-Task $win "^完成　" 20
        Check ($null -ne $r) "correct password: extraction completes ($r)"
        Check (Test-Path "$t\enc\readme.txt") "encrypted zip extracted with correct content"
    }

    # ===== 4. Password (7z with data-level encryption): not a "false success" =====
    $p = Start-App "$t\seven.7z"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    $pw = Wait-Find $win "密碼" 8
    Check ($null -ne $pw) "password dialog appears for encrypted 7z"
    if ($pw) {
        Type-Into (Find $win "密碼") "nope"; Click (Find $win "確定")
        Check ($null -ne (Wait-Find $win "密碼不正確，請再試一次。" 8)) "7z: wrong password rejected up front"
        Click (Find $win "取消")
        Start-Sleep -Milliseconds 500
        Check (-not (Test-Path "$t\seven")) "cancelling the password dialog extracts nothing"
    }

    # ===== 5. 7z with encrypted headers: the password is needed at open =====
    $p = Start-App "$t\hdr.7z"; $win = Get-Win $p
    $pw = Wait-Find $win "密碼" 8
    Check ($null -ne $pw) "header-encrypted 7z asks for a password on open"
    if ($pw) {
        Type-Into (Find $win "密碼") "bad"; Click (Find $win "確定")
        Check ($null -ne (Wait-Find $win "密碼不正確，請再試一次。" 8)) "header-encrypted 7z: wrong password → asks again"
        Type-Into (Find $win "密碼") "Secret123"; Click (Find $win "確定")
        Check ($null -ne (Wait-Find $win "解壓縮到此" 8)) "header-encrypted 7z opens with the correct password"
    }
    }

    if (Want "shell") {
        Reset-State
    # ===== 6. Context menu actions (with the same command lines Explorer uses; nothing is written to the registry) =====

    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $qx = Join-Path $t "qx"; New-Item -ItemType Directory $qx | Out-Null
    Copy-Item "$t\sample.zip" "$qx\one.zip"; Copy-Item "$t\sample.zip" "$qx\two.zip"

    # Extract here: the compact window, closes by itself when done
    $p = Start-Raw @("--extract-here", (Q "$qx\one.zip"))
    Check (Wait-Exit $p 20) "extract-here: compact window closes by itself after success"
    Check (Test-Path "$qx\one\readme.txt") "extract-here: smart-extracted next to the archive"

    # Extract to a folder of the same name: the content goes straight into two\ (no extra wrapper)
    $p = Start-Raw @("--extract-folder", (Q "$qx\two.zip"))
    Check (Wait-Exit $p 20) "extract-folder: compact window closes by itself"
    Check ((Test-Path "$qx\two\readme.txt") -and (Test-Path "$qx\two\資料夾\a.txt")) "extract-folder: contents placed directly in <name>\"

    # Compress several: Explorer starts a process for each file, all processes should end, and only "one" archive is made
    $ms = Join-Path $t "multi"; New-Item -ItemType Directory $ms | Out-Null
    1..3 | ForEach-Object { Set-Content "$ms\file$_.txt" ("data$_" * 50) }
    $procs = 1..3 | ForEach-Object { Start-Raw @("--compress", "zip", (Q "$ms\file$_.txt")) }
    $allExited = $true; foreach ($pp in $procs) { if (-not (Wait-Exit $pp 25)) { $allExited = $false } }
    Check $allExited "multi-select compress: every launched process exits (forwarded or auto-closed)"
    $zips = @(Get-ChildItem $ms -Filter *.zip)
    Check ($zips.Count -eq 1) "multi-select compress: exactly one archive created ($($zips.Count))"
    if ($zips.Count -eq 1) {
        $names = (7z l -ba $zips[0].FullName | ForEach-Object { ($_ -split '\s+')[-1] }) -join ","
        Check (($names -match "file1.txt") -and ($names -match "file2.txt") -and ($names -match "file3.txt")) "multi-select compress: archive contains all three files ($names)"
    }

    # New context-menu actions: other formats, templates, one archive per item, integrity test, open content, encrypted
    $nx = Join-Path $t "newactions"; New-Item -ItemType Directory $nx | Out-Null
    1..2 | ForEach-Object { Set-Content "$nx\item$_.txt" ("data$_" * 50) }
    $p = Start-Raw @("--compress", "tar.gz", (Q "$nx\item1.txt"))
    Check (Wait-Exit $p 25) "compress tar.gz: compact window closes by itself"
    Check (Test-Path "$nx\item1.tar.gz") "compress tar.gz: item1.tar.gz created"
    $p = Start-Raw @("--compress", "zip", "--template", (Q "Smallest size"), (Q "$nx\item2.txt"))
    Check (Wait-Exit $p 25) "compress with a template: compact window closes by itself"
    Check (Test-Path "$nx\item2.7z") "compress with a template: the template's format (7z) wins over the given one"
    $p = Start-Raw @("--compress-each", "zip", (Q "$nx\item1.txt"), (Q "$nx\item2.txt"))
    Check (Wait-Exit $p 25) "compress-each: compact window closes by itself"
    Check ((Test-Path "$nx\item1.zip") -and (Test-Path "$nx\item2.zip")) "compress-each: one archive per item"
    $p = Start-Raw @("--test", (Q "$t\sample.zip"))
    Check (Wait-Exit $p 25) "test: compact window closes by itself after a passing test"
    Copy-Item "$t\sample.zip" "$nx\report.docx"
    $p = Start-Raw @("--open-content", (Q "$nx\report.docx"))
    $w3 = Get-Win $p
    Check ($null -ne (Wait-Find $w3 "readme.txt" 12)) "open-content: a .docx is opened as an archive and its entries are listed"
    Stop-Process -Id $p.Id -Force
    $p = Start-Raw @("--add-encrypted", (Q "$nx\item1.txt"))
    $w3 = Get-Win $p
    Check ($null -ne (Wait-Find $w3 "新增壓縮檔" 10)) "add-encrypted: shows the compress page"
    Start-Sleep -Milliseconds 1200
    $pwFocused = $false
    foreach ($e in $w3.FindAll($TS::Descendants, (ByName "密碼"))) { if ($e.Current.HasKeyboardFocus) { $pwFocused = $true } }
    Check $pwFocused "add-encrypted: the password box has the keyboard focus"
    Stop-Process -Id $p.Id -Force
    }

    # making an archive whose file exists already: save as a new name / replace / add to it (the update modes)
    if (Want "existingout") {
        Reset-State
        $eo = Join-Path $t "existing_out"; Remove-Item $eo -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $eo -Force | Out-Null
        Set-Content "$eo\a.txt" ("first " * 40)
        Set-Content "$eo\b.txt" ("second " * 40)
        function Pick-Radio($win, $text) {   # the radio button around a text (the text itself cannot be selected)
            $x = Find $win $text; $w = [Windows.Automation.TreeWalker]::ControlViewWalker
            while ($x) { $pp = $null; if ($x.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pp)) { $pp.Select(); return $true }; $x = $w.GetParent($x) }
            return $false
        }
        function Run-Add($files, $ext) { $pr = Start-Raw (@("--add") + ($files | ForEach-Object { Q $_ })); $ww = Get-Win $pr; Wait-Find $ww "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000; return @($pr, $ww) }
        # the first time the name is free: no question
        $pr, $ww = Run-Add @("$eo\a.txt")
        Click (Find $ww "開始壓縮")
        Check (Wait-Exit $pr 25) "existing output: a free name makes the archive without a question"
        Check (Test-Path "$eo\a.zip") "existing output: a.zip is made"
        # the second time the name is taken: it asks
        $pr, $ww = Run-Add @("$eo\a.txt")
        Click (Find $ww "開始壓縮")
        $q = Wait-Find $ww "檔案已經存在" 8
        Check ($null -ne $q) "existing output: a name that is taken asks what to do"
        Check (($null -ne (Find $ww "加入到這個壓縮檔")) -and ($null -ne (Find $ww "另存新檔")) -and ($null -ne (Find $ww "取代它"))) "existing output: it offers add to it, save as a new file and replace it"
        # the default is the old behaviour: a new name (2)
        Click (Find $ww "確定")
        Check (Wait-Exit $pr 25) "existing output: the window closes after the compression"
        Check (Test-Path "$eo\a (2).zip") "existing output: the default choice saves under a new name (a (2).zip)"
        # cancel: nothing is made, the page stays
        $pr, $ww = Run-Add @("$eo\a.txt")
        Click (Find $ww "開始壓縮")
        Wait-Find $ww "檔案已經存在" 8 | Out-Null
        Click (Find $ww "取消")
        Start-Sleep -Milliseconds 800
        Check (-not (Test-Path "$eo\a (3).zip")) "existing output: cancelling makes nothing"
        Stop-Process -Id $pr.Id -Force -ErrorAction SilentlyContinue
        # add to it (update mode: new files only come in when newer): the second source goes into a.zip
        $pr, $ww = Run-Add @("$eo\a.txt", "$eo\b.txt")
        Check ($null -ne (Find $ww "新增壓縮檔")) "existing output: the page for two files"
        Stop-Process -Id $pr.Id -Force -ErrorAction SilentlyContinue
        $pr, $ww = Run-Add @("$eo\a.txt")
        Click (Find $ww "開始壓縮")
        Wait-Find $ww "檔案已經存在" 8 | Out-Null
        Check (Pick-Radio $ww "加入到這個壓縮檔") "existing output: 'add to it' can be chosen"
        Start-Sleep -Milliseconds 500
        Check ($null -ne (Wait-Find $ww "同名檔案的處理方式" 4)) "existing output: choosing 'add to it' shows the update modes"
        Click (Find $ww "確定")
        Check (Wait-Exit $pr 25) "existing output: adding to the archive finishes"
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "existing_output=update") "existing output: the choice is remembered"
        Check (-not (Test-Path "$eo\a (3).zip")) "existing output: adding does not make another archive"
    }

    # creating an ISO image: the format list has ISO, the compression options are off, the label box shows, the image is made and readable
    if (Want "isocreate") {
        Reset-State
        $io = Join-Path $t "iso_create"; Remove-Item $io -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$io\disc\sub" -Force | Out-Null
        Set-Content "$io\disc\a.txt" ("alpha " * 50); Set-Content "$io\disc\sub\b.txt" ("beta " * 50)
        $p = Start-Raw @("--add", (Q "$io\disc"))
        $w2 = Get-Win $p
        Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
        Check ($null -eq (Find $w2 "磁碟標籤")) "iso: the label box is hidden for the other formats"
        Select-Combo $w2 "格式" "ISO"; Start-Sleep -Milliseconds 600
        Check ($null -ne (Find $w2 "磁碟標籤")) "iso: choosing ISO shows the label box"
        Check ($null -ne (Find $w2 "保留檔案的修改時間")) "iso: and the switch for the modified times"
        Check (Has-Text $w2 "ISO 沒有壓縮，也不支援密碼與分割。" 4) "iso: the page says that there is no compression, password or splitting"
        Check (-not (Find $w2 "密碼").Current.IsEnabled) "iso: the password box is disabled"
        Check (-not (Find-Combo $w2 "模式").Current.IsEnabled) "iso: the mode is disabled"
        Type-Into (Find $w2 "磁碟標籤") "SMOKE DISC"; Start-Sleep -Milliseconds 300
        Click (Find $w2 "開始壓縮")
        Check (Wait-Exit $p 30) "iso: the window closes after the image is written"
        Check (Test-Path "$io\disc.iso") "iso: disc.iso is made"
        $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
        $l = (& $cliExe l "$io\disc.iso" 2>&1 | Out-String)
        Check ($l.Contains("a.txt") -and $l.Contains("b.txt")) "iso: the image lists the files and the folder inside"
        $bytes = [IO.File]::ReadAllBytes("$io\disc.iso")
        Check (([Text.Encoding]::ASCII.GetString($bytes, 32768 + 1, 5) -eq "CD001") -and ([Text.Encoding]::ASCII.GetString($bytes, 32768 + 40, 10) -eq "SMOKE_DISC")) "iso: the volume descriptor has the label"
    }

    # the program's own high contrast look (settings > theme): the page is pure black, it is saved, it is there again at the next start, and the
    # other themes bring the usual background back (the colour of the page is read from the screen)
    if (Want "contrast") {
        Reset-State
        function Page-Pixel($w) {
            $r = $w.Current.BoundingRectangle
            $bm = New-Object System.Drawing.Bitmap 1, 1; $gg = [System.Drawing.Graphics]::FromImage($bm)
            $gg.CopyFromScreen([int]($r.X + $r.Width * 0.5), [int]($r.Y + 150), 0, 0, $bm.Size); return $bm.GetPixel(0, 0)
        }
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
        Click (Find $win "設定"); Start-Sleep -Milliseconds 900
        Select-Combo $win "主題" "高對比（黑底）"; Start-Sleep -Milliseconds 1200
        $px = Page-Pixel $win
        Check (($px.R -le 6) -and ($px.G -le 6) -and ($px.B -le 6)) "contrast: choosing the high contrast theme makes the page pure black (got $($px.R),$($px.G),$($px.B))"
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "theme=contrast-black") "contrast: the choice is saved (theme=contrast-black)"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
        $px = Page-Pixel $win
        Check (($px.R -le 6) -and ($px.G -le 6) -and ($px.B -le 6)) "contrast: it is there again at the next start (got $($px.R),$($px.G),$($px.B))"
        Click (Find $win "設定"); Start-Sleep -Milliseconds 900
        Select-Combo $win "主題" "深色"; Start-Sleep -Milliseconds 1200
        $px = Page-Pixel $win
        Check ($px.R + $px.G + $px.B -gt 20) "contrast: switching to the dark theme brings the usual background back (got $($px.R),$($px.G),$($px.B))"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    # the welcome window: shown at the first start (welcome_done=false), three steps, the first two show their choices at once and nothing
    # is kept before "Finish"; Skip changes nothing; nothing is registered with Windows except at "Finish" (the test registry root takes it)
    if (Want "welcome") {
        Reset-State
        $env:LYCAN_NO_WELCOME = $null; Remove-Item Env:LYCAN_NO_WELCOME -ErrorAction SilentlyContinue
        $env:LYCAN_TEST_REG_ROOT = "Software\LycanZipTest_welcome"
        Remove-Item "HKCU:\Software\LycanZipTest_welcome" -Recurse -Force -ErrorAction SilentlyContinue
        function Find-Welcome($proc, $sec = 12) {
            $end = (Get-Date).AddSeconds($sec)
            do {
                foreach ($w in $AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)))) { if ($w.Current.Name -like "歡迎使用*") { return $w } }
                Start-Sleep -Milliseconds 300
            } while ((Get-Date) -lt $end)
            return $null
        }
        function Pick($w, $name) {   # a radio button by its text
            $x = Find $w $name; $tw = [Windows.Automation.TreeWalker]::ControlViewWalker
            while ($x) { $pp = $null; if ($x.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pp)) { $pp.Select(); return $true }; $x = $tw.GetParent($x) }
            return $false
        }
        # 1. the first start: the window is there; step 1 has the language, the theme, the accent colour, sound and animations
        Set-Content -Path $settingsFile -Value "[state]`nwelcome_done=false`n" -Encoding UTF8
        $p = Start-Raw @()
        $wel = Find-Welcome $p
        Check ($null -ne $wel) "welcome: the window is shown at the first start"
        if ($wel) {
            foreach ($n in "語言", "主題", "主題色", "音效", "動畫") { Check ($null -ne (Find $wel $n)) "welcome: step 1 has '$n'" }
            Check ($null -ne (Find $wel "跟隨系統")) "welcome: the themes are offered (the system, light, dark, high contrast)"
            Pick $wel "高對比（黑底）" | Out-Null; Start-Sleep -Milliseconds 800
            Check (-not ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "theme=contrast-black")) "welcome: a choice is shown at once but not saved before Finish"
            Click (Find $wel "下一步"); Start-Sleep -Milliseconds 600
            foreach ($n in "預設檢視", "預設壓縮格式", "圖片預覽", "音訊預覽", "使用 Windows 的檔案圖示", "顯示資料夾樹") { Check ($null -ne (Find $wel $n)) "welcome: step 2 has '$n'" }
            # the picture follows the choices: the details view has a size column, the icon view does not
            Pick $wel "詳細資料" | Out-Null; Start-Sleep -Milliseconds 700
            Check ($null -ne (Find $wel "284 KB")) "welcome: the picture of step 2 shows the details view when it is chosen (a size column)"
            Pick $wel "中圖示" | Out-Null; Start-Sleep -Milliseconds 700
            Check ($null -eq (Find $wel "284 KB")) "welcome: and the icon view when that is chosen (no size column)"
            Check ($null -eq (Find $wel "圖示大小")) "welcome: no separate icon size (the sizes are in the view choice)"
            $med = Find $wel "中圖示"
            Check ($null -ne $med -and $med.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) "welcome: the default view is medium icons"
            Click (Find $wel "下一步"); Start-Sleep -Milliseconds 600
            foreach ($n in "傳統右鍵選單", "檔案關聯（開啟方式）") { Check ($null -ne (Find $wel $n)) "welcome: step 3 has '$n'" }
            Check ($null -eq (Find $wel "Windows 11 新式右鍵選單")) "welcome: step 3 has no Windows 11 menu row where it cannot be switched on (no signed menu package)"
            Check ($null -ne (Find $wel "完成")) "welcome: the last step ends with Finish"
            # the update check: off by default, offered on the last step, kept when it is switched on
            $upd = $null
            foreach ($e in $wel.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, "檢查新版本")))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$pt)) { $upd = $pt } }
            Check ($null -ne $upd) "welcome: the last step offers 'check for new versions'"
            Check ($upd -and $upd.Current.ToggleState -eq [Windows.Automation.ToggleState]::Off) "welcome: and it is off by default"
            if ($upd) { $upd.Toggle(); Start-Sleep -Milliseconds 400 }
            Check (-not (Test-Path "HKCU:\Software\LycanZipTest_welcome\Classes")) "welcome: nothing is registered before Finish"
            Click (Find $wel "完成"); Start-Sleep -Milliseconds 2500
            $ini = Get-Content $settingsFile -Raw -Encoding UTF8
            Check (($ini -match "welcome_done=true") -and ($ini -match "theme=contrast-black")) "welcome: Finish keeps the choices and the window is not due again"
            Check ($ini -match "check_updates=true") "welcome: Finish keeps the update check that was switched on"
            Check (Test-Path "HKCU:\Software\LycanZipTest_welcome\Classes") "welcome: Finish registers the menu and the associations (under the test key)"
            Check ($null -eq (Find-Welcome $p 1)) "welcome: the window is gone after Finish"
        }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 2. the next start: not again
        $p = Start-Raw @()
        Check ($null -eq (Find-Welcome $p 5)) "welcome: not shown again at the next start"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 3. Skip: nothing changes, and it is done
        Set-Content -Path $settingsFile -Value "[state]`nwelcome_done=false`n" -Encoding UTF8
        Remove-Item "HKCU:\Software\LycanZipTest_welcome" -Recurse -Force -ErrorAction SilentlyContinue
        $p = Start-Raw @()
        $wel = Find-Welcome $p
        if ($wel) {
            Pick $wel "深色" | Out-Null; Start-Sleep -Milliseconds 500
            Click (Find $wel "略過"); Start-Sleep -Milliseconds 1500
            $note = $null; $endN = (Get-Date).AddSeconds(8)
            while (-not $note -and (Get-Date) -lt $endN) { foreach ($w in $AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)))) { $e = Find $w "已略過設定"; if ($e) { $note = $e } }; if (-not $note) { Start-Sleep -Milliseconds 300 } }
            Check ($null -ne $note) "welcome: Skip says that everything can be changed in Settings (and that the menus and associations are left to the user)"
            $ini = Get-Content $settingsFile -Raw -Encoding UTF8
            Check (($ini -match "welcome_done=true") -and ($ini -notmatch "theme=dark")) "welcome: Skip changes nothing (the theme tried is not kept) and the window is done"
            Check ($ini -notmatch "check_updates=true") "welcome: Skip leaves the update check off"
            Check (-not (Test-Path "HKCU:\Software\LycanZipTest_welcome\Classes")) "welcome: Skip registers nothing"
        } else { Check $false "welcome: the window is shown (skip test)" }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 4. a settings file from before the window existed: no window
        Set-Content -Path $settingsFile -Value "compact_close_when_done=true" -Encoding ascii
        $p = Start-Raw @()
        Check ($null -eq (Find-Welcome $p 5)) "welcome: a settings file without the key (an older version) does not get the window"
        # the button on the About page brings it back
        $win = Get-Win $p
        Click (Find $win "設定"); Start-Sleep -Milliseconds 900
        Open-SettingsTab $win "關於"
        $btn = Wait-Find $win "重新顯示歡迎視窗" 5
        Check ($null -ne $btn) "welcome: the About page has 'show the welcome window again'"
        if ($btn) { Click $btn; Check ($null -ne (Find-Welcome $p)) "welcome: the button shows the window" }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 5. the first start is a double-click on an archive (the Store does not start the program after it is installed): no window, a notice above
        #    the list offers it; "No, thanks" counts as seen
        Set-Content -Path $settingsFile -Value "[state]`nwelcome_done=false`n" -Encoding UTF8
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
        Check ($null -eq (Find-Welcome $p 3)) "welcome: opened with an archive, the window does not cover the archive"
        Check ($null -ne (Wait-Find $win "第一次使用 LycanZip？" 6)) "welcome: opened with an archive, a notice offers the welcome window"
        $no = Find $win "不用了"
        if ($no) { Click $no; Start-Sleep -Milliseconds 900 }
        Check ($null -eq (Find $win "第一次使用 LycanZip？") -and ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "welcome_done=true")) "welcome: 'No, thanks' hides the notice and counts as seen"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1500
        Check ($null -eq (Find $win "第一次使用 LycanZip？")) "welcome: once seen, no notice any more"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 6. "Set up" in the notice opens the window
        Set-Content -Path $settingsFile -Value "[state]`nwelcome_done=false`n" -Encoding UTF8
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
        $go = Wait-Find $win "開始設定" 6
        if ($go) { Click $go }
        Check ($null -ne (Find-Welcome $p)) "welcome: 'Set up' in the notice opens the welcome window"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 7. the Store version: step 3 has the menu switch (a setting), the Windows 11 menu and the file types are built in
        Set-Content -Path $settingsFile -Value "[state]`nwelcome_done=false`n" -Encoding UTF8
        $env:LYCAN_TEST_STORE_UI = "1"
        $p = Start-Raw @()
        $wel = Find-Welcome $p
        if ($wel) {
            Click (Find $wel "下一步"); Start-Sleep -Milliseconds 600
            Click (Find $wel "下一步"); Start-Sleep -Milliseconds 600
            $cm = $null
            foreach ($e in $wel.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, "檔案總管的右鍵選單")))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$pt)) { $cm = $e } }
            Check ($cm -and $cm.Current.IsEnabled) "welcome (Store): one switch for the File Explorer menus, and it can be changed"
            Check ($null -eq (Find $wel "Windows 11 新式右鍵選單") -and $null -eq (Find $wel "傳統右鍵選單")) "welcome (Store): no separate classic or Windows 11 menu rows (the one switch covers both)"
            if ($cm) { $cm.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 300 }
            Click (Find $wel "完成"); Start-Sleep -Milliseconds 2500
            $ini = Get-Content $settingsFile -Raw -Encoding UTF8
            Check (($ini -match "shell_menu=false") -and -not (Test-Path "HKCU:\Software\LycanZipTest_welcome\Classes")) "welcome (Store): switching the menu off is kept as shell_menu=false and nothing is registered"
        } else { Check $false "welcome (Store): the window is shown" }
        Remove-Item Env:LYCAN_TEST_STORE_UI -ErrorAction SilentlyContinue
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Remove-Item "HKCU:\Software\LycanZipTest_welcome" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item Env:LYCAN_TEST_REG_ROOT -ErrorAction SilentlyContinue
        $env:LYCAN_NO_WELCOME = "1"
    }

    # an archive that would unpack to far more than it is (a compression bomb or a disk image): asked before anything is written; the tests make the
    # limits small (LYCAN_TEST_LARGE_BYTES) and use 3 MB of zeros in a few KB
    if (Want "bombcheck") {
        Reset-State
        $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
        $bd = Join-Path $t "bomb"; Remove-Item $bd -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$bd\src", "$bd\a", "$bd\b", "$bd\c", "$bd\d" -Force | Out-Null
        [IO.File]::WriteAllBytes("$bd\src\zeros.bin", (New-Object byte[] (3MB)))
        Set-Content "$bd\src\small.txt" "tiny"
        Start-Process $cliExe -ArgumentList @("a", (Q "$bd\zeros.zip"), (Q "$bd\src\zeros.bin"), "-y") -Wait -WindowStyle Hidden
        Start-Process $cliExe -ArgumentList @("a", (Q "$bd\plain.zip"), (Q "$bd\src\small.txt"), "-y") -Wait -WindowStyle Hidden
        foreach ($x in "a", "b", "c", "d") { Copy-Item "$bd\zeros.zip" "$bd\$x\zeros.zip" }
        Copy-Item "$bd\plain.zip" "$bd\d\plain.zip"
        $env:LYCAN_TEST_LARGE_BYTES = "1000000"
        # 1. the question; cancelling writes nothing
        $p = Start-Raw @("--extract-here", (Q "$bd\a\zeros.zip")); $w = Get-Win $p
        $q = Wait-Find $w "此壓縮檔展開後非常大" 15
        Check ($null -ne $q) "bomb check: an archive that unpacks to far more than it is asks first"
        Check ($null -ne (Find $w "繼續解壓縮")) "bomb check: the question offers to continue (and to cancel)"
        Click (Find $w "取消"); Start-Sleep -Milliseconds 2500
        Check (-not (Test-Path "$bd\a\zeros")) "bomb check: cancelling extracts nothing"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 2. continuing extracts
        $p = Start-Raw @("--extract-here", (Q "$bd\b\zeros.zip")); $w = Get-Win $p
        Wait-Find $w "此壓縮檔展開後非常大" 15 | Out-Null
        Click (Find $w "繼續解壓縮")
        $ok = $false; $end = (Get-Date).AddSeconds(20); while (-not $ok -and (Get-Date) -lt $end) { $ok = (Test-Path "$bd\b\zeros\zeros.bin") -or (Test-Path "$bd\b\zeros.bin"); if (-not $ok) { Start-Sleep -Milliseconds 300 } }
        Check $ok "bomb check: continuing extracts the archive"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 3. an ordinary archive does not ask
        $p = Start-Raw @("--extract-here", (Q "$bd\d\plain.zip")); $w = Get-Win $p
        Check ($null -eq (Wait-Find $w "此壓縮檔展開後非常大" 4)) "bomb check: an ordinary archive does not ask"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        # 4. 'do not check again' + continue turns the check off (saved), and it does not ask any more
        $p = Start-Raw @("--extract-here", (Q "$bd\c\zeros.zip")); $w = Get-Win $p
        Wait-Find $w "此壓縮檔展開後非常大" 15 | Out-Null
        Click (Find $w "不再檢查（可在設定中重新開啟）"); Start-Sleep -Milliseconds 300
        Click (Find $w "繼續解壓縮"); Start-Sleep -Milliseconds 2500
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "bomb_check=false") "bomb check: 'do not check again' is saved (bomb_check=false)"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
        $p = Start-Raw @("--extract-here", (Q "$bd\d\zeros.zip")); $w = Get-Win $p
        Check ($null -eq (Wait-Find $w "此壓縮檔展開後非常大" 4)) "bomb check: with the check off it does not ask"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Remove-Item Env:LYCAN_TEST_LARGE_BYTES -ErrorAction SilentlyContinue
    }

    if (Want "compress") {
        Reset-State
    $cm = Join-Path $t "multi_compress"; New-Item -ItemType Directory $cm -Force | Out-Null
    1..3 | ForEach-Object { Set-Content "$cm\file$_.txt" ("data$_" * 50) }
    # --add: shows the new archive dialog; after cancelling the compact window closes at once
    $p = Start-Raw @("--add", (Q "$cm\file1.txt"))
    $w2 = Get-Win $p
    Check ($null -ne (Wait-Find $w2 "新增壓縮檔" 10)) "--add shows the compress page"
    Start-Sleep -Milliseconds 1000
    Check (($null -ne (Wait-Find $w2 "更改位置" 6)) -and ($null -ne (Wait-Find $w2 "格式" 6)) -and ($null -ne (Wait-Find $w2 "模式" 6)) -and ($null -ne (Wait-Find $w2 "範本" 6))) "--add page: output location, format, mode and template controls are present"
    Check ($null -ne (Find $w2 "壓縮完成後測試完整性")) "--add page: has the 'test the integrity after compressing' switch"
    Check ($null -eq (Find $w2 "壓縮後刪除原檔")) "--add page: the 'delete the originals' switch is gone (it is a button on the finished card)"
    Click (Find $w2 "取消")
    Check (Wait-Exit $p 10) "--add: cancelling the page closes the window"

    # --add: choose the preset "一般 7z" and start compressing -> a .7z is made, the window shrinks back to the compact window and closes by itself when done
    $p = Start-Raw @("--add", (Q "$cm\file2.txt"))
    $w2 = Get-Win $p
    Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
    Select-Combo $w2 "範本" "一般 7z"
    Check ($null -ne (Find $w2 "加入檔案")) "--add page: sources can be extended"
    Click (Find $w2 "開始壓縮")
    $closed = Wait-Exit $p 25
    if (-not $closed) { Shot $p "addpage-stuck" }
    Check $closed "--add page: window closes after the compression finishes"
    Check (Test-Path "$cm\file2.7z") "--add page: 'general 7z' template produced file2.7z"

    # Preset: save as a new preset -> it is in presets.ini (the password is not written into it)
    $p = Start-Raw @("--add", (Q "$cm\file3.txt"))
    $w2 = Get-Win $p
    Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
    Select-Combo $w2 "格式" "7z"
    Type-Into (Find $w2 "密碼") "TopSecret1"
    Click (Wait-Find $w2 "範本選項" 5); Start-Sleep -Milliseconds 500
    $mi = Wait-Find $w2 "另存為新範本…" 5
    Check ($null -ne $mi) "template menu offers 'save as new template'"
    if ($mi) {
        Click $mi
        $nameBox = Wait-Find $w2 "範本名稱" 8
        Check ($null -ne $nameBox) "template: the name dialog appears"
        if ($nameBox) {
            Type-Into $nameBox "冒煙測試範本"; Click (Find $w2 "確定"); Start-Sleep -Milliseconds 800
            $presetsFile = Join-Path $exeDir "presets.ini"
            $pt = if (Test-Path $presetsFile) { Get-Content $presetsFile -Raw -Encoding UTF8 } else { "" }
            Check (($pt -match "\[冒煙測試範本\]") -and ($pt -match "format=7z")) "template: saved to presets.ini with its format"
            Check ($pt -notmatch "TopSecret1") "template: the password is never written to presets.ini"
        }
    }
    Click (Find $w2 "取消")
    Wait-Exit $p 10 | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item (Join-Path $exeDir "presets.ini") -Force -ErrorAction SilentlyContinue

    # The password is shown in plain text by default (Chinese can be typed) + custom split size
    $big = Join-Path $cm "bigdata.bin"
    $rnd = New-Object Random 7; $bytes = New-Object byte[] (3MB); $rnd.NextBytes($bytes); [IO.File]::WriteAllBytes($big, $bytes)
    $p = Start-Raw @("--add", (Q $big))
    $w2 = Get-Win $p
    Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
    $sp = Find $w2 "顯示密碼"
    Check (($null -ne $sp) -and ($sp.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On")) "password: 'show password' is checked by default"
    Select-Combo $w2 "格式" "7z"
    Type-Into (Find $w2 "密碼") "中文密碼123"
    Start-Sleep -Milliseconds 500
    Check ($null -eq (Find $w2 "再次輸入密碼")) "password: no confirmation box while the password is visible"
    $expander = $null
    foreach ($e in $w2.FindAll($TS::Descendants, (ByName "進階選項"))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pt)) { $expander = $pt; break } }
    Check ($null -ne $expander) "advanced options: the expander is available"
    if ($expander) { $expander.Expand(); Start-Sleep -Milliseconds 800 }
    Select-Combo $w2 "分割壓縮檔" "自訂…"
    $cv = Wait-Find $w2 "自訂分割大小" 5
    Check ($null -ne $cv) "volume: choosing custom reveals a size box"
    if ($cv) {
        Type-Into $cv "abc"; Start-Sleep -Milliseconds 500
        Check (-not (Find $w2 "開始壓縮").Current.IsEnabled) "volume: an invalid size disables Start"
        Type-Into $cv "1 MB"; Start-Sleep -Milliseconds 500
        if ($expander) { $expander.Collapse(); Start-Sleep -Milliseconds 500 }   # collapse: do not leave "advanced options expanded" in the settings file, it would affect later tests
        Click (Find $w2 "開始壓縮")
        Check (Wait-Exit $p 40) "volume: window closes after the split compression"
        Check ((Test-Path "$cm\bigdata.7z.001") -and (Test-Path "$cm\bigdata.7z.002")) "volume: custom 1 MB size produced multiple parts"
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }

    if (Want "instance") {
        Reset-State
    # Single instance: when the main window is open, start another process with an action -> it is handed over to the main window and exits at once
    $first = Start-Raw @(); $w1 = Get-Win $first; Wait-Find $w1 "開啟壓縮檔" | Out-Null
    $qy = Join-Path $t "qy"; New-Item -ItemType Directory $qy | Out-Null; Copy-Item "$t\sample.zip" "$qy\fwd.zip"
    $second = Start-Raw @("--extract-here", (Q "$qy\fwd.zip"))
    Check (Wait-Exit $second 12) "single instance: second process forwards its request and exits quickly"
    # a right-click action has a window of its own (a compact one): the window that is open is not used for its progress
    function Count-Windows($pid2) { @($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid2)))).Count }
    $most = 1; $end = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $end -and $most -lt 2) { $most = [Math]::Max($most, (Count-Windows $first.Id)); Start-Sleep -Milliseconds 50 }   # the compact window is there for a moment only
    while ((Get-Date) -lt $end -and -not (Test-Path "$qy\fwd\readme.txt")) { Start-Sleep -Milliseconds 100 }
    Check (Test-Path "$qy\fwd\readme.txt") "single instance: forwarded extraction produced files"
    Check ($most -ge 2) "single instance: the forwarded action ran in a window of its own (up to $most windows of the program)"
    Check ($null -eq (Wait-Task $w1 "^完成　" 2)) "single instance: the window that was open shows nothing of it"
    Start-Sleep -Milliseconds 2500
    Check ((Count-Windows $first.Id) -eq 1) "single instance: the compact window closed when it was done"
    Check (-not $first.HasExited) "single instance: main window stays open (not compact)"
    Stop-Process -Id $first.Id -Force; $first.WaitForExit(5000) | Out-Null   # wait until it has really ended: while the mutex of the single instance is still held, the next process would think it is a secondary process, hand its request over and leave

    # Encrypted archive + compact window: password dialog, retry on a wrong password, closes by itself when done
    $qz = Join-Path $t "qz"; New-Item -ItemType Directory $qz | Out-Null; Copy-Item "$t\enc.zip" "$qz\locked.zip"
    $p = Start-Raw @("--extract-here", (Q "$qz\locked.zip"))
    $w3 = Get-Win $p
    $pwd = Wait-Find $w3 "密碼" 10
    Check ($null -ne $pwd) "compact + encrypted: password dialog appears"
    if ($pwd) {
        Type-Into (Find $w3 "密碼") "WRONG"; Click (Find $w3 "確定")
        Check ($null -ne (Wait-Find $w3 "密碼不正確，請再試一次。" 8)) "compact + encrypted: wrong password asks again"
        Type-Into (Find $w3 "密碼") "Secret123"; Click (Find $w3 "確定")
        Check (Wait-Exit $p 25) "compact + encrypted: closes after a correct password and success"
        Check (Test-Path "$qz\locked\readme.txt") "compact + encrypted: extracted content present"
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

    # A damaged archive: the error dialog must stay for the user to read (it must not close by itself before it is shown), and closes only after OK
    $bad = Join-Path $t "bad.zip"; [IO.File]::WriteAllBytes($bad, [byte[]](1..200))
    $p = Start-Raw @("--extract-here", (Q $bad))
    $w4 = Get-Win $p
    Check ($null -ne (Wait-Find $w4 "無法開啟壓縮檔" 10)) "compact + corrupt archive: error dialog is shown"
    Start-Sleep -Seconds 2
    Check (-not $p.HasExited) "compact + corrupt archive: window stays open while the error is unread"
    if (-not $p.HasExited) {
        Click (Find $w4 "確定")
        Check (Wait-Exit $p 8) "compact + corrupt archive: window closes after the error is dismissed"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "view") {
        Reset-State
    # ===== 7. Archive view: folder navigation, list everything, search, ticking, extract only the selection (and strip the upper path) =====
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800

    # The default is one level browsing: the root shows only the "folder" and readme.txt, deeper files are not seen
    Check (($null -ne (Find-Row $win "資料夾")) -and ($null -ne (Find-Row $win "readme.txt")) -and ($null -eq (Find-Row $win "a.txt"))) "view: default is single-level (root shows folder + readme only)"
    Check ($null -ne (Find $win "資料夾樹")) "view: folder tree is shown by default"
    Shot $p "view-root"

    # Double click a folder to enter; the breadcrumb appears; "up one level" is usable
    Double-Click (Row-Text $win "資料夾")
    Check (($null -ne (Find-Row $win "sub")) -and ($null -ne (Find-Row $win "a.txt")) -and ($null -eq (Find-Row $win "readme.txt"))) "view: double-click enters the folder"
    Check ((Find $win "上一層").Current.IsEnabled) "view: 'up' is enabled inside a folder"

    # List everything: flatten all levels and show the folder column; then back to one level
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "全部列出"); Start-Sleep -Milliseconds 600
    Check (($null -ne (Find-Row $win "b.bin")) -and ($null -ne (Find $win "所在路徑"))) "view: 'list all' flattens every level and shows the path column"
    Check (($null -eq (Find-Row $win "sub")) -and ($null -ne (Find-Row $win "a.txt"))) "view: 'list all' shows files only (no folder rows)"
    Check ($null -ne (Find $win "依類型排序")) "view: the details header has a sortable type column"
    Click (Find $win "依所在路徑排序"); Start-Sleep -Milliseconds 500
    Check (Has-Text $win "所在路徑 ▲" 3) "view: the path header is clickable and shows the sort arrow"
    Click (Find $win "依類型排序"); Start-Sleep -Milliseconds 500
    Check (Has-Text $win "類型 ▲" 3) "view: clicking the type header sorts by type (arrow shown)"
    Click (Find $win "依名稱排序"); Start-Sleep -Milliseconds 400
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "全部列出"); Start-Sleep -Milliseconds 600
    Check ($null -eq (Find-Row $win "b.bin")) "view: toggling back returns to single-level"

    # Search (with subfolders)
    # Ctrl+F moves the focus to the search box (handled in KeyDown now, no longer with the XAML shortcut)
    $box = Find $win "搜尋"
    try { (Find-Row $win "readme.txt").SetFocus() } catch { }; Start-Sleep -Milliseconds 300   # the focus is on a list item (the list handles keys first, which is the real situation)
    [System.Windows.Forms.SendKeys]::SendWait("^f"); Start-Sleep -Milliseconds 600
    Check ([bool]$box.Current.HasKeyboardFocus) "Ctrl+F focuses the search box"
    # After a click on an empty part of the list (focus leaves all items) Ctrl+F must still work
    $rowEl = Find-Row $win "readme.txt"
    try { $rowEl.SetFocus() } catch { }; Start-Sleep -Milliseconds 300   # leave the search box first
    $allRows = @($win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem))))
    $rr = $allRows[$allRows.Count - 1].Current.BoundingRectangle   # below the last row is the empty part of the list
    [Mouse.M]::SetCursorPos([int]($rr.X + $rr.Width / 2), [int]($rr.Y + $rr.Height + 150)) | Out-Null; Start-Sleep -Milliseconds 200
    [Mouse.M]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Mouse.M]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("^f"); Start-Sleep -Milliseconds 600
    Check ([bool]$box.Current.HasKeyboardFocus) "Ctrl+F still works after clicking empty list space"
    Type-Into $box "b.b"
    $end = (Get-Date).AddSeconds(6); do { Start-Sleep -Milliseconds 300 } while (-not (Find-Row $win "b.bin") -and (Get-Date) -lt $end)   # the search has an input pause of 250 ms
    Start-Sleep -Milliseconds 400
    Check (($null -ne (Find-Row $win "b.bin")) -and ($null -eq (Find-Row $win "a.txt"))) "view: search finds b.bin in a sub-folder and hides non-matches"
    Type-Into $box ""; Start-Sleep -Milliseconds 900
    Check ($null -ne (Find-Row $win "a.txt")) "view: clearing the search restores the folder listing"

    # Tick sub (with the UIA selection mode, same as ticking the checkbox) -> selection bar -> extract only the selected item, and strip the upper "folder\"
    $row = Find-Row $win "sub"; $pat = $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern); $pat.AddToSelection(); Start-Sleep -Milliseconds 500
    Check ($null -ne (Find $win "解壓選取項目到此處")) "view: selecting an item shows the selection bar"
    Click (Find $win "解壓選取項目到此處")
    $r = Wait-Task $win "^完成　" 15
    Check ($null -ne $r) "view: selected-only extraction completes ($r)"
    Check ((Test-Path "$vw\sub\b.bin") -and -not (Test-Path "$vw\資料夾") -and -not (Test-Path "$vw\readme.txt")) "view: only the selected folder was extracted, without the parent path"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "viewmodes") {
        Reset-State
    # ===== 7b. Switching the view (list / icons) and the selection model: click a row = select only it, checkbox = add, double click does not change the selection =====
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "大圖示"); Start-Sleep -Milliseconds 800
    Check (($null -ne (Find-Row $win "資料夾")) -and ($null -ne (Find-Row $win "readme.txt"))) "view: icon view lists the same items"
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "清單"); Start-Sleep -Milliseconds 800
    Check (($null -ne (Find-Row $win "資料夾")) -and ($null -ne (Find-Row $win "readme.txt"))) "view: list view lists the same items"
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "詳細資料"); Start-Sleep -Milliseconds 800
    Check ($null -ne (Find $win "依大小排序")) "view: details view shows the sortable column headers"

    # A click on a row selects only that row; with a selection, a click on another row -> select that row instead (not add)
    $r1 = Find-Row $win "資料夾"; $r2 = Find-Row $win "readme.txt"
    $pt = $r1.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern); $pt.Select(); Start-Sleep -Milliseconds 300
    $pt2 = $r2.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern); $pt2.Select(); Start-Sleep -Milliseconds 400
    Check ((-not $pt.Current.IsSelected) -and $pt2.Current.IsSelected) "select: selecting another row replaces the selection (explorer-style)"
    # Checkbox adds: click the checkbox of the row with the mouse (it appears only after moving over)
    $cr = $r1.Current.BoundingRectangle
    [Mouse.M]::SetCursorPos([int]($cr.X + $cr.Width / 2), [int]($cr.Y + $cr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 100
    [Mouse.M]::mouse_event(1, 3, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400
    $chk = $r1.FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, "選取")))
    if ($chk) {
        $br = $chk.Current.BoundingRectangle
        [Mouse.M]::SetCursorPos([int]($br.X + $br.Width / 2), [int]($br.Y + $br.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 200
        [Mouse.M]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [Mouse.M]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 600
    }
    $r1 = Find-Row $win "資料夾"; $r2 = Find-Row $win "readme.txt"
    Check ($r1.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected -and $r2.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) "select: the row checkbox adds to the selection"
    Check ($null -ne (Find $win "解壓選取項目到此處")) "select: selection bar appears for a multi-selection"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "accent") {
        Reset-State
    # ===== 7c. Accent themes and coloured category icons =====
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "設定"); Start-Sleep -Milliseconds 700
    $swatch = Find $win "血月"
    Check ($null -ne $swatch) "theme: the accent picker lists the themes"
    if ($swatch) {
        $swatch.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 600
        Check ((Get-Content $settingsFile -Raw) -match "accent=blood_moon") "theme: choosing an accent persists accent=blood_moon"
        Shot $p "accent-blood-moon"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 700
    $sw2 = Find $win "血月"
    Check ($sw2 -and $sw2.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) "theme: the chosen accent survives a restart"
    $m = Find $win "月光"; if ($m) { $m.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400 }   # restore the default
    # Background: changing the drop-down to "gradient only" writes the setting (and applies it at once); then back to "full"
    Select-Combo $win "背景" "僅漸層"
    Check ((Get-Content $settingsFile -Raw) -match "background=gradient") "background: choosing gradient-only persists background=gradient"
    Select-Combo $win "背景" "完整"
    # Restore the default settings: first change the accent to Blood moon and the background to off, then restore -> both go back to the default
    $sw = Find $win "血月"; if ($sw) { $sw.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400 }
    Select-Combo $win "背景" "關閉"
    Click (Find $win "還原預設設定")
    Check ($null -ne (Wait-Find $win "還原預設設定？" 5)) "reset: asks for confirmation"
    Click (Find $win "還原"); Start-Sleep -Milliseconds 800
    $iniNow = Get-Content $settingsFile -Raw
    Check (($iniNow -match "accent=moonlight") -and ($iniNow -match "background=full")) "reset: settings return to the defaults"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "open") {
        Reset-State
    # ===== 8. Double click a file: first extract to a temporary place (only that file, without the upper path), then hand it to the default program =====
    $tmpOpen = Join-Path ([IO.Path]::GetTempPath()) "LycanZip\open"
    if (Test-Path $tmpOpen) { Get-ChildItem $tmpOpen -Directory | ForEach-Object { [IO.Directory]::Delete($_.FullName, $true) } }
    $before = @(Get-Process -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Double-Click (Row-Text $win "資料夾")
    Double-Click (Row-Text $win "a.txt")
    $hit = $null; $end = (Get-Date).AddSeconds(12)
    while (-not $hit -and (Get-Date) -lt $end) { $hit = Get-ChildItem $tmpOpen -Recurse -Filter a.txt -ErrorAction SilentlyContinue | Select-Object -First 1; Start-Sleep -Milliseconds 300 }
    Check ($null -ne $hit) "open: double-clicking a file extracts it to a temp folder"
    if ($hit) {
        Check ((Split-Path $hit.Directory.FullName -Leaf) -notmatch "資料夾") "open: temp copy has no parent folders (just the file)"
        Check ((Get-Content $hit.FullName -Raw).StartsWith("hello")) "open: temp copy has the right content"
    }
    Start-Sleep -Seconds 2
    # The default program (Notepad etc.) may have been started: close only the processes that are new during this test and whose window title has a.txt
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id -and $_.MainWindowTitle -like "*a.txt*" } | Stop-Process -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "listui") {
        Reset-State
    # ===== 9. Minimum window size, dragging items out to Explorer =====
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $dg = Join-Path $t "dragtest"; $dn = "lzdest" + (Get-Date -Format HHmmss); New-Item -ItemType Directory "$dg\$dn" | Out-Null; Copy-Item "$t\sample.zip" "$dg\sample.zip"
    Set-Content -LiteralPath "$dg\sample.zip" -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3"   # downloaded: what is dragged out must keep the mark
    $p = Start-App "$dg\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $scale = [Win.D]::GetDpiForWindow($p.MainWindowHandle) / 96.0
    [Win.D]::MoveWindow($p.MainWindowHandle, 50, 50, 300, 300, $true) | Out-Null; Start-Sleep -Milliseconds 600
    $rect = New-Object Win.D+RECT; [Win.D]::GetWindowRect($p.MainWindowHandle, [ref]$rect) | Out-Null
    Check ((($rect.R - $rect.L) -ge [int](630 * $scale)) -and (($rect.B - $rect.T) -ge [int](470 * $scale))) "min size: the window cannot be shrunk below the minimum ($($rect.R - $rect.L) x $($rect.B - $rect.T) px)"

    # Band selection with the mouse: press on an empty part of the list and drag up over all rows -> all are selected
    $scr0 = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    function MoveAbs0($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr0.Width), [uint32]($y * 65535 / $scr0.Height), 0, [UIntPtr]::Zero) }
    [Win.D]::MoveWindow($p.MainWindowHandle, 20, 20, 1400, 1100, $true) | Out-Null; Start-Sleep -Milliseconds 1200
    $listRows = @($win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem))))
    Check ($listRows.Count -ge 2) "band select: the archive root lists at least two rows"
    if ($listRows.Count -ge 2) {
        $first = $listRows[0].Current.BoundingRectangle; $last = $listRows[$listRows.Count - 1].Current.BoundingRectangle
        $bx = [int]($last.X + $last.Width * 0.6); $by0 = [int]($last.Y + $last.Height + 160); $by1 = [int]($first.Y - 4)
        [Win.D]::SetCursorPos($bx, $by0) | Out-Null; Start-Sleep -Milliseconds 400
        [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        for ($i = 1; $i -le 30; $i++) { MoveAbs0 ($bx + $i) ([int]($by0 + ($by1 - $by0) * $i / 30)); Start-Sleep -Milliseconds 30 }
        Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 600
        $selCount = 0; foreach ($r in $listRows) { if ($r.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $selCount++ } }
        Check ($selCount -eq $listRows.Count) "band select: dragging a box from empty space selects every row it touches ($selCount of $($listRows.Count))"
        # Esc clears the selection
        try { $listRows[0].SetFocus() } catch { }
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 500
        $selCount = 0; foreach ($r in $listRows) { if ($r.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $selCount++ } }
        Check ($selCount -eq 0) "shortcut: Esc clears the selection"
    }

    # Keyboard shortcuts
    function Toggle-State($name) {   # 'list all' and 'folder tree' are check items of the View menu now: open it, read, close it
        Click (Find $win "檢視"); Start-Sleep -Milliseconds 500
        $st = "missing"
        foreach ($e in $win.FindAll($TS::Descendants, (ByName $name))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$pt)) { $st = $pt.Current.ToggleState; break } }
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
        return $st
    }
    try { (Find $win "檢視").SetFocus() } catch { }; Start-Sleep -Milliseconds 300
    $flat0 = Toggle-State "全部列出"; [System.Windows.Forms.SendKeys]::SendWait("^+f"); Start-Sleep -Milliseconds 700
    Check ((Toggle-State "全部列出") -ne $flat0) "shortcut: Ctrl+Shift+F toggles 'list all'"
    [System.Windows.Forms.SendKeys]::SendWait("^+f"); Start-Sleep -Milliseconds 500
    $tree0 = Toggle-State "資料夾樹開關"; [System.Windows.Forms.SendKeys]::SendWait("^b"); Start-Sleep -Milliseconds 600
    Check ((Toggle-State "資料夾樹開關") -ne $tree0) "shortcut: Ctrl+B toggles the folder tree"
    [System.Windows.Forms.SendKeys]::SendWait("^b"); Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("^t")
    $r = Wait-Task $win "完整性測試通過" 15
    Check ($null -ne $r) "shortcut: Ctrl+T runs the integrity test ($r)"
    [System.Windows.Forms.SendKeys]::SendWait("^,")
    Check ($null -ne (Wait-Find $win "同時執行的工作數" 8)) "shortcut: Ctrl+, opens the settings page"
    Open-SettingsTab $win "快速鍵"
    $sc = Wait-Find $win "快速鍵" 5
    Check ($null -ne $sc) "settings: a shortcuts reference section exists"
    Check ($null -ne (Find $win "Ctrl + E")) "settings: the shortcuts table lists Ctrl + E"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
    Check ($null -ne (Wait-Find $win "檢視" 5)) "shortcut: Esc leaves the settings page"

    # Drag: LycanZip on the left, Explorer on the right (opened on the empty folder dest); drag "readme.txt" and the "folder" over
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$dg\$dn`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*$dn*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "drag out: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        function Drag-Row($name) {
            $row = $null; foreach ($i in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.FindFirst($TS::Descendants, (ByName $name))) { $row = $i; break } }
            if (-not $row) { return }
            $r = $row.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700   # select first
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }   # move in small steps first, so that the drag recognizer sees "press and move"
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 2
        }
        $tmpRoot = Join-Path $env:TEMP "LycanZip\open"
        function Drag-Dirs { @(Get-ChildItem $tmpRoot -Filter "drag-*" -ErrorAction SilentlyContinue).Count }
        $dragBefore = Drag-Dirs
        Drag-Row "readme.txt"
        Check (Test-Path "$dg\$dn\readme.txt") "drag out: a file dropped on Explorer is extracted there"
        Check ((Drag-Dirs) -eq $dragBefore) "drag out: nothing was unpacked to the temporary folder (the files are streamed)"
        7z e "$dg\sample.zip" readme.txt "-o$dg\expected" -y | Out-Null
        Check ((Test-Path "$dg\$dn\readme.txt") -and ((Get-FileHash "$dg\$dn\readme.txt").Hash -eq (Get-FileHash "$dg\expected\readme.txt").Hash)) "drag out: the content is identical to the archive's"
        $zone = Get-Content -LiteralPath "$dg\$dn\readme.txt" -Stream Zone.Identifier -ErrorAction SilentlyContinue
        Check (($zone -join "`n") -match "ZoneId=3") "drag out: the dropped file keeps the Mark of the Web of the archive (Zone.Identifier: $($zone -join ' '))"
        Drag-Row "資料夾"
        Check ((Test-Path "$dg\$dn\資料夾\a.txt") -and (Test-Path "$dg\$dn\資料夾\sub\b.bin")) "drag out: a folder dropped on Explorer keeps its whole tree"
        $zone = Get-Content -LiteralPath "$dg\$dn\資料夾\sub\b.bin" -Stream Zone.Identifier -ErrorAction SilentlyContinue
        Check (($zone -join "`n") -match "ZoneId=3") "drag out: the files of a dropped folder keep the Mark of the Web"
        $strays = @(Get-ChildItem -LiteralPath "$dg\$dn" -Recurse -Force | Where-Object { $_.Name -like "*Zone*" })
        Check ($strays.Count -eq 0) "drag out: the mark is a stream of the file, not a file of its own ($($strays.Name -join ', '))"
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    # Ctrl+W closes the archive and goes back to the start page
    try { (Find $win "檢視").SetFocus() } catch { }
    [System.Windows.Forms.SendKeys]::SendWait("^w")
    Check ($null -ne (Wait-Find $win "開啟壓縮檔" 6)) "shortcut: Ctrl+W closes the archive and returns home"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "preview") {
        Reset-State
    # ===== 10. Picture preview (separate window, read only when clicked) =====
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500
    $pv = Join-Path $t "preview"; New-Item -ItemType Directory "$pv\src" | Out-Null
    foreach ($i in 1, 2) {
        $bmp = New-Object System.Drawing.Bitmap (64 * $i), 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
        $bmp.Save("$pv\src\pic$i.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Set-Content "$pv\src\note.txt" "x"
    Push-Location "$pv\src"; 7z a -tzip "$pv\img.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$pv\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $imgRow = Find-Row $win "pic1.png"
    Check ($null -ne $imgRow) "preview: the archive lists the image"
    if ($imgRow) {
        $imgRow.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        Check ($null -ne $pw) "preview: Alt+P opens a separate preview window named after the file"
        if ($pw) {
            Check ($null -ne (Wait-Find $pw "1 / 2" 8)) "preview: the counter shows 1 / 2 (only images of the folder are paged)"
            $dim = $null; $end = (Get-Date).AddSeconds(10); do { $dim = (All-Elements $pw) | Where-Object { $_.Current.Name -like "64 × 48*" } | Select-Object -First 1; if (-not $dim) { Start-Sleep -Milliseconds 300 } } while (-not $dim -and (Get-Date) -lt $end)
            Check ($null -ne $dim) "preview: the image is read from the archive and decoded (64 x 48)"
            [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 800
            $dim2 = $null; $end = (Get-Date).AddSeconds(10); do { $dim2 = (All-Elements $pw) | Where-Object { $_.Current.Name -like "128 × 48*" } | Select-Object -First 1; if (-not $dim2) { Start-Sleep -Milliseconds 300 } } while (-not $dim2 -and (Get-Date) -lt $end)
            Check ($null -ne $dim2) "preview: Right arrow shows the next image (128 x 48)"
            [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
            Check ($null -eq (Preview-Window)) "preview: Esc closes the preview window"
        }
    }
    # Thumbnail strip: the pictures of the same folder are listed on the left
    [System.Windows.Forms.SendKeys]::SendWait("%p")
    $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
    if ($pw) {
        Check ($null -ne (Wait-Find $pw "縮圖列" 8)) "preview: the thumbnail strip is shown"
        $tiles = @($pw.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem))))
        Check ($tiles.Count -eq 2) "preview: the strip has one tile per image ($($tiles.Count))"
        # Mouse wheel switches to the previous / next picture (down = next)
        $pr = $pw.Current.BoundingRectangle
        [Win.D]::SetCursorPos([int]($pr.X + $pr.Width * 0.6), [int]($pr.Y + $pr.Height * 0.5)) | Out-Null; Start-Sleep -Milliseconds 400
        [Win.D]::mouse_event(0x0800, 0, 0, [uint32]4294967176, [UIntPtr]::Zero)   # WHEEL, -120
        Check ($null -ne (Wait-Find $pw "2 / 2" 6)) "preview: mouse wheel down shows the next image"
        [Win.D]::mouse_event(0x0800, 0, 0, [uint32]120, [UIntPtr]::Zero)          # WHEEL, +120
        Check ($null -ne (Wait-Find $pw "1 / 2" 6)) "preview: mouse wheel up shows the previous image"
        # Pressing "next" quickly must not count as a double click on the title bar (it would maximize / restore the window)
        $nb = Find $pw "下一張（→）"; $nr = $nb.Current.BoundingRectangle
        [Win.D]::SetCursorPos([int]($nr.X + $nr.Width / 2), [int]($nr.Y + $nr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 400
        1..2 | ForEach-Object { [Win.D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
        Start-Sleep -Milliseconds 900
        $vs = $pw.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Current.WindowVisualState
        Check ($vs -eq "Normal") "preview: clicking 'next' twice quickly does not maximize the window ($vs)"
        # Shortcuts for zoom, paging, rotate, thumbnail strip, full screen, clipboard etc.
        [System.Windows.Forms.SendKeys]::SendWait("{ADD}")
        Check (Has-Text $pw "*125%") "preview: '+' zooms in by 25% (125%)"
        [System.Windows.Forms.SendKeys]::SendWait("^1")
        Check (Has-Text $pw "*100%") "preview: Ctrl+1 shows the actual size (100%)"
        [System.Windows.Forms.SendKeys]::SendWait("{SUBTRACT}")
        Check (Has-Text $pw "*80%") "preview: '-' zooms out (80%)"
        [System.Windows.Forms.SendKeys]::SendWait("^0")
        Check (Has-Text $pw "符合視窗（100%）") "preview fit modes: Ctrl+0 fits the window, a small image is not enlarged by default (100%)"
        [System.Windows.Forms.SendKeys]::SendWait("^2")
        Check (Has-Text $pw "符合寬度（100%）") "preview fit modes: Ctrl+2 fits the width (small image stays 100%)"
        [System.Windows.Forms.SendKeys]::SendWait("^3")
        Check (Has-Text $pw "符合高度（100%）") "preview fit modes: Ctrl+3 fits the height (small image stays 100%)"
        [System.Windows.Forms.SendKeys]::SendWait("^0")
        [System.Windows.Forms.SendKeys]::SendWait("r"); Start-Sleep -Milliseconds 300; [System.Windows.Forms.SendKeys]::SendWait("+r"); Start-Sleep -Milliseconds 300
        Check ($null -ne (Preview-Window)) "preview: R / Shift+R rotate without closing the window"
        # a rotation of 90 degrees shows the whole picture with the sides swapped (64 x 48 -> 48 x 64): it was squeezed into the unrotated box once
        Add-Type -Namespace PvRot -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); public struct RECT { public int L, T, R, B; }' -ErrorAction SilentlyContinue
        function Picture-Box($pwin) {
            [PvRot.W]::SetProcessDPIAware() | Out-Null
            $hh = [IntPtr]$pwin.Current.NativeWindowHandle; $rr = New-Object PvRot.W+RECT; [PvRot.W]::GetWindowRect($hh, [ref]$rr) | Out-Null
            $bm = New-Object System.Drawing.Bitmap ($rr.R - $rr.L), ($rr.B - $rr.T); $gg = [System.Drawing.Graphics]::FromImage($bm); $dc = $gg.GetHdc(); [PvRot.W]::PrintWindow($hh, $dc, 2) | Out-Null; $gg.ReleaseHdc($dc); $gg.Dispose()
            $x0 = 99999; $y0 = 99999; $x1 = -1; $y1 = -1
            for ($y = 80; $y -lt $bm.Height - 60; $y += 2) { for ($x = [int]($bm.Width * 0.18); $x -lt $bm.Width; $x += 2) {   # (right of the thumbnail strip)
                $c = $bm.GetPixel($x, $y)
                if ([Math]::Abs($c.R - 100) -lt 14 -and [Math]::Abs($c.G - 149) -lt 14 -and [Math]::Abs($c.B - 237) -lt 14) { if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }; if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y } } } }
            $bm.Dispose()
            if ($x1 -lt 0) { return 0 }
            return ($y1 - $y0 + 2) / ($x1 - $x0 + 2)
        }
        $pwin = Preview-Window
        if ($pwin) {
            $ratio0 = Picture-Box $pwin
            [System.Windows.Forms.SendKeys]::SendWait("r"); Start-Sleep -Milliseconds 700
            $ratio90 = Picture-Box $pwin
            Check (($ratio0 -gt 0.65 -and $ratio0 -lt 0.85) -and ($ratio90 -gt 1.2 -and $ratio90 -lt 1.5)) ("preview: rotating by 90 degrees shows the whole picture with its sides swapped (height / width {0:N2} -> {1:N2})" -f $ratio0, $ratio90)
            [System.Windows.Forms.SendKeys]::SendWait("r"); Start-Sleep -Milliseconds 300; [System.Windows.Forms.SendKeys]::SendWait("r"); Start-Sleep -Milliseconds 700
            $ratio270 = Picture-Box $pwin
            Check ($ratio270 -gt 1.2 -and $ratio270 -lt 1.5) ("preview: rotating by 270 degrees does the same ({0:N2})" -f $ratio270)
            [System.Windows.Forms.SendKeys]::SendWait("r"); Start-Sleep -Milliseconds 500
        }
        [System.Windows.Forms.SendKeys]::SendWait(" ")
        Check (Has-Text $pw "2 / 2") "preview: Space moves to the next image"
        [System.Windows.Forms.SendKeys]::SendWait("{BACKSPACE}"); Start-Sleep -Milliseconds 600
        [System.Windows.Forms.SendKeys]::SendWait("t"); Start-Sleep -Milliseconds 600
        Check ($null -eq (Find $pw "縮圖列")) "preview: T hides the thumbnail strip"
        [System.Windows.Forms.SendKeys]::SendWait("t"); Start-Sleep -Milliseconds 600
        Check ($null -ne (Find $pw "縮圖列")) "preview: T shows the thumbnail strip again"
        [System.Windows.Forms.SendKeys]::SendWait("{F11}"); Start-Sleep -Milliseconds 1200
        Check ($null -eq (Find $pw "下一張（→）")) "preview: F11 goes full screen and hides the toolbar"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 1200
        Check (($null -ne (Preview-Window)) -and ($null -ne (Find $pw "下一張（→）"))) "preview: Esc leaves full screen first (the window stays open)"
        [System.Windows.Forms.SendKeys]::SendWait("^+c"); Start-Sleep -Milliseconds 500
        $clipText = & powershell -STA -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::GetText()"
        Check ($clipText -match "pic\d\.png") "preview: Ctrl+Shift+C copies the path inside the archive ($clipText)"
        [System.Windows.Forms.SendKeys]::SendWait("^c"); Start-Sleep -Milliseconds 1200
        $clipImg = & powershell -STA -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::ContainsImage()"
        Check ($clipImg -eq "True") "preview: Ctrl+C copies the image to the clipboard"
        # The menu at the top right and the picture information
        $mb = Find $pw "選單"
        Check ($null -ne $mb) "preview: the toolbar has a menu button"
        if ($mb) {
            try {
                $mb.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 900
                $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
                $menuItem = $null
                foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { $m = $w.FindFirst($TS::Descendants, (ByName "圖片資訊")); if ($m) { $menuItem = $m } }
                Check ($null -ne $menuItem) "preview: the menu lists 'image info' (and the other actions)"
                [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600
            } catch { Check $false "preview: the menu opens ($_)" }
        }
        Check ($null -ne (Preview-Window)) "preview: closing the menu with Esc keeps the window"
        [System.Windows.Forms.SendKeys]::SendWait("i")
        Check (Has-Text $pw "壓縮檔內路徑" 8) "preview: I opens the image info panel"
        Check (Has-Text $pw "*像素*" 8) "preview: the info panel shows the pixel size from the decoder"
        Check (Has-Text $pw "PNG" 8) "preview: the info panel shows the image format"
        [System.Windows.Forms.SendKeys]::SendWait("i"); Start-Sleep -Milliseconds 600
        Check ($null -eq ((All-Elements $pw) | Where-Object { $_.Current.Name -eq "壓縮檔內路徑" } | Select-Object -First 1)) "preview: I closes the image info panel"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
    }

    # Double click a picture file: the default is to open it in the picture preview
    Double-Click (Row-Text $win "pic2.png")
    $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
    Check ($null -ne $pw) "preview: double-clicking an image opens the preview window by default"
    if ($pw) {
    Start-Sleep -Milliseconds 700
    Check ([Win.F]::GetForegroundWindow() -eq [IntPtr]$pw.Current.NativeWindowHandle) "preview: a preview opened by double-click is in front of the main window"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
    }

    # New options and default values of the settings page
    try { (Find $win "檢視").SetFocus() } catch { }
    [System.Windows.Forms.SendKeys]::SendWait("^,")
    Wait-Find $win "設定分頁" 8 | Out-Null; Open-SettingsTab $win "檢視"
    Check ($null -ne (Wait-Find $win "預讀" 8)) "settings: the image preview section exists"
    $c1 = Find-Combo $win "按兩下圖片檔時"; $c2 = Find-Combo $win "預讀"
    Check (($null -ne $c1) -and ((Combo-Selected $c1) -eq "在圖形預覽開啟")) "settings: double-clicking an image opens the preview by default"
    Check (($null -ne $c2) -and ((Combo-Selected $c2) -like "256 MB*")) "settings: prefetch defaults to 256 MB"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 500
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # .cb7 (7z for comics) must be opened as an archive, not become "add to archive"
    # (with the reading mode setting off: on, a comic book archive opens in the picture preview, see the section 'reading')
    Push-Location "$pv\src"; 7z a -t7z "$pv\comic.cb7" ".\*" | Out-Null; Pop-Location
    Set-Content $settingsFile "welcome_done=true`ncomic_reading_mode=false" -Encoding UTF8
    $p = Start-App "$pv\comic.cb7"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "解壓縮到此" 10)) "a .cb7 file opens as an archive (not the compress page)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # Self-extracting archive (.exe): every time ask "open the content" or "add to archive"; an ordinary program .exe is simply a file to compress
    $sx = Join-Path $t "sfxtest"; New-Item -ItemType Directory "$sx\src" | Out-Null; Set-Content "$sx\src\a.txt" "hello"
    Push-Location $sx; 7z a -t7z "$sx\t.7z" ".\src" | Out-Null; Pop-Location
    $stub = [IO.File]::ReadAllBytes((Join-Path $root "third_party\7zip-bin\7z.exe"))
    [IO.File]::WriteAllBytes("$sx\plain.exe", $stub)
    [IO.File]::WriteAllBytes("$sx\sfx.exe", $stub + [IO.File]::ReadAllBytes("$sx\t.7z"))
    $p = Start-App "$sx\sfx.exe"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "這是自解壓縮檔嗎？" 12)) "sfx: an .exe containing a 7z archive asks what to do"
    if (Find $win "開啟內容") {
        Click (Find $win "開啟內容")
        Check ($null -ne (Wait-Find $win "解壓縮到此" 10)) "sfx: 'open contents' lists the archive without running the program"
    }
    $p = Start-App "$sx\plain.exe"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "新增壓縮檔" 12)) "sfx: an ordinary program .exe goes straight to the compress page"
    Check ($null -eq (Find $win "這是自解壓縮檔嗎？")) "sfx: an ordinary program .exe does not show the question"
    }

    if (Want "flicker") {
        Reset-State
        Add-Content -Path $settingsFile -Value "animations=false" -Encoding ascii   # the fade of the next picture is meant to show in-between frames; this looks for unintended ones
    # ===== 11. Preview: no empty / flickering frame when switching pictures (sample the colour at the center of the screen continuously) =====
    $fl = Join-Path $t "flicker"; New-Item -ItemType Directory "$fl\src" | Out-Null
    foreach ($cc in @(@("a_red", [System.Drawing.Color]::Red), @("b_green", [System.Drawing.Color]::Lime), @("c_red", [System.Drawing.Color]::Red), @("d_green", [System.Drawing.Color]::Lime))) {
        $bmp = New-Object System.Drawing.Bitmap 900, 600; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear($cc[1]); $g.Dispose()
        $bmp.Save("$fl\src\$($cc[0]).png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Push-Location "$fl\src"; 7z a -tzip "$fl\img.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$fl\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $r0 = Find-Row $win "a_red.png"
    if ($r0) {
        $r0.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        function Find-Preview { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -like "*.png*") { return $w } }; return $null }
        $fp = $null; $end = (Get-Date).AddSeconds(12); do { $fp = Find-Preview; if (-not $fp) { Start-Sleep -Milliseconds 300 } } while (-not $fp -and (Get-Date) -lt $end)
        if ($fp) {
            Start-Sleep -Milliseconds 3000   # let the read-ahead read the pictures before and after first
            $rc = $fp.Current.BoundingRectangle
            $cx = [int]($rc.X + $rc.Width * 0.58); $cy = [int]($rc.Y + $rc.Height * 0.5)
            $bmp1 = New-Object System.Drawing.Bitmap 1, 1; $g1 = [System.Drawing.Graphics]::FromImage($bmp1)
            $bad = 0; $n = 0
            for ($k = 0; $k -lt 8; $k++) {
                [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
                $t0 = Get-Date
                while (((Get-Date) - $t0).TotalMilliseconds -lt 350) {
                    $g1.CopyFromScreen($cx, $cy, 0, 0, $bmp1.Size); $px = $bmp1.GetPixel(0, 0); $n++
                    $isRed = ($px.R -gt 200 -and $px.G -lt 60 -and $px.B -lt 60); $isGreen = ($px.G -gt 200 -and $px.R -lt 60 -and $px.B -lt 60)
                    if (-not ($isRed -or $isGreen)) { $bad++ }
                }
            }
            $g1.Dispose(); $bmp1.Dispose()
            Check ($n -gt 100) "preview flicker test: enough screen samples were taken ($n)"
            Check ($bad -eq 0) "preview: switching images never shows a blank or half-faded frame ($bad bad of $n samples)"
        } else { Check $false "preview flicker test: the preview window opened" }
    }
    }

    if (Want "previewmeta") {
        Reset-State
    # ===== 12. Preview: EXIF orientation is turned upright automatically, AI prompt information, a large picture switching to a small one is not squeezed small first =====
    $ex = Join-Path $t "exif"; New-Item -ItemType Directory "$ex\src" | Out-Null
    $py = @'
import sys, json
from PIL import Image, PngImagePlugin
d = sys.argv[1]
im = Image.new('RGB', (800, 400), (255, 0, 0)); im.paste((0, 0, 255), (400, 0, 800, 400))
e = Image.Exif(); e[0x0112] = 6; im.save(d + '/a_orient6.jpg', exif=e, quality=95)
m = PngImagePlugin.PngInfo(); m.add_text('parameters', 'a red fox, forest\nNegative prompt: blurry, lowres\nSteps: 30, Sampler: Euler a, CFG scale: 7, Seed: 12345, Size: 64x64, Model: testmodel')
Image.new('RGB', (64, 64), (0, 128, 0)).save(d + '/b_a1111.png', pnginfo=m)
prompt = {"3": {"class_type": "KSampler", "inputs": {"seed": 777, "steps": 25, "cfg": 6.5, "sampler_name": "euler", "scheduler": "normal", "denoise": 1, "positive": ["6", 0], "negative": ["7", 0]}}, "6": {"class_type": "CLIPTextEncode", "inputs": {"text": "a blue cat"}}, "7": {"class_type": "CLIPTextEncode", "inputs": {"text": "ugly"}}}
m2 = PngImagePlugin.PngInfo(); m2.add_text('prompt', json.dumps(prompt))
Image.new('RGB', (64, 64), (128, 0, 128)).save(d + '/c_comfy.png', pnginfo=m2)
'@
    $py | python - "$ex\src"
    Push-Location "$ex\src"; 7z a -tzip "$ex\img.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$ex\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $rx = Find-Row $win "a_orient6.jpg"
    if ($rx) {
        $rx.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        function Find-Prev2 { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -match "\.(png|jpg)( |$)") { return $w } }; return $null }
        $pw2 = $null; $end = (Get-Date).AddSeconds(12); do { $pw2 = Find-Prev2; if (-not $pw2) { Start-Sleep -Milliseconds 300 } } while (-not $pw2 -and (Get-Date) -lt $end)
        if ($pw2) {
            Start-Sleep -Milliseconds 2500
            $rc2 = $pw2.Current.BoundingRectangle; $cx2 = [int]($rc2.X + $rc2.Width * 0.58); $cy2 = [int]($rc2.Y + $rc2.Height * 0.5)
            $bm = New-Object System.Drawing.Bitmap 1, 1; $gg = [System.Drawing.Graphics]::FromImage($bm)
            $gg.CopyFromScreen($cx2, $cy2 - 120, 0, 0, $bm.Size); $top = $bm.GetPixel(0, 0)
            $gg.CopyFromScreen($cx2, $cy2 + 120, 0, 0, $bm.Size); $bot = $bm.GetPixel(0, 0)
            Check (($top.R -gt 200 -and $top.B -lt 60) -and ($bot.B -gt 200 -and $bot.R -lt 60)) "preview: EXIF orientation 6 is applied (top red, bottom blue; got top=$($top.R),$($top.G),$($top.B) bottom=$($bot.R),$($bot.G),$($bot.B))"
            # Regression: the status line (and the layout) must use the displayed size of the upright image, not the stored one
            Check (Has-Text $pw2 "*400 × 800*" 5) "preview: EXIF orientation 6 reports the upright size (400 × 800, not the stored 800 × 400)"
            # AI prompt information
            [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 1200
            [System.Windows.Forms.SendKeys]::SendWait("i")
            Check (Has-Text $pw2 "*a red fox, forest*" 10) "preview AI info: the positive prompt of a Stable Diffusion WebUI PNG is shown"
            Check (Has-Text $pw2 "blurry, lowres" 4) "preview AI info: the negative prompt is shown"
            Check (Has-Text $pw2 "12345" 4) "preview AI info: the generation parameters (seed) are shown"
            [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 1500
            Check (Has-Text $pw2 "a blue cat" 10) "preview AI info: a ComfyUI workflow's positive prompt is found"
            Check (Has-Text $pw2 "ComfyUI" 4) "preview AI info: the source is recognized as ComfyUI"
            [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600
            if (Find-Prev2) { [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600 }
        } else { Check $false "preview EXIF test: the preview window opened" }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # Large picture to small picture: the old picture must not be squeezed to the small size first (an old problem: the frame shrank first). Sample the center (A) and a point to the right (B, inside the large picture, outside the small one):
    # the normal order is "A red / B red" -> "A green / B background"; if "A red / B background" appears, the old picture was squeezed small
    $sq = Join-Path $t "squeeze"; New-Item -ItemType Directory "$sq\src" | Out-Null
    foreach ($cc in @(@("a_big", 900, 600, [System.Drawing.Color]::Red), @("b_small", 300, 200, [System.Drawing.Color]::Lime))) {
        $bmp = New-Object System.Drawing.Bitmap $cc[1], $cc[2]; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear($cc[3]); $g.Dispose()
        $bmp.Save("$sq\src\$($cc[0]).png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Push-Location "$sq\src"; 7z a -tzip "$sq\img.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$sq\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $rs = Find-Row $win "a_big.png"
    if ($rs) {
        $rs.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw3 = $null; $end = (Get-Date).AddSeconds(12); do { $pw3 = Find-Prev2; if (-not $pw3) { Start-Sleep -Milliseconds 300 } } while (-not $pw3 -and (Get-Date) -lt $end)
        if ($pw3) {
            Start-Sleep -Milliseconds 3000
            $r3 = $pw3.Current.BoundingRectangle; $ax = [int]($r3.X + $r3.Width * 0.58); $ay = [int]($r3.Y + $r3.Height * 0.5); $bx = $ax + 300
            $bm3 = New-Object System.Drawing.Bitmap 1, 1; $g3 = [System.Drawing.Graphics]::FromImage($bm3)
            $squeezed = 0; $n3 = 0
            for ($k = 0; $k -lt 6; $k++) {
                [System.Windows.Forms.SendKeys]::SendWait($(if ($k % 2 -eq 0) { "{RIGHT}" } else { "{LEFT}" }))
                $t0 = Get-Date
                while (((Get-Date) - $t0).TotalMilliseconds -lt 400) {
                    $g3.CopyFromScreen($ax, $ay, 0, 0, $bm3.Size); $pa = $bm3.GetPixel(0, 0)
                    $g3.CopyFromScreen($bx, $ay, 0, 0, $bm3.Size); $pb = $bm3.GetPixel(0, 0)
                    $g3.CopyFromScreen($ax, $ay, 0, 0, $bm3.Size); $pa2 = $bm3.GetPixel(0, 0)   # sample A once more: if the picture changes between the samples A turns green, which does not count
                    $n3++
                    $aRed = (($pa.R -gt 200 -and $pa.G -lt 60 -and $pa.B -lt 60) -and ($pa2.R -gt 200 -and $pa2.G -lt 60 -and $pa2.B -lt 60)); $bRed = ($pb.R -gt 200 -and $pb.G -lt 60 -and $pb.B -lt 60)
                    if ($k % 2 -eq 0 -and $aRed -and -not $bRed) { $squeezed++ }
                }
            }
            Check ($n3 -gt 30) "preview: big-to-small test took enough samples ($n3)"
            Check ($squeezed -eq 0) "preview: switching from a big image to a small one never shows the old image squeezed small ($squeezed of $n3 samples)"
        } else { Check $false "preview big-to-small test: the preview window opened" }
    }
    }

    if (Want "animated") {
        Reset-State
    # ===== 15. Preview: animations (GIF play / pause; animated WebP the same; APNG is not supported, only the real behaviour is reported) =====
    $an = Join-Path $t "anim"; New-Item -ItemType Directory "$an\src" | Out-Null
    $py = @'
import sys
from PIL import Image
d = sys.argv[1]
fr = [Image.new('RGB', (200, 120), c) for c in ((255, 0, 0), (0, 255, 0), (0, 0, 255))]
fr[0].save(d + '/a.gif', save_all=True, append_images=fr[1:], duration=300, loop=0)
fr[0].save(d + '/b.png', save_all=True, append_images=fr[1:], duration=300, loop=0)
fr[0].save(d + '/c.webp', save_all=True, append_images=fr[1:], duration=300, loop=0, lossless=True)
'@
    $py | python - "$an\src"
    Push-Location "$an\src"; 7z a -tzip "$an\anim.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$an\anim.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    function Distinct-Colors($cx, $cy, $ms) {
        $b = New-Object System.Drawing.Bitmap 1, 1; $g = [System.Drawing.Graphics]::FromImage($b); $set = @{}
        $t0 = Get-Date
        while (((Get-Date) - $t0).TotalMilliseconds -lt $ms) { $g.CopyFromScreen($cx, $cy, 0, 0, $b.Size); $px = $b.GetPixel(0, 0); $set["$($px.R),$($px.G),$($px.B)"] = 1; Start-Sleep -Milliseconds 40 }
        $g.Dispose(); $b.Dispose(); return $set.Count
    }
    $ra = Find-Row $win "a.gif"
    if ($ra) {
        $ra.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        function Find-Anim { param($pat) $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -like $pat) { return $w } }; return $null }
        $fp = $null; $end = (Get-Date).AddSeconds(12); do { $fp = Find-Anim "*.gif*"; if (-not $fp) { Start-Sleep -Milliseconds 300 } } while (-not $fp -and (Get-Date) -lt $end)
        if ($fp) {
            Start-Sleep -Milliseconds 1500
            $rc = $fp.Current.BoundingRectangle; $cx = [int]($rc.X + $rc.Width * 0.58); $cy = [int]($rc.Y + $rc.Height * 0.5)
            Check ((Distinct-Colors $cx $cy 1800) -ge 2) "animated GIF: plays by default (frame colors change)"
            [System.Windows.Forms.SendKeys]::SendWait("p"); Start-Sleep -Milliseconds 400
            Check ((Distinct-Colors $cx $cy 1500) -eq 1) "animated GIF: P pauses (picture stays still)"
            [System.Windows.Forms.SendKeys]::SendWait("p"); Start-Sleep -Milliseconds 300
            Check ((Distinct-Colors $cx $cy 1800) -ge 2) "animated GIF: P again resumes playing"
            [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 1500
            $fq = Find-Anim "*png*"
            if ($fq) { $rc = $fq.Current.BoundingRectangle; Write-Host "  INFO: APNG (not supported, only first frame expected) distinct colors over 1.8 s: $(Distinct-Colors ([int]($rc.X + $rc.Width * 0.58)) ([int]($rc.Y + $rc.Height * 0.5)) 1800)" }
            [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 2500
            $fq = Find-Anim "*webp*"
            if ($fq) {
                $rc = $fq.Current.BoundingRectangle; $cx = [int]($rc.X + $rc.Width * 0.58); $cy = [int]($rc.Y + $rc.Height * 0.5)
                Check ((Distinct-Colors $cx $cy 1800) -ge 2) "animated WebP: plays (frame colors change)"
                [System.Windows.Forms.SendKeys]::SendWait("p"); Start-Sleep -Milliseconds 400
                Check ((Distinct-Colors $cx $cy 1500) -eq 1) "animated WebP: P pauses"
                [System.Windows.Forms.SendKeys]::SendWait("p"); Start-Sleep -Milliseconds 300
                Check ((Distinct-Colors $cx $cy 1800) -ge 2) "animated WebP: P again resumes"
            } else { Check $false "animated WebP: the preview shows c.webp" }
        } else { Check $false "animated GIF: the preview window opened" }
    } else { Check $false "animated GIF: a.gif row found" }
    }

    if (Want "fitmodes") {
        Reset-State
    # ===== 15b. Preview: with "also enlarge small pictures" on, fit window / width / height enlarges small pictures =====
    Set-Content -Path $settingsFile -Value "preview_enlarge_small=true" -Encoding ascii
    $fm = Join-Path $t "fitmodes"; New-Item -ItemType Directory "$fm\src" | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 400, 100; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Orange); $g.Dispose()
    $bmp.Save("$fm\src\pic1.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Push-Location "$fm\src"; 7z a -tzip "$fm\img.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$fm\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $rf = Find-Row $win "pic1.png"
    if ($rf) {
        $rf.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        if ($pw) {
            function Fit-Pct($pat) { $e = (All-Elements $pw) | Where-Object { $_.Current.Name -like $pat } | Select-Object -First 1; if ($e -and $e.Current.Name -match "（(\d+)%）") { return [int]$Matches[1] }; return 0 }
            Start-Sleep -Milliseconds 1200
            $w0 = Fit-Pct "符合視窗（*%）"
            Check ($w0 -gt 100) "fit modes (enlarge on): fit window enlarges a small image ($w0%)"
            [System.Windows.Forms.SendKeys]::SendWait("^2"); Start-Sleep -Milliseconds 600
            $w1 = Fit-Pct "符合寬度（*%）"
            Check ($w1 -gt 100) "fit modes (enlarge on): fit width enlarges a small image ($w1%)"
            [System.Windows.Forms.SendKeys]::SendWait("^3"); Start-Sleep -Milliseconds 600
            $w2 = Fit-Pct "符合高度（*%）"
            Check (($w2 -gt 100) -and ($w2 -ge $w0)) "fit modes (enlarge on): fit height is at least as large as fit window ($w2% vs $w0%)"
            Check ($w1 -le $w2 + 1 -or $w1 -ge $w0) "fit modes: wide image - fit width ($w1%) vs fit height ($w2%) are different modes"
            [System.Windows.Forms.SendKeys]::SendWait("^1"); Start-Sleep -Milliseconds 500
            Check (Has-Text $pw "原圖尺寸（100%）") "fit modes: Ctrl+1 shows the original size (100%)"
            # The preview menu has the same switch ("enlarge small images"): turning it off stops the enlargement and is saved
            $menuBtn = Find $pw "選單"
            Check ($null -ne $menuBtn) "fit modes: the preview has a menu button"
            if ($menuBtn) {
                $menuBtn.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 700
                $pidCond = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
                $mi = $AE::RootElement.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition($pidCond, (ByName "小圖也放大"))))
                Check ($null -ne $mi) "fit modes: the preview menu lists 'enlarge small images'"
                if ($mi) {
                    try { $mi.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle() } catch { try { $mi.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() } catch { } }
                    Start-Sleep -Milliseconds 900
                    [System.Windows.Forms.SendKeys]::SendWait("^0"); Start-Sleep -Milliseconds 700
                    $w3 = Fit-Pct "符合視窗（*%）"
                    Check ($w3 -gt 0 -and $w3 -le 100) "fit modes: the menu switch turns the enlargement off at once (fit window now $w3%)"
                    Check ((Get-Content $settingsFile -Raw) -match "preview_enlarge_small=false") "fit modes: the menu switch is saved to the settings file"
                }
            }
        } else { Check $false "fit modes: the preview window opened" }
    } else { Check $false "fit modes: image row found" }
    }

    if (Want "thumbs") {
        Reset-State
    # ===== 15c. Picture thumbnails of the file list (icon view): shown for non-solid archives; not made for solid archives, and the screen says why =====
    $th = Join-Path $t "thumbs"; New-Item -ItemType Directory "$th\src" | Out-Null
    foreach ($i in 1..3) {
        $bmp = New-Object System.Drawing.Bitmap 200, 150; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::OrangeRed); $g.Dispose()
        $bmp.Save("$th\src\pic$i.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Set-Content "$th\src\note.txt" "x"
    Push-Location "$th\src"; 7z a -tzip "$th\plain.zip" ".\*" | Out-Null; 7z a -t7z -ms=on "$th\solid.7z" ".\*" | Out-Null; Pop-Location
    function Count-Images($w) { @($w.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Image)))).Count }
    # Baseline: thumbnails off
    Set-Content -Path $settingsFile -Value "view_mode=icons`nicon_thumbnails=false`nshell_icons=false" -Encoding ascii
    $p = Start-App "$th\plain.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1500
    $n0 = Count-Images $win
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    # Non-solid: thumbnails on
    Set-Content -Path $settingsFile -Value "view_mode=icons`nicon_thumbnails=true`nshell_icons=false" -Encoding ascii
    $p = Start-App "$th\plain.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 3000
    $n1 = Count-Images $win
    Check ($n1 -ge $n0 + 3) "thumbnails: a non-solid zip shows a thumbnail per image in icon view ($n0 -> $n1 images)"
    Check (-not (Has-Text $win "*不產生縮圖*" 2)) "thumbnails: no 'not generated' message for a non-solid archive"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    # Solid 7z: not made, and it says so
    $p = Start-App "$th\solid.7z"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 2500
    Check (Has-Text $win "*固實壓縮檔，不產生縮圖*") "thumbnails: a solid archive explains why no thumbnails are generated (status area)"
    Check ((Count-Images $win) -le $n0) "thumbnails: a solid archive shows no thumbnails"
    # The options in the View menu must say so too
    $vb = Find $win "檢視"
    if ($vb) {
        $vb.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 600
        Check (Has-Text $win "圖示檢視顯示圖片縮圖（此壓縮檔不支援：固實壓縮）" 4) "thumbnails: the View menu item says this archive is not supported (solid)"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    } else { Check $false "thumbnails: view button found" }
    }

    if (Want "layout") {
        Reset-State
    # ===== 15d. Layout of the main screen: the title bar shows the file name, the archive information is in the status bar at the lower left, the view chosen last is remembered =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
    Check ($win.Current.Name -eq "LycanZip - sample.zip") "layout: the window title shows the archive name ($($win.Current.Name))"
    Check (Has-Text $win "*共 * 個項目*｜*") "layout: the status bar shows the archive info (format, items, size) and the folder item count"
    # The buttons of the first row and the path / search of the second row are all there
    $rb = (Find $win "解壓縮到此").Current.BoundingRectangle; $rs = (Find $win "搜尋").Current.BoundingRectangle; $ru = (Find $win "上一層").Current.BoundingRectangle
    $rv = (Find $win "檢視").Current.BoundingRectangle
    Check (($rv.Y -lt $ru.Y) -and ($rb.Y -lt $rs.Y) -and ([math]::Abs($ru.Y - $rs.Y) -lt 20)) "layout: buttons are on the first row; 'up' and the search box share the second row"
    Check ($rs.X -gt $ru.X + 400) "layout: the search box is at the right end of the path row"
    # After selecting items: the selection bar appears, the archive information is still there
    $rr = Find-Row $win "readme.txt"
    if ($rr) { $rr.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500 }
    Check (($null -ne (Find $win "解壓選取項目到此處")) -and (Has-Text $win "*共 * 個項目*" 2)) "layout: selecting keeps the archive info visible next to the selection bar"
    # Remember the view chosen last: choosing an icon size in the menu writes the view and the size into the settings
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500; Click (Find $win "大圖示"); Start-Sleep -Milliseconds 800
    Check (((Get-Content $settingsFile -Raw) -match "view_mode=icons") -and ((Get-Content $settingsFile -Raw) -match "thumb_size=2")) "layout: choosing Large icons in the menu is remembered (view_mode=icons, thumb_size=2)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
    Check ($null -ne (Find $win "壓縮檔內容（清單 / 圖示）")) "layout: the next archive opens in the remembered view (icons grid)"
    # The settings page has the matching option
    Click (Find $win "設定"); Start-Sleep -Milliseconds 800
    Open-SettingsTab $win "檢視"
    Check (($null -ne (Find $win "記住上次選擇的檢視")) -and ($null -ne (Find $win "圖示檢視顯示圖片縮圖")) -and ($null -eq (Find $win "圖示大小"))) "layout: settings has 'remember view' and thumbnails, and no separate icon size"
    }

    if (Want "ctxmenu") {
        Reset-State
    # ===== 15e. Context menu of an empty part of the file list: view, sort by, select all, list everything =====
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    function Find-Any($n) { $c = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)), (ByName $n), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::MenuItem))); return $AE::RootElement.FindFirst($TS::Descendants, $c) }   # a pop-up menu is not always under the main window
    $lst = Find $win "壓縮檔內容"
    if ($lst) {
        try { $lst.SetFocus() } catch { }; Start-Sleep -Milliseconds 300
        $lr = $lst.Current.BoundingRectangle   # right click on the empty part in the lower half of the list
        [Win.D]::SetCursorPos([int]($lr.X + $lr.Width * 0.6), [int]($lr.Y + $lr.Height * 0.85)) | Out-Null; Start-Sleep -Milliseconds 400
        [Win.D]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 800
        Check (($null -ne (Find-Any "排序方式")) -and ($null -ne (Find-Any "全選"))) "blank-space menu: has 'sort by' and 'select all'"
        $sv = Find-Any "排序方式"
        if ($sv) {
            try { $sv.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand() } catch { Click $sv }; Start-Sleep -Milliseconds 600
            Check ($null -ne (Find-Any "類型")) "blank-space menu: the sort submenu offers 'type'"
        }
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}{ESC}"); Start-Sleep -Milliseconds 300
    } else { Check $false "blank-space menu: the file list was found" }
    }

    if (Want "columns") {
        Reset-State
    # ===== 15f. Columns of the details view: short type names, column choice (right click on the header), narrow window hides them in order, the splitter of the folder tree can be dragged =====
    $p = Start-App "$vw\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
    function Find-AnyItem($n) { $c = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)), (ByName $n), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::MenuItem))); return $AE::RootElement.FindFirst($TS::Descendants, $c) }
    Check (Has-Text $win "TXT" 3) "columns: the type column shows just the extension (TXT)"
    Check (($null -ne (Find $win "依類型排序")) -and ($null -ne (Find $win "依大小排序")) -and ($null -ne (Find $win "依修改時間排序"))) "columns: type, size and modified are shown by default"
    Check ($null -eq (Find $win "依壓縮後大小排序")) "columns: the packed-size column is hidden by default"
    # Right click on a column header -> tick "packed"
    $hd = Find $win "依名稱排序"; $hr = $hd.Current.BoundingRectangle
    [Win.D]::SetCursorPos([int]($hr.X + $hr.Width + 300), [int]($hr.Y + $hr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 400
    [Win.D]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 800
    $mi = Find-AnyItem "壓縮後"
    Check ($null -ne $mi) "columns: right-clicking the header offers a column chooser"
    if ($mi) {
        try { $mi.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle() } catch { try { $mi.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() } catch { } }
        Start-Sleep -Milliseconds 800
        Check ($null -ne (Find $win "依壓縮後大小排序")) "columns: choosing 'packed' shows the column"
        Check ((Get-Content $settingsFile -Raw) -match "list_columns=15") "columns: the choice is saved (list_columns=15)"
    }
    # Splitter of the folder tree: drag 100 pixels to the right, the tree gets wider
    $tr0 = (Find $win "資料夾樹").Current.BoundingRectangle
    $sp = Find $win "資料夾樹分隔線"
    if ($sp) {
        $sr = $sp.Current.BoundingRectangle; $sx = [int]($sr.X + $sr.Width / 2); $sy = [int]($sr.Y + $sr.Height / 2)
        $scrC = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        function MoveAbsC($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scrC.Width), [uint32]($y * 65535 / $scrC.Height), 0, [UIntPtr]::Zero) }
        MoveAbsC $sx $sy; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        foreach ($dx in 20, 50, 80, 110) { MoveAbsC ($sx + $dx) $sy; Start-Sleep -Milliseconds 80 }
        [Win.D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 600
        $tr1 = (Find $win "資料夾樹").Current.BoundingRectangle
        Check ($tr1.Width -gt $tr0.Width + 60) "columns: dragging the splitter widens the folder tree ($([int]$tr0.Width) -> $([int]$tr1.Width))"
        Check ((Get-Content $settingsFile -Raw) -match "tree_width=\d+") "columns: the tree width is saved"
    } else { Check $false "columns: the tree splitter exists" }
    }

    if (Want "iconview") {
        Reset-State
    # ===== 15g. Icon view: long file names shown in full (not cut in half), gaps between the items allow band selection =====
    $ic = Join-Path $t "iconview"; New-Item -ItemType Directory "$ic\src" | Out-Null
    foreach ($n in '!$Gate1.png_', '!$GamePara_0.5_0.png_', 'a very long file name with spaces that wraps around.txt', 'short.txt', 'readme.md', 'data.json') { Set-Content -LiteralPath "$ic\src\$n" "x" }
    Push-Location "$ic\src"; 7z a -tzip "$ic\icons.zip" ".\*" | Out-Null; Pop-Location
    Set-Content -Path $settingsFile -Value "view_mode=icons" -Encoding ascii
    $p = Start-App "$ic\icons.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $items = @($win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem))))
    Check ($items.Count -eq 6) "icon view: all items are listed ($($items.Count))"
    # All items have the same size (fixed height, names are not cut off), and there are gaps between them
    $hs = @($items | ForEach-Object { [int]$_.Current.BoundingRectangle.Height } | Select-Object -Unique)
    Check ($hs.Count -eq 1) "icon view: every item has the same height ($($hs -join ','))"
    $r0 = $items[0].Current.BoundingRectangle; $r1 = $items[1].Current.BoundingRectangle
    $gap = $r1.X - ($r0.X + $r0.Width)
    Check ($gap -ge 4) "icon view: there is a gap between neighbouring items ($([int]$gap) px)"
    # Press in a gap and drag to the lower right over a few items = band selection
    $scrI = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    function MoveAbsI($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scrI.Width), [uint32]($y * 65535 / $scrI.Height), 0, [UIntPtr]::Zero) }
    $sx = [int]($r0.X + $r0.Width + $gap / 2); $sy = [int]($r0.Y + $r0.Height / 2)
    $r3 = $items[3].Current.BoundingRectangle
    MoveAbsI $sx $sy; Start-Sleep -Milliseconds 300
    [Win.D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
    foreach ($f in 0.25, 0.5, 0.75, 1.0) { MoveAbsI ([int]($sx + ($r3.X + $r3.Width / 2 - $sx) * $f)) ([int]($sy + 20 * $f)); Start-Sleep -Milliseconds 120 }
    [Win.D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 700
    Check (Has-Text $win "已選 *" 3) "icon view: dragging from the gap between items selects the items it touches"
    }

    if (Want "nodecoder") {
        Reset-State
    # ===== 13. Picture formats that Windows has no decoder for (for example JXL): opened with the associated program, and the reason is shown below =====
    # A development machine usually has the decoders, so an environment variable makes .jxl count as "no decoder", and the associated program is not really started
    $env:LYCAN_TEST_NO_DECODER = ".jxl"; $env:LYCAN_TEST_NO_OPEN = "1"
    $jx = Join-Path $t "jxl"; New-Item -ItemType Directory "$jx\src" | Out-Null; [IO.File]::WriteAllBytes("$jx\src\x.jxl", [byte[]](1..200))
    Push-Location "$jx\src"; 7z a -tzip "$jx\img.zip" ".\x.jxl" | Out-Null; Pop-Location
    $p = Start-App "$jx\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $rj = Find-Row $win "x.jxl"
    if ($rj) {
        $rj.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        Check (Has-Text $win "*JPEG XL 影像擴充功能*" 8) "no decoder: Alt+P explains which Windows extension is needed"
        Check ($null -eq (Preview-Window)) "no decoder: no preview window is opened for an unsupported format"
        $rj.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        Double-Click (Row-Text $win "x.jxl")
        Check (Has-Text $win "*已改用關聯程式開啟*" 8) "no decoder: double-click opens with the associated program and says so in the status area"
        Check ($null -eq (Preview-Window)) "no decoder: double-click does not open the preview window"
        $tmpHit = Get-ChildItem (Join-Path $env:TEMP "LycanZip\open") -Recurse -Filter x.jxl -ErrorAction SilentlyContinue | Select-Object -First 1
        Check ($null -ne $tmpHit) "no decoder: the file was extracted for the associated program"
    }
    Remove-Item Env:\LYCAN_TEST_NO_DECODER, Env:\LYCAN_TEST_NO_OPEN -ErrorAction SilentlyContinue
    }

    if (Want "descend") {
        Reset-State
    # ===== 14. On open, a single folder is entered automatically (at most three times) =====
    $dd = Join-Path $t "descend"; New-Item -ItemType Directory "$dd\a\b\c", "$dd\w\x\y\z" | Out-Null
    Set-Content "$dd\a\b\c\x.txt" "x"; Set-Content "$dd\a\b\c\y.txt" "y"; Set-Content "$dd\w\x\y\z\f.txt" "f"
    Push-Location $dd; 7z a -tzip "$dd\three.zip" ".\a" | Out-Null; 7z a -tzip "$dd\four.zip" ".\w" | Out-Null; Pop-Location
    $p = Start-App "$dd\three.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    Check ($null -ne (Find-Row $win "x.txt")) "auto-descend: a single-folder chain of three levels opens at the innermost folder (files visible)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p = Start-App "$dd\four.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    Check (($null -ne (Find-Row $win "z")) -and ($null -eq (Find-Row $win "f.txt"))) "auto-descend: stops after three levels (shows folder 'z', not the file inside it)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "encoding") {
        Reset-State
    # ===== 15. After switching the file name encoding, go back to the folder where we were =====
    # Write a zip with Python whose names are Big5 bytes (no UTF-8 flag): docs/sub/ has two Chinese file names, and the root has one more file (to avoid the automatic descent)
    $ed = Join-Path $t "encname"; New-Item -ItemType Directory $ed | Out-Null
    $py = Join-Path $ed "mk.py"
    Set-Content $py -Encoding UTF8 -Value @'
import sys, zipfile
class Raw(zipfile.ZipInfo):
    def _encodeFilenameFlags(self):
        return self.raw, 0
z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED)
for name in ["docs/sub/測試.txt", "docs/sub/二.txt", "其他.txt"]:
    zi = Raw(name)
    zi.raw = name.encode("cp950")
    z.writestr(zi, "x")
z.close()
'@
    python $py "$ed\big5.zip"
    $p = Start-App "$ed\big5.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
    Check ($null -ne (Find-Row $win "docs")) "encoding: the archive root lists the 'docs' folder"
    Double-Click (Row-Text $win "docs"); Start-Sleep -Milliseconds 600
    Double-Click (Row-Text $win "sub"); Start-Sleep -Milliseconds 600
    Check (($null -eq (Find-Row $win "docs")) -and ($null -eq (Find-Row $win "sub"))) "encoding: navigated into docs/sub"
    Select-Combo $win "檔名編碼" "GBK (簡體中文)"
    Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1500
    $rows = $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))
    Check ($rows.Count -ge 2) "encoding: after switching encoding the list still shows the two files ($($rows.Count) rows)"
    Check (($null -eq (Find-Row $win "docs")) -and ($null -eq (Find-Row $win "sub"))) "encoding: switching the encoding keeps the current folder (still inside docs/sub)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "i18n") {
        Reset-State
    # ===== 16. Interface language =====
    # Put a folder named exactly "設定" in the archive: it is data and must not be translated in any language
    $il = Join-Path $t "i18n"; New-Item -ItemType Directory "$il/src/設定" | Out-Null
    Set-Content "$il/src/設定/a.txt" "x"; Set-Content "$il/src/readme.txt" "x"
    Push-Location "$il/src"; 7z a -tzip "$il/l.zip" "./設定" "./readme.txt" | Out-Null; Pop-Location
    try {
        # The environment variable LYCAN_LANG forces the language (for tests)
        $env:LYCAN_LANG = "en-US"
        $p = Start-App "$il/l.zip"; $win = Get-Win $p
        Check ($null -ne (Wait-Find $win "Extract here")) "i18n en-US: the toolbar is in English"
        Check ($null -ne (Find $win "More options")) "i18n en-US: another toolbar button is in English"
        Check ($null -ne (Find-Row $win "設定")) "i18n en-US: a folder named like a UI string keeps its real name"
        Check ($null -eq (Find-Row $win "Settings")) "i18n en-US: the data was not translated (no 'Settings' row)"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

        $env:LYCAN_LANG = "zh-CN"
        $p = Start-App "$il/l.zip"; $win = Get-Win $p
        Check ($null -ne (Wait-Find $win "解压到此")) "i18n zh-CN: the toolbar is in Simplified Chinese"
        Check ($null -ne (Find-Row $win "設定")) "i18n zh-CN: the folder keeps its real (Traditional) name"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

        # the languages added in 1.1: one text each is enough here (all texts are checked by tools\i18n.py --check)
        foreach ($case in @(@("ja-JP", "ここに展開"), @("ko-KR", "여기에 압축 풀기"), @("de-DE", "Hier extrahieren"), @("es-ES", "Extraer aquí"), @("fr-FR", "Extraire ici"))) {
            $env:LYCAN_LANG = $case[0]
            $p = Start-App "$il/l.zip"; $win = Get-Win $p
            Check ($null -ne (Wait-Find $win $case[1])) ("i18n {0}: the toolbar is in that language ('{1}')" -f $case[0], $case[1])
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    } finally { $env:LYCAN_LANG = "zh-TW" }

    # Choose the language on the settings page -> written to the settings file, a restart notice; takes effect after the restart (the first start always uses zh-TW; at the restart the environment variable is not set, so the settings file decides)
    Reset-State
    $p = Start-App "$il/l.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "設定"); Start-Sleep -Milliseconds 800
    Select-Combo $win "語言" "English"
    Check ($null -ne (Wait-Find $win "已儲存，重新啟動後生效。" 3)) "i18n: choosing a language shows the 'restart to apply' note"
    Check ((Get-Content $settingsFile -Raw) -match "language=en-US") "i18n: the language choice is saved (language=en-US)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_LANG -ErrorAction SilentlyContinue
    try {
        $p = Start-App "$il/l.zip"; $win = Get-Win $p
        Check ($null -ne (Wait-Find $win "Extract here")) "i18n: after a restart the saved language is used"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    } finally { $env:LYCAN_LANG = "zh-TW" }
    }

    if (Want "multiwin") {
        Reset-State
    # ===== 17. Several windows: one process, one window for each archive =====
    $mw = Join-Path $t "multiwin"; New-Item -ItemType Directory $mw | Out-Null
    foreach ($i in 1..10) { New-Item -ItemType Directory "$mw/s$i" | Out-Null; Set-Content "$mw/s$i/f.txt" "x$i"; Push-Location "$mw/s$i"; 7z a -tzip "$mw/a$i.zip" "./f.txt" | Out-Null; Pop-Location }
    function MW-Wins($procId) { @($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)))) | Where-Object { $_.Current.Name -ne "" } }
    function MW-Wait-Count($procId, [int]$n, [int]$sec = 20) {
        $end = (Get-Date).AddSeconds($sec)
        do { if ((MW-Wins $procId).Count -ge $n) { return $true }; Start-Sleep -Milliseconds 200 } while ((Get-Date) -lt $end)
        return $false
    }
    Add-Type -Namespace U2 -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);' -ErrorAction SilentlyContinue
    function MW-Activate($w) { [U2.W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [U2.W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [U2.W]::SetForegroundWindow([IntPtr]$w.Current.NativeWindowHandle) | Out-Null }

    # Open three archives in a row: one process, three windows
    $p = Start-Process $exe -ArgumentList (Q "$mw/a1.zip") -PassThru; Check (MW-Wait-Count $p.Id 1) "multiwin: the first archive opens a window"
    foreach ($i in 2..3) { Start-Process $exe -ArgumentList (Q "$mw/a$i.zip"); Start-Sleep -Milliseconds 1500 }
    Check (MW-Wait-Count $p.Id 3) "multiwin: opening more archives opens more windows (3)"
    Check (@(Get-Process LycanZip -ErrorAction SilentlyContinue).Count -eq 1) "multiwin: still a single process"
    Check ((MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a2.zip*" }).Count -eq 1) "multiwin: each archive has its own window (a2.zip)"

    # Open the same archive again (here with a path written in different case on purpose): no second window, the one that is open comes to the front
    Start-Process $exe -ArgumentList (Q "$mw/A2.ZIP"); Start-Sleep -Seconds 2.5
    Check ((MW-Wins $p.Id).Count -eq 3) "multiwin: opening an archive that is already open does not open another window (still 3)"
    Check ((MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a2.zip*" }).Count -eq 1) "multiwin: still exactly one window shows a2.zip"

    # Open 7 at once (simulates an Explorer multi-select): more than 5 asks, choose "open only the first 5"
    foreach ($i in 4..10) { Start-Process $exe -ArgumentList (Q "$mw/a$i.zip") }
    function MW-Find-Any($procId, $name, [int]$sec = 10) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (MW-Wins $procId)) { $hit = Find $w $name; if ($hit) { return $hit } }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $end)
        return $null
    }
    $ask = MW-Find-Any $p.Id "只開啟前 5 個"
    Check ($null -ne $ask) "multiwin: a batch of 7 asks how many to open (button 'only the first 5')"
    Check ((MW-Wins $p.Id).Count -eq 3) "multiwin: nothing is opened while the question is open (still 3)"
    if ($ask) { Click $ask }
    Check (MW-Wait-Count $p.Id 8) "multiwin: choosing 'only the first 5' opens 5 more windows (8 in total)"
    Start-Sleep -Seconds 2
    Check ((MW-Wins $p.Id).Count -eq 8) "multiwin: the rest of the batch is not opened (still 8)"

    # Another batch of 6: choose "open all"
    foreach ($i in 1..6) { New-Item -ItemType Directory "$mw/t$i" | Out-Null; Set-Content "$mw/t$i/f.txt" "t$i"; Push-Location "$mw/t$i"; 7z a -tzip "$mw/b$i.zip" "./f.txt" | Out-Null; Pop-Location }
    foreach ($i in 1..6) { Start-Process $exe -ArgumentList (Q "$mw/b$i.zip") }
    $askAll = MW-Find-Any $p.Id "全部開啟（6 個）"
    Check ($null -ne $askAll) "multiwin: a batch of 6 asks again (button 'open all (6)')"
    if ($askAll) { Click $askAll }
    Check (MW-Wait-Count $p.Id 14) "multiwin: choosing 'open all' opens all 6 (14 in total)"

    # After all are closed the process ends
    $guard = 0
    while ($guard++ -lt 30) {
        $w = MW-Wins $p.Id | Select-Object -First 1
        if (-not $w) { break }
        try { $w.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { break }
        Start-Sleep -Milliseconds 500
    }
    Check ($p.WaitForExit(10000)) "multiwin: the process exits after the last window is closed"
    Reset-State

    # An empty start page window is reused
    $p = Start-Process $exe -PassThru; Check (MW-Wait-Count $p.Id 1) "multiwin: a home-page window opens"
    Start-Sleep -Seconds 1
    Start-Process $exe -ArgumentList (Q "$mw/a1.zip"); Start-Sleep -Seconds 3
    Check ((MW-Wins $p.Id).Count -eq 1) "multiwin: an empty home window is reused for the next archive (1 window)"
    Check ((MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a1.zip*" }).Count -eq 1) "multiwin: the reused window now shows the archive"

    # Settings sync: another window saved the settings file, this window applies it when it is activated
    Start-Process $exe -ArgumentList (Q "$mw/a2.zip"); Start-Sleep -Seconds 2.5
    Check (MW-Wait-Count $p.Id 2) "multiwin: a second window for the settings-sync test"
    $w1 = MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a1.zip*" } | Select-Object -First 1
    $w2 = MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a2.zip*" } | Select-Object -First 1
    MW-Activate $w2; Start-Sleep -Seconds 1
    Click (Find $w2 "設定"); Start-Sleep -Milliseconds 1500
    $c2 = Find-Combo $w2 "主題"
    Check (($null -ne $c2) -and ((Combo-Selected $c2) -eq "跟隨系統")) "multiwin: window 2 shows the settings page (theme = follow system)"
    Set-Content $settingsFile "theme=dark`n" -Encoding ASCII
    MW-Activate $w1; Start-Sleep -Seconds 1
    MW-Activate $w2; Start-Sleep -Milliseconds 1500
    Check ((Combo-Selected (Find-Combo $w2 "主題")) -eq "深色") "multiwin: settings written by another window are applied when this window is activated"

    # Immediate sync in the same process (no need to activate the window) + a window that saves later does not overwrite the one that saved earlier
    MW-Activate $w1; Start-Sleep -Seconds 1
    Click (Find $w1 "設定"); Start-Sleep -Milliseconds 1500
    Select-Combo $w1 "主題" "淺色"; Start-Sleep -Milliseconds 800
    Check ((Combo-Selected (Find-Combo $w2 "主題")) -eq "淺色") "multiwin: a setting changed in one window shows up in the other at once (no activation needed)"
    $animCond = New-Object Windows.Automation.AndCondition((ByName "動畫"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    $animTog = $w2.FindFirst($TS::Descendants, $animCond)
    $animTog.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 800
    $iniNow = Get-Content $settingsFile -Raw
    Check (($iniNow -match "theme=light") -and ($iniNow -match "animations=false")) "multiwin: the second window's change did not overwrite the first window's (theme=light and animations=false are both saved; note: UIA focus activates the window, so this check alone cannot tell the hub from reload-on-activation)"
    # Open mode = replace the current window: the second archive replaces the content of the first window, no new window
    Reset-State
    Set-Content $settingsFile "open_archive_in=replace`n" -Encoding ASCII
    $p = Start-Process $exe -ArgumentList (Q "$mw/a1.zip") -PassThru; Check (MW-Wait-Count $p.Id 1) "multiwin: replace policy: the first archive opens"
    Start-Sleep -Seconds 2
    Start-Process $exe -ArgumentList (Q "$mw/a2.zip"); Start-Sleep -Seconds 3
    Check ((MW-Wins $p.Id).Count -eq 1) "multiwin: replace policy: a second archive does not open another window (1 window)"
    Check ((MW-Wins $p.Id | Where-Object { $_.Current.Name -like "*a2.zip*" }).Count -eq 1) "multiwin: replace policy: the window now shows the second archive"
    Reset-State
    }

    if (Want "extractmodes") {
        Reset-State
    # ===== 21. "Extract here": automatic / no folder / folder, main button + drop-down follow the setting =====
    $em = Join-Path $t "emodes"; New-Item -ItemType Directory $em | Out-Null
    New-Item -ItemType Directory "$em\srcone\root" | Out-Null; "one" | Set-Content "$em\srcone\root\x.txt"
    Push-Location "$em\srcone"; 7z a -tzip "$em\single.zip" ".\root" | Out-Null; Pop-Location
    function EM-Dir($name, $zip) { $d = Join-Path $em $name; New-Item -ItemType Directory $d | Out-Null; Copy-Item $zip (Join-Path $d "sample.zip") -ErrorAction SilentlyContinue; $d }
    function EM-Menu($w, $btn, $item) {
        $b = Find $w $btn
        if (-not $b) { return $null }
        $b.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 700
        $pidCond = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
        return $AE::RootElement.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition($pidCond, (ByName $item))))
    }
    function EM-Run($label, $zipName, $dir) {
        # opens $dir\sample.zip and returns the window
        $script:p = Start-App "$dir\sample.zip"; $script:win = Get-Win $p
        return $win
    }

    # 1. default = automatic: a multi-item archive gets a folder named after the archive; button text is plain
    $d1 = EM-Dir "d1" "$t\sample.zip"
    $win = EM-Run "d1" "sample.zip" $d1
    Check ($null -ne (Wait-Find $win "解壓縮到此")) "extract modes: the default button is 'Extract here'"
    $mi = EM-Menu $win "解壓縮到此" "解壓縮到此（不建資料夾）"
    Check ($null -ne $mi) "extract modes: the drop-down offers 'no folder' (automatic is the main button)"
    $mf = $null
    if ($mi) { $pidCond = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); $mf = $AE::RootElement.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition($pidCond, (ByName "解壓縮到「sample」資料夾")))) }
    Check ($null -ne $mf) "extract modes: the drop-down offers 'folder' with the real folder name (sample)"
    # close the menu, run the main button
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    Click (Find $win "解壓縮到此")
    $r = Wait-Task $win "^完成　"; Check ($null -ne $r) "extract modes: automatic extraction completes"
    Check (Test-Path "$d1\sample\readme.txt") "extract modes: automatic wraps a multi-item archive in sample\"

    # 2. drop-down: no folder -> straight into the archive's folder
    Reset-State
    $d2 = EM-Dir "d2" "$t\sample.zip"
    $win = EM-Run "d2" "sample.zip" $d2
    Wait-Find $win "解壓縮到此" | Out-Null
    $mi = EM-Menu $win "解壓縮到此" "解壓縮到此（不建資料夾）"
    if ($mi) { try { $mi.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() } catch { } }
    $r = Wait-Task $win "^完成　"; Check ($null -ne $r) "extract modes: 'no folder' extraction completes"
    Check ((Test-Path "$d2\readme.txt") -and -not (Test-Path "$d2\sample")) "extract modes: 'no folder' puts the items next to the archive"

    # 3. drop-down: folder -> always a folder, even for an archive that has a single root folder
    Reset-State
    $d3 = Join-Path $em "d3"; New-Item -ItemType Directory $d3 | Out-Null; Copy-Item "$em\single.zip" "$d3\sample.zip"
    $win = EM-Run "d3" "sample.zip" $d3
    Wait-Find $win "解壓縮到此" | Out-Null
    $mi = EM-Menu $win "解壓縮到此" "解壓縮到「sample」資料夾"
    if ($mi) { try { $mi.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() } catch { } }
    $r = Wait-Task $win "^完成　"; Check ($null -ne $r) "extract modes: 'folder' extraction completes"
    Check (Test-Path "$d3\sample\root\x.txt") "extract modes: 'folder' wraps even a single-root archive (sample\root\x.txt)"
    # ... while automatic would not wrap it
    Reset-State
    $d3b = Join-Path $em "d3b"; New-Item -ItemType Directory $d3b | Out-Null; Copy-Item "$em\single.zip" "$d3b\sample.zip"
    $win = EM-Run "d3b" "sample.zip" $d3b
    Wait-Find $win "解壓縮到此" | Out-Null; Click (Find $win "解壓縮到此")
    $r = Wait-Task $win "^完成　"
    Check ((Test-Path "$d3b\root\x.txt") -and -not (Test-Path "$d3b\sample")) "extract modes: automatic does not wrap a single-root archive (root\x.txt)"

    # 4. setting = no folder: the main button and the drop-down change with it
    Reset-State
    Set-Content -Path $settingsFile -Value "extract_mode=no_folder" -Encoding ascii
    $d4 = EM-Dir "d4" "$t\sample.zip"
    $win = EM-Run "d4" "sample.zip" $d4
    Check ($null -ne (Wait-Find $win "解壓縮到此（不建資料夾）")) "extract modes: with the setting 'no folder' the main button says so"
    $mi = EM-Menu $win "解壓縮到此（不建資料夾）" "解壓縮到此（自動判斷）"
    Check ($null -ne $mi) "extract modes: the drop-down then offers 'automatic'"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    Click (Find $win "解壓縮到此（不建資料夾）")
    $r = Wait-Task $win "^完成　"
    Check ((Test-Path "$d4\readme.txt") -and -not (Test-Path "$d4\sample")) "extract modes: the main button follows the 'no folder' setting"

    # 5. setting = folder
    Reset-State
    Set-Content -Path $settingsFile -Value "extract_mode=folder" -Encoding ascii
    $d5 = EM-Dir "d5" "$t\sample.zip"
    $win = EM-Run "d5" "sample.zip" $d5
    Check ($null -ne (Wait-Find $win "解壓縮到「sample」資料夾")) "extract modes: with the setting 'folder' the main button names the folder"
    Click (Find $win "解壓縮到「sample」資料夾")
    $r = Wait-Task $win "^完成　"
    Check (Test-Path "$d5\sample\readme.txt") "extract modes: the main button follows the 'folder' setting"
    Reset-State
    }

    if (Want "compactopts") {
        Reset-State
    # ===== 22. Compact window (context-menu actions): the "after completion" row =====
    $co = Join-Path $t "compactopts"; New-Item -ItemType Directory $co | Out-Null
    function CO-Box($w, $name) { $c = New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::CheckBox))); $w.FindFirst($TS::Descendants, $c) }
    function CO-Toggle($box) { $box.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 500 }
    function CO-State($box) { $box.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState }

    # extraction: the row is there, the close option can keep the window open and be switched on again
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    Copy-Item "$t\sample.zip" "$co\a.zip"
    $p = Start-Raw @("--extract-here", (Q "$co\a.zip")); $win = Get-Win $p
    $bOpen = $null; $end = (Get-Date).AddSeconds(8); do { $bOpen = CO-Box $win "完成後開啟資料夾"; if (-not $bOpen) { Start-Sleep -Milliseconds 300 } } while (-not $bOpen -and (Get-Date) -lt $end)
    Check ($null -ne $bOpen) "compact options: 'open folder' check box is shown for an extraction"
    Check ($null -eq (CO-Box $win "完成後刪除原壓縮檔")) "compact options: there is no 'delete the archive' check box (it is a button on the finished card)"
    $bClose = CO-Box $win "完成後關閉視窗"
    Check (($null -ne $bClose) -and ((CO-State $bClose) -eq 0)) "compact options: 'close the window' is off when the setting says so"
    Start-Sleep -Seconds 4
    Check ((Test-Path "$co\a\readme.txt")) "compact options: the extraction ran"
    Check (-not $p.HasExited) "compact options: the window stays open after success when 'close the window' is off"
    $win = Get-Win $p; $bClose = CO-Box $win "完成後關閉視窗"
    if ($bClose) { CO-Toggle $bClose }
    Check ((Get-Content $settingsFile -Raw) -match "compact_close_when_done=true") "compact options: ticking 'close the window' is saved to the settings"
    Check (Wait-Exit $p 10) "compact options: ticking it closes the finished window"

    # the other boxes are saved too (and are the same settings as the settings page)
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    Copy-Item "$t\sample.zip" "$co\b.zip"
    $p = Start-Raw @("--extract-here", (Q "$co\b.zip")); $win = Get-Win $p
    $bOpen = $null; $end = (Get-Date).AddSeconds(8); do { $bOpen = CO-Box $win "完成後開啟資料夾"; if (-not $bOpen) { Start-Sleep -Milliseconds 300 } } while (-not $bOpen -and (Get-Date) -lt $end)
    Start-Sleep -Seconds 3
    $win = Get-Win $p; $bOpen = CO-Box $win "完成後開啟資料夾"
    if ($bOpen) { CO-Toggle $bOpen }
    Check ((Get-Content $settingsFile -Raw) -match "open_folder_after_extract=true") "compact options: 'open folder' is saved to the settings"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # compression: its own options, no 'delete the archive'
    Reset-State
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    Set-Content "$co\c1.txt" ("data" * 50)
    $p = Start-Raw @("--compress", "zip", (Q "$co\c1.txt")); $win = Get-Win $p
    $bShow = $null; $end = (Get-Date).AddSeconds(8); do { $bShow = CO-Box $win "完成後在檔案總管顯示"; if (-not $bShow) { Start-Sleep -Milliseconds 300 } } while (-not $bShow -and (Get-Date) -lt $end)
    Check ($null -ne $bShow) "compact options: a compression offers 'show in Explorer'"
    $bTest = CO-Box $win "完成後測試完整性"
    Check ($null -ne $bTest) "compact options: a compression offers 'test the integrity'"
    Start-Sleep -Seconds 3
    Check (Test-Path "$co\c1.zip") "compact options: the compression ran"
    $win = Get-Win $p; $bTest = CO-Box $win "完成後測試完整性"
    if ($bTest) { CO-Toggle $bTest }
    Check ((Get-Content $settingsFile -Raw) -match "test_after_compress=true") "compact options: 'test the integrity' is saved to the settings"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "afterbuttons") {
        Reset-State
    # ===== 23. Buttons on a finished card: test the integrity / delete the originals (never automatic) =====
    $ab = Join-Path $t "afterbuttons"; New-Item -ItemType Directory $ab | Out-Null
    function AB-Button($w, $name) { $c = New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button))); $w.FindFirst($TS::Descendants, $c) }
    function AB-Wait($w, $name, [int]$sec = 12) { $e = (Get-Date).AddSeconds($sec); do { $x = AB-Button $w $name; if (-not $x) { Start-Sleep -Milliseconds 300 } } while (-not $x -and (Get-Date) -lt $e); $x }
    function AB-Press($el) { $el.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 700 }

    # --- extraction: the "delete the archive" button
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    Copy-Item "$t\sample.zip" "$ab\x.zip"
    $p = Start-Raw @("--extract-here", (Q "$ab\x.zip")); $win = Get-Win $p
    $del = AB-Wait $win "刪除原壓縮檔"
    Check ($null -ne $del) "after buttons: a finished extraction offers 'delete the archive'"
    Check (Test-Path "$ab\x.zip") "after buttons: nothing is deleted automatically"
    if ($del) {
        AB-Press $del
        Check ($null -ne (Wait-Find $win "刪除原壓縮檔？" 6)) "after buttons: the delete button asks first (warning dialog)"
        Click (Find $win "取消"); Start-Sleep -Milliseconds 800
        Check (Test-Path "$ab\x.zip") "after buttons: cancelling the question keeps the archive"
        $del = AB-Button $win "刪除原壓縮檔"
        AB-Press $del
        Wait-Find $win "刪除原壓縮檔？" 6 | Out-Null
        Click (Find $win "移到資源回收筒")
        $e = (Get-Date).AddSeconds(10); while ((Test-Path "$ab\x.zip") -and (Get-Date) -lt $e) { Start-Sleep -Milliseconds 300 }
        Check (-not (Test-Path "$ab\x.zip")) "after buttons: confirming moves the archive to the Recycle Bin"
        Check (Has-Text $win "*資源回收筒*" 6) "after buttons: the card says what happened"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # --- compression: test + delete the originals
    Reset-State
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    Set-Content "$ab\orig1.txt" ("data" * 200)
    $p = Start-Raw @("--compress", "zip", (Q "$ab\orig1.txt")); $win = Get-Win $p
    $tst = AB-Wait $win "測試完整性"
    $del = AB-Wait $win "刪除原始檔案"
    Check (($null -ne $tst) -and ($null -ne $del)) "after buttons: a finished compression offers 'test the integrity' and 'delete the originals'"
    Check (Test-Path "$ab\orig1.txt") "after buttons: the originals are untouched"
    if ($tst -and $del) {
        # not tested yet: the warning says so
        AB-Press $del
        Check ($null -ne (Wait-Find $win "刪除原始檔案？" 6)) "after buttons: deleting the originals asks first"
        Check (Has-Text $win "*尚未測試*" 4) "after buttons: the question warns that the archive was not tested"
        Click (Find $win "取消"); Start-Sleep -Milliseconds 800
        Check (Test-Path "$ab\orig1.txt") "after buttons: cancelling keeps the originals"
        # test it
        $tst = AB-Button $win "測試完整性"
        AB-Press $tst
        Check (Has-Text $win "*完整性測試通過*" 15) "after buttons: the test result is shown on the card"
        Check ($null -ne (AB-Wait $win "刪除原始檔案" 6)) "after buttons: the buttons come back after the test"
        $del = AB-Button $win "刪除原始檔案"
        AB-Press $del
        Wait-Find $win "刪除原始檔案？" 6 | Out-Null
        Check (Has-Text $win "*已經通過完整性測試*" 4) "after buttons: after a passed test the question says so"
        Click (Find $win "移到資源回收筒")
        $e = (Get-Date).AddSeconds(10); while ((Test-Path "$ab\orig1.txt") -and (Get-Date) -lt $e) { Start-Sleep -Milliseconds 300 }
        Check (-not (Test-Path "$ab\orig1.txt")) "after buttons: confirming moves the originals to the Recycle Bin"
        Check (Test-Path "$ab\orig1.zip") "after buttons: the new archive stays"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # --- test after compressing (the check box): runs by itself, result on the card
    Reset-State
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false`ntest_after_compress=true" -Encoding ascii
    Set-Content "$ab\orig2.txt" ("data" * 200)
    $p = Start-Raw @("--compress", "zip", (Q "$ab\orig2.txt")); $win = Get-Win $p
    Check (Has-Text $win "*完整性測試通過*" 20) "after buttons: 'test the integrity' runs by itself after compressing"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "saveimage") {
        Reset-State
    # ===== 24. Preview: Ctrl+S saves the one image as a file ("save as": the name can be changed) =====
    $sv = Join-Path $t "saveimage"; New-Item -ItemType Directory "$sv\src", "$sv\out" | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 120, 80; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::SteelBlue); $g.Dispose()
    $bmp.Save("$sv\src\pic.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Push-Location "$sv\src"; 7z a -tzip "$sv\img.zip" ".\pic.png" | Out-Null; Pop-Location
    $p = Start-App "$sv\img.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $rs = Find-Row $win "pic.png"
    if ($rs) {
        $rs.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        if ($pw) {
            Start-Sleep -Milliseconds 1500
            [System.Windows.Forms.SendKeys]::SendWait("^s")
            $pidCond = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
            $dlg = $null; $end = (Get-Date).AddSeconds(10)
            do { $dlg = $AE::RootElement.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition($pidCond, (ByName "另存圖片")))); if (-not $dlg) { Start-Sleep -Milliseconds 300 } } while (-not $dlg -and (Get-Date) -lt $end)
            Check ($null -ne $dlg) "save image: Ctrl+S opens a 'save as' dialog (not a folder picker)"
            if ($dlg) {
                # the dialog opens with the file name box focused: type a new full path over the suggested name
                Start-Sleep -Milliseconds 800
                # set the text through UI Automation, not SendKeys: an input method (a Chinese IME) swallows typed letters
                $ed = $null
                foreach ($c in $dlg.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Edit)))) {
                    if ($c.Current.Name -like "檔案名稱*" -or $c.Current.Name -like "File name*") { $ed = $c; break }
                }
                $vp = $null
                if ($ed -and $ed.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) { $vp.SetValue($sv + "\out\renamed.png") }
                else { [System.Windows.Forms.SendKeys]::SendWait("^a"); [System.Windows.Forms.SendKeys]::SendWait(($sv + "\out\renamed.png").Replace("{", "{{}").Replace("}", "{}}")) }
                Start-Sleep -Milliseconds 400
                [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
                $e = (Get-Date).AddSeconds(10); while (-not (Test-Path "$sv\out\renamed.png") -and (Get-Date) -lt $e) { Start-Sleep -Milliseconds 300 }
                Check (Test-Path "$sv\out\renamed.png") "save image: the image is saved under the new name"
                Check ((Test-Path "$sv\out\renamed.png") -and ((Get-FileHash "$sv\out\renamed.png").Hash -eq (Get-FileHash "$sv\src\pic.png").Hash)) "save image: the saved file is identical to the image in the archive"
                Check (-not (Test-Path "$sv\out\renamed.png.lycantmp")) "save image: no temporary file is left"
                # a second time: accepting the suggested name saves the image under its own name (in the remembered folder)
                Start-Sleep -Milliseconds 800
                [System.Windows.Forms.SendKeys]::SendWait("^s"); Start-Sleep -Milliseconds 2500
                [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
                $e = (Get-Date).AddSeconds(10); while (-not ((Test-Path "$sv\out\pic.png") -or (Test-Path "$sv\pic.png")) -and (Get-Date) -lt $e) { Start-Sleep -Milliseconds 300 }
                Check ((Test-Path "$sv\out\pic.png") -or (Test-Path "$sv\pic.png")) "save image: the dialog suggests the image's own name"
            }
        } else { Check $false "save image: the preview window opened" }
    } else { Check $false "save image: image row found" }
    Reset-State
    }

    if (Want "workerproc") {
        Reset-State
    # ===== 25. Tasks run in worker processes (lycanzip-cli.exe --worker), with the priority from the settings =====
    $wp = Join-Path $t "workerproc"; New-Item -ItemType Directory $wp | Out-Null
    $buf = New-Object byte[] (64MB); (New-Object Random 11).NextBytes($buf)
    $fs = [IO.File]::Create("$wp\big.bin"); 1..5 | ForEach-Object { $fs.Write($buf, 0, $buf.Length) }; $fs.Close()   # 320 MB: a few seconds of deflate
    Check (Test-Path (Join-Path $exeDir "lycanzip-cli.exe")) "worker: lycanzip-cli.exe sits next to the app"

    Set-Content -Path $settingsFile -Value "task_priority=below_normal`ncompact_close_when_done=true" -Encoding ascii
    $p = Start-Raw @("--compress", "zip", (Q "$wp\big.bin"))
    $worker = $null; $end = (Get-Date).AddSeconds(15)
    do { $worker = Get-Process lycanzip-cli -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $worker) { Start-Sleep -Milliseconds 200 } } while (-not $worker -and (Get-Date) -lt $end)
    Check ($null -ne $worker) "worker: a compression runs in a separate lycanzip-cli process"
    if ($worker) {
        Check ($worker.PriorityClass -eq "BelowNormal") "worker: the priority setting (below normal) is applied to the worker ($($worker.PriorityClass))"
        Check ($p.PriorityClass -eq "Normal") "worker: the app itself keeps its normal priority"
    }
    Check (Wait-Exit $p 90) "worker: the compact window closes after the compression"
    Check (Test-Path "$wp\big.zip") "worker: the archive was created by the worker"
    Start-Sleep -Milliseconds 800
    Check ($null -eq (Get-Process lycanzip-cli -ErrorAction SilentlyContinue)) "worker: no worker process is left behind"

    # closing the app kills a running worker (they are tied together)
    Reset-State
    Remove-Item "$wp\big.zip" -Force -ErrorAction SilentlyContinue
    Set-Content -Path $settingsFile -Value "compact_close_when_done=false" -Encoding ascii
    $p = Start-Raw @("--compress", "zip", (Q "$wp\big.bin"))
    $worker = $null; $end = (Get-Date).AddSeconds(15)
    do { $worker = Get-Process lycanzip-cli -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $worker) { Start-Sleep -Milliseconds 200 } } while (-not $worker -and (Get-Date) -lt $end)
    Stop-Process -Id $p.Id -Force
    Start-Sleep -Seconds 2
    Check ($null -eq (Get-Process lycanzip-cli -ErrorAction SilentlyContinue)) "worker: killing the app also ends its worker process"
    Reset-State
    }

    if (Want "closeguard") {
        Reset-State
    # ===== 18. Closing the window while tasks are still running: confirm first =====
    $cg = Join-Path $t "closeguard"; New-Item -ItemType Directory "$cg/src" | Out-Null
    $buf = New-Object byte[] (64MB); (New-Object Random 7).NextBytes($buf)
    foreach ($i in 1..12) { [IO.File]::WriteAllBytes("$cg/src/f$i.bin", $buf) }   # about 770 MB of incompressible data: packed with "store only", extracting it takes a few seconds
    Push-Location "$cg/src"; 7z a -tzip -mx=0 "$cg/big.zip" "./*" | Out-Null; Pop-Location
    $p = Start-App "$cg/big.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    $pauseBtn = Wait-Find $win "暫停" 8
    Check ($null -ne $pauseBtn) "closeguard: a long extraction is running (pause button visible)"
    if ($pauseBtn) { Click $pauseBtn }   # a pause counts as "not finished" too: the task stays unfinished, so the test does not depend on the extraction speed
    Start-Sleep -Milliseconds 600
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    $dlgTitle = Wait-Find $win "仍有工作進行中" 5
    Check ($null -ne $dlgTitle) "closeguard: closing the window with a running task asks first"
    Check (-not $p.HasExited) "closeguard: the window is still open while the question is shown"
    $keep = Find $win "繼續等待"
    if ($keep) { Click $keep }
    Start-Sleep -Milliseconds 800
    Check ($null -eq (Find $win "仍有工作進行中")) "closeguard: 'keep waiting' closes the question"
    Check ((-not $p.HasExited) -and ($null -ne (Find $win "繼續"))) "closeguard: the window and the (paused) task are still there"
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Check ($null -ne (Wait-Find $win "仍有工作進行中" 5)) "closeguard: closing again asks again"
    $go = Find $win "取消工作並關閉"
    if ($go) { Click $go }
    Check ($p.WaitForExit(15000)) "closeguard: 'cancel tasks and close' closes the window and ends the process"
    }

    if (Want "background") {
        Reset-State
    # ===== 26. Background work: a button hides the windows, the tasks keep running, the process exits when they are done =====
    $bg = Join-Path $t "background"; New-Item -ItemType Directory "$bg/src" | Out-Null
    $buf = New-Object byte[] (64MB); (New-Object Random 9).NextBytes($buf)
    foreach ($i in 1..20) { [IO.File]::WriteAllBytes("$bg/src/f$i.bin", $buf) }   # 1.3 GB stored: the extraction takes a while
    Push-Location "$bg/src"; 7z a -tzip -mx=0 "$bg/big.zip" "./*" | Out-Null; Pop-Location
    Remove-Item "$bg/src" -Recurse -Force
    Add-Type -Namespace N4 -Name W -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
public static int Count(uint procId, bool host, bool visibleOnly) {
    int n = 0;
    EnumWindows(delegate (IntPtr h, IntPtr l) {
        uint p; GetWindowThreadProcessId(h, out p);
        if (p != procId) return true;
        var sb = new System.Text.StringBuilder(256); GetClassName(h, sb, 256);
        bool isHost = sb.ToString() == "LycanZipBackgroundHost";
        if (isHost == host && (!visibleOnly || IsWindowVisible(h))) n++;
        return true;
    }, IntPtr.Zero);
    return n;
}
'@
    $p = Start-App "$bg/big.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Check ($null -eq (Find $win "背景執行")) "background: no button while nothing runs"
    Click (Find $win "解壓縮到此")
    $btn = Wait-Find $win "背景執行" 8
    Check ($null -ne $btn) "background: the 'run in the background' button appears while a task runs"
    Check ([N4.W]::Count([uint32]$p.Id, $true, $false) -eq 0) "background: no tray host window before"
    $pauseBtn = Find $win "暫停"
    if ($pauseBtn) { Click $pauseBtn }   # a paused task is unfinished: keeps the first run deterministic
    if ($btn) { Click $btn }
    Start-Sleep -Milliseconds 1200
    Check (-not $p.HasExited) "background: the process keeps running after the window is hidden"
    Check ([N4.W]::Count([uint32]$p.Id, $false, $true) -eq 0) "background: no visible window while in the background"
    Check ([N4.W]::Count([uint32]$p.Id, $true, $false) -eq 1) "background: the tray host exists while in the background"
    Check ($null -ne (Get-Process lycanzip-cli -ErrorAction SilentlyContinue)) "background: the worker keeps running"

    # a second launch (the single-instance forwarding) brings the windows back
    $p2 = Start-Raw @()
    Check ($p2.WaitForExit(15000)) "background: the second instance forwards and exits"
    Start-Sleep -Milliseconds 1000
    Check ([N4.W]::Count([uint32]$p.Id, $false, $true) -ge 1) "background: forwarding shows the window again"
    Check ([N4.W]::Count([uint32]$p.Id, $true, $false) -eq 0) "background: the tray host is gone after the window is back"
    $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "繼續" 3)) "background: the (paused) task is still there after coming back"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # second run: send it to the background right away and let it finish; the process quits by itself
    Remove-Item "$bg/big" -Recurse -Force -ErrorAction SilentlyContinue
    $p = Start-App "$bg/big.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    $btn = Wait-Find $win "背景執行" 8
    if ($btn) { Click $btn }
    Check ($p.WaitForExit(120000)) "background: the process exits by itself when all tasks are done"
    Check (@(Get-ChildItem "$bg/big" -Filter "f*.bin" -ErrorAction SilentlyContinue).Count -eq 20) "background: the extraction completed in the background (20 files)"
    Start-Sleep -Milliseconds 500
    Check ($null -eq (Get-Process lycanzip-cli -ErrorAction SilentlyContinue)) "background: no worker process is left behind"
    Reset-State
    }

    if (Want "execopen") {
        Reset-State
    # ===== 27. Opening a program from an archive: warning, extracting its folder, working directory, temp cleanup =====
    $eo = Join-Path $t "execopen"; New-Item -ItemType Directory "$eo/src/prog/sub", "$eo/out" | Out-Null
    [IO.File]::WriteAllText("$eo\src\prog\run.cmd", "@echo off`r`ntype data.txt > `"$eo\out\ran.txt`" 2> nul`r`ncd > `"$eo\out\cwd.txt`"`r`n", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText("$eo\src\prog\data.txt", "payload-123", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText("$eo\src\prog\sub\extra.txt", "extra", [Text.Encoding]::ASCII)
    Push-Location "$eo/src"; 7z a -tzip "$eo/prog.zip" "./prog" | Out-Null; Pop-Location
    $tmpOpen = Join-Path ([IO.Path]::GetTempPath()) "LycanZip\open"
    if (Test-Path $tmpOpen) { Get-ChildItem $tmpOpen -Directory | ForEach-Object { [IO.Directory]::Delete($_.FullName, $true) } }
    function Open-Prog($win) {
        # the archive may open inside its only folder: go into 'prog' only when it is listed
        $end = (Get-Date).AddSeconds(8)
        while (-not (Find-Row $win "prog") -and -not (Find-Row $win "run.cmd") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }
        if (Find-Row $win "prog") { Double-Click (Row-Text $win "prog") }
        $end = (Get-Date).AddSeconds(8)
        while (-not (Find-Row $win "run.cmd") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }
        Double-Click (Row-Text $win "run.cmd")
    }

    # 1. the warning: cancel does nothing
    $p = Start-App "$eo\prog.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Open-Prog $win
    Check ($null -ne (Wait-Find $win "要執行這個程式嗎？" 8)) "execopen: opening a .cmd from the archive asks first"
    Check ($null -ne (Find $win "同資料夾的檔案（建議）")) "execopen: the question offers 'files in the same folder'"
    Check ($null -ne (Find $win "只解壓這個檔案")) "execopen: the question offers 'only this file'"
    Check ($null -ne (Find $win "不再警告（可在設定中重新開啟）")) "execopen: the question has a 'do not warn again' box"
    $c = Find $win "取消"; if ($c) { Click $c }
    Start-Sleep -Milliseconds 1500
    Check (-not (Test-Path "$eo\out\ran.txt")) "execopen: cancelling runs nothing"
    Check (@(Get-ChildItem $tmpOpen -Recurse -Filter run.cmd -ErrorAction SilentlyContinue).Count -eq 0) "execopen: cancelling extracts nothing"

    # 2. run: the folder is extracted with it, the program runs in that folder and finds its data file
    Open-Prog $win
    Wait-Find $win "要執行這個程式嗎？" 8 | Out-Null
    $go = Find $win "解壓縮並執行"; if ($go) { Click $go }
    $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$eo\out\cwd.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Milliseconds 500
    Check ((Test-Path "$eo\out\ran.txt") -and ((Get-Content "$eo\out\ran.txt" -Raw) -match "payload-123")) "execopen: the program ran and found data.txt next to it"
    Check ((Test-Path "$eo\out\cwd.txt") -and ((Get-Content "$eo\out\cwd.txt" -Raw) -match "LycanZip\\open\\")) "execopen: it ran from the temp folder"
    Check ($null -ne (Get-ChildItem $tmpOpen -Recurse -Filter extra.txt -ErrorAction SilentlyContinue | Select-Object -First 1)) "execopen: subfolders were extracted too"

    # 3. 'do not warn again' is saved, and the next open runs without asking
    Remove-Item "$eo\out\*" -Force
    Open-Prog $win
    Wait-Find $win "要執行這個程式嗎？" 8 | Out-Null
    $nv = Find $win "不再警告（可在設定中重新開啟）"; if ($nv) { Click $nv }
    $go = Find $win "解壓縮並執行"; if ($go) { Click $go }
    $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$eo\out\cwd.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
    Check ((Test-Path $settingsFile) -and ((Get-Content $settingsFile -Raw) -match "warn_executable=false")) "execopen: 'do not warn again' is saved to the settings"
    Remove-Item "$eo\out\*" -Force
    Open-Prog $win
    $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$eo\out\cwd.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
    Check ((Test-Path "$eo\out\cwd.txt") -and ($null -eq (Find $win "要執行這個程式嗎？"))) "execopen: with the warning off it runs without asking"

    # 4. closing the app deletes its temp folders of programs that have finished
    Start-Sleep -Seconds 1
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Check ($p.WaitForExit(15000)) "execopen: the app closes"
    Start-Sleep -Milliseconds 800
    Check (@(Get-ChildItem $tmpOpen -Directory -ErrorAction SilentlyContinue).Count -eq 0) "execopen: the temp folders were deleted when the app closed"

    # 5. scope 'only this file': the program does not find data.txt
    Remove-Item "$eo\out\*" -Force
    Set-Content -Path $settingsFile -Value "warn_executable=false`nexecutable_scope=file_only" -Encoding ascii
    $p = Start-App "$eo\prog.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Open-Prog $win
    $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$eo\out\cwd.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Milliseconds 500
    Check ((Test-Path "$eo\out\cwd.txt") -and (-not ((Get-Content "$eo\out\ran.txt" -Raw -ErrorAction SilentlyContinue) -match "payload"))) "execopen: 'only this file' does not bring data.txt along"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # 6. start-up removes the folders of dead processes, but only old ones
    New-Item -ItemType Directory "$tmpOpen\999999-1-1", "$tmpOpen\999998-1-1" -Force | Out-Null
    [IO.File]::WriteAllText("$tmpOpen\999999-1-1\x.txt", "x"); [IO.File]::WriteAllText("$tmpOpen\999998-1-1\x.txt", "x")
    (Get-Item "$tmpOpen\999999-1-1").LastWriteTime = (Get-Date).AddDays(-3)
    $p = Start-App "$eo\prog.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Seconds 3
    Check (-not (Test-Path "$tmpOpen\999999-1-1")) "execopen: start-up deletes an old folder of a dead process"
    Check (Test-Path "$tmpOpen\999998-1-1") "execopen: a recent folder of a dead process is kept (the program may still use it)"
    Remove-Item "$tmpOpen\999998-1-1" -Recurse -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # 7. a program that is alone in its folder: still warned, but there is no scope to choose
    New-Item -ItemType Directory "$eo/solo" | Out-Null
    [IO.File]::WriteAllText("$eo\solo\solo.cmd", "@echo off`r`n", [Text.Encoding]::ASCII)
    Push-Location "$eo/solo"; 7z a -tzip "$eo/solo.zip" "./solo.cmd" | Out-Null; Pop-Location
    Set-Content -Path $settingsFile -Value "warn_executable=true" -Encoding ascii
    $p = Start-App "$eo\solo.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $end = (Get-Date).AddSeconds(8); while (-not (Find-Row $win "solo.cmd") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }
    Double-Click (Row-Text $win "solo.cmd")
    Check ($null -ne (Wait-Find $win "要執行這個程式嗎？" 8)) "execopen: a lone program is still warned about"
    Check ($null -eq (Find $win "同資料夾的檔案（建議）")) "execopen: no scope question when the program is alone in its folder"
    $c = Find $win "取消"; if ($c) { Click $c }
    Reset-State
    }

    if (Want "audiopreview") {
        Reset-State
    # ===== 28. The toolbar (Preview / View / More options) and the audio preview window =====
    $ap = Join-Path $t "audiopreview"; New-Item -ItemType Directory "$ap/src/songs" | Out-Null
    Set-Content "$ap/mk.py" -Encoding UTF8 -Value @'
import struct, sys, os
def sub(i, s):
    b = s.encode('utf-8') + b'\0'
    n = len(b)
    if n & 1: b += b'\0'
    return i + struct.pack('<I', n) + b
def wav(path, seconds, title, artist):
    rate = 8000; n = rate * seconds
    fmt = struct.pack('<HHIIHH', 1, 1, rate, rate, 1, 8)
    info = b'INFO' + sub(b'INAM', title) + sub(b'IART', artist)
    chunks = b'fmt ' + struct.pack('<I', 16) + fmt + b'LIST' + struct.pack('<I', len(info)) + info + b'data' + struct.pack('<I', n) + bytes([128]) * n
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks)
d = sys.argv[1]
wav(os.path.join(d, 'a1.wav'), 40, '月光奏鳴曲', '貝多芬')
wav(os.path.join(d, 'a2.wav'), 40, 'Second Song', 'Second Artist')
'@
    python "$ap/mk.py" "$ap/src/songs"
    Set-Content "$ap/src/songs/readme.txt" -Value "not audio" -Encoding ascii
    Set-Content "$ap/src/only.txt" -Value "text only" -Encoding ascii
    Push-Location "$ap/src"; 7z a -tzip "$ap/music.zip" "./songs" | Out-Null; 7z a -tzip "$ap/text.zip" "./only.txt" | Out-Null; Pop-Location
    function Audio-Win($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -like "音訊預覽*") { return $w } }; return $null }
    function Open-Songs($win) {
        $end = (Get-Date).AddSeconds(8)
        while (-not (Find-Row $win "songs") -and -not (Find-Row $win "a1.wav") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }
        if (Find-Row $win "songs") { Double-Click (Row-Text $win "songs") }
        $end = (Get-Date).AddSeconds(8); while (-not (Find-Row $win "a2.wav") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }
    }
    $env:LYCAN_TEST_MUTE = "1"   # the player must not make noise during the test
    function Shot-Rect($el, $name) {   # a screenshot of any window (the audio preview is not the main window)
        $r = $el.Current.BoundingRectangle
        $bmp = New-Object System.Drawing.Bitmap ([int]$r.Width), ([int]$r.Height); $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen([int]$r.X, [int]$r.Y, 0, 0, $bmp.Size); $g.Dispose(); $bmp.Save((Join-Path $shots "$name.png")); $bmp.Dispose()
    }

    # the toolbar
    $p = Start-App "$ap\music.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Check (($null -ne (Find $win "預覽")) -and ($null -ne (Find $win "檢視")) -and ($null -ne (Find $win "其他選項"))) "toolbar: Preview, View and More options drop-downs exist"
    Check (($null -eq (Find $win "全部列出")) -and ($null -eq (Find $win "資料夾樹開關")) -and ($null -eq (Find $win "測試完整性"))) "toolbar: 'list all', 'folder tree' and 'test integrity' are no longer buttons"
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500
    Check (($null -ne (Find $win "全部列出")) -and ($null -ne (Find $win "資料夾樹開關"))) "toolbar: the View menu holds 'list all' and 'folder tree'"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
    Check ($null -ne (Find $win "測試完整性")) "toolbar: 'test integrity' is in the More options menu"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    Click (Find $win "預覽"); Start-Sleep -Milliseconds 500
    Shot $p "toolbar-preview-menu"
    Check (($null -ne (Find $win "圖形預覽")) -and ($null -ne (Find $win "音訊預覽"))) "toolbar: the Preview menu offers image and audio preview"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400

    # audio preview: starts at the selected file, shows its tags, plays, goes on to the next
    Open-Songs $win
    $r2 = Find-Row $win "a2.wav"
    if ($r2) { $r2.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400 }
    Click (Find $win "預覽"); Start-Sleep -Milliseconds 500; Click (Find $win "音訊預覽")
    $aw = $null; $end = (Get-Date).AddSeconds(12); while (-not $aw -and (Get-Date) -lt $end) { $aw = Audio-Win $p; if (-not $aw) { Start-Sleep -Milliseconds 300 } }
    Check ($null -ne $aw) "audio: the audio preview window opens"
    if ($aw) {
        Check (Has-Text $aw "Second Song" 10) "audio: it starts at the selected file and shows the title from its tags"
        Check (Has-Text $aw "Second Artist" 3) "audio: the artist from the tags is shown"
        Check ($null -ne (Wait-Find $aw "暫停" 8)) "audio: the file plays (the button offers 'pause')"
        Check (Has-Text $aw "0:0[1-9]" 8) "audio: the position moves"
        Shot-Rect $aw "audio-preview"
        Click (Find $aw "上一首（Ctrl+←）")
        Check (Has-Text $aw "月光奏鳴曲" 10) "audio: 'previous' plays the other file (Chinese title from its tags)"
        Check (Has-Text $aw "貝多芬" 3) "audio: the other artist is shown"
        Click (Wait-Find $aw "暫停" 8)
        Start-Sleep -Milliseconds 600
        Check ($null -ne (Find $aw "播放")) "audio: pause turns the button into 'play'"
        $aw.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
        Start-Sleep -Milliseconds 800
        Check ($null -eq (Audio-Win $p)) "audio: closing the window closes the player"
        Check (-not $p.HasExited) "audio: the main window is still there"
    }
    # the image preview in an archive without images: told after asking
    Click (Find $win "預覽"); Start-Sleep -Milliseconds 500; Click (Find $win "圖形預覽")
    Check ($null -ne (Wait-Find $win "沒有可以預覽的圖片" 6)) "preview: asking for an image preview where there are no images gives a message"
    $ok = Find $win "確定"; if ($ok) { Click $ok }
    Start-Sleep -Milliseconds 500
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # no audio at all
    $p = Start-App "$ap\text.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "預覽"); Start-Sleep -Milliseconds 500; Click (Find $win "音訊預覽")
    Check ($null -ne (Wait-Find $win "沒有可以預覽的音訊" 6)) "audio: asking for an audio preview where there is no audio gives a message"
    Check ($null -eq (Audio-Win $p)) "audio: no player window is opened then"
    Remove-Item Env:\LYCAN_TEST_MUTE
    Reset-State
    }

    if (Want "loadview") {
        Reset-State
    # ===== 29. Opening an archive from nothing: no home page flash, a loading view with the file name; settings header stays =====
    $lv = Join-Path $t "loadview"; New-Item -ItemType Directory "$lv/src" | Out-Null
    Set-Content "$lv/src/a.txt" -Value "hello" -Encoding ascii
    Push-Location "$lv/src"; 7z a -tzip "$lv/quick.zip" "./a.txt" | Out-Null; 7z a -t7z -mhe=on -psecret "$lv/enc.7z" "./a.txt" | Out-Null; Pop-Location
    # polls the window of a freshly started process: did the home page or the loading text ever show?
    function Watch-Start($proc, [int]$ms, [string]$until) {
        $seenHome = $false; $seenLoading = $false; $reachedUntil = $false
        $end = (Get-Date).AddMilliseconds($ms)
        while ((Get-Date) -lt $end) {
            $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
            $w = $AE::RootElement.FindFirst($TS::Children, $c)
            if ($w) {
                foreach ($e in $w.FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                    $n = $e.Current.Name
                    if ($n -eq "將檔案拖曳到此處") { $seenHome = $true }
                    elseif ($n -like "正在開啟*") { $seenLoading = $true }
                    elseif ($until -and $n -eq $until) { $reachedUntil = $true }
                }
                if ($reachedUntil) { break }
            }
        }
        return [pscustomobject]@{ Home = $seenHome; Loading = $seenLoading; Reached = $reachedUntil }
    }

    # a quick open: neither the home page nor the spinner is shown on the way
    $p = Start-Raw @((Q "$lv\quick.zip"))
    $r = Watch-Start $p 8000 "解壓縮到此"
    Check $r.Reached "loadview: the archive view appears"
    Check (-not $r.Home) "loadview: the home page was never shown while opening"
    Check (-not $r.Loading) "loadview: a quick open does not flash the spinner"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # a slow open: blank first, then the spinner with the file name, never the home page
    $env:LYCAN_TEST_OPEN_DELAY = "3000"
    $p = Start-Raw @((Q "$lv\quick.zip"))
    Start-Sleep -Milliseconds 1200; Shot $p "loading-view"
    $r = Watch-Start $p 10000 "解壓縮到此"
    Remove-Item Env:\LYCAN_TEST_OPEN_DELAY
    Check $r.Reached "loadview: the slow archive opens in the end"
    Check $r.Loading "loadview: a slow open shows the spinner with the file name"
    Check (-not $r.Home) "loadview: the home page was never shown during the slow open"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # an encrypted header: the password question comes over the loading view, cancelling goes back to the home page
    $p = Start-Raw @((Q "$lv\enc.7z"))
    $win = Get-Win $p
    $dlg = Wait-Find $win "取消" 10
    $r = Watch-Start $p 1200 ""
    Check ($null -ne $dlg) "loadview: an encrypted archive asks for the password"
    Check (-not $r.Home) "loadview: the home page is not behind the password question"
    $dlg = Find $win "取消"; if ($dlg) { Click $dlg }
    Check ($null -ne (Wait-Find $win "將檔案拖曳到此處" 8)) "loadview: cancelling the password question returns to the home page"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # the settings header does not scroll away
    $p = Start-App "$lv\quick.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("^,"); Start-Sleep -Milliseconds 900   # the page fades in; its controls exist (collapsed) before
    $reset = Wait-Find $win "還原預設設定" 8
    Check ($null -ne $reset) "settings header: the settings page opens"
    Shot $p "settings-top"
    $scrolled = $false
    foreach ($e in $win.FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
        $sp = $null
        if ($e.TryGetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern, [ref]$sp) -and $sp.Current.VerticallyScrollable) { $sp.SetScrollPercent([Windows.Automation.ScrollPattern]::NoScroll, 100); $scrolled = $true; break }
    }
    Start-Sleep -Milliseconds 600
    Shot $p "settings-scrolled"
    Check $scrolled "settings header: the settings page can be scrolled down"
    $reset = Find $win "還原預設設定"
    $ry = if ($reset) { $reset.Current.BoundingRectangle.Y } else { -1 }
    $wy = $win.Current.BoundingRectangle.Y
    Check (($null -ne $reset) -and (-not $reset.Current.IsOffscreen) -and ($ry - $wy -lt 220)) "settings header: after scrolling down the header (reset button) is still at the top"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
    Check ($null -ne (Wait-Find $win "解壓縮到此" 5)) "settings header: Esc still leaves the settings page"
    Reset-State
    }

    if (Want "bandfull") {
        Reset-State
    # ===== 30. Band selection in a full list: starting on the blank part of a row (the item's own drag must not win) =====
    $bf = Join-Path $t "bandfull"; New-Item -ItemType Directory "$bf/src" | Out-Null
    1..120 | ForEach-Object { Set-Content ("$bf/src/file{0:000}.txt" -f $_) -Value "x" -Encoding ascii }
    Push-Location "$bf/src"; 7z a -tzip "$bf/many.zip" "./*" | Out-Null; Pop-Location
    $scrB = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    function MoveB($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scrB.Width), [uint32]($y * 65535 / $scrB.Height), 0, [UIntPtr]::Zero) }
    function Rows-B($w) { @($w.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem))) | Where-Object { -not $_.Current.IsOffscreen }) }
    function SelCount-B($rows) { $n = 0; foreach ($r in $rows) { if ($r.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $n++ } }; return $n }
    $p = Start-App "$bf\many.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    [Win.D]::MoveWindow($p.MainWindowHandle, 20, 20, 1300, 800, $true) | Out-Null; Start-Sleep -Milliseconds 1200
    $rows = Rows-B $win
    Check ($rows.Count -ge 6) "band select (full list): the list is longer than the window ($($rows.Count) rows visible)"
    if ($rows.Count -ge 6) {
        $r2 = $rows[2].Current.BoundingRectangle; $r6 = $rows[6].Current.BoundingRectangle
        $bx = [int]($r2.X + $r2.Width * 0.45); $y0 = [int]($r2.Y + $r2.Height / 2); $y1 = [int]($r6.Y + $r6.Height / 2)
        [Win.D]::SetCursorPos($bx, $y0) | Out-Null; Start-Sleep -Milliseconds 400
        [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        for ($i = 1; $i -le 30; $i++) { MoveB ($bx + [int](6 * $i / 30)) ([int]($y0 + ($y1 - $y0) * $i / 30)); Start-Sleep -Milliseconds 30 }
        Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
        $n = SelCount-B (Rows-B $win)
        Check ($n -eq 5) "band select (full list): dragging from the blank part of a row over 5 rows selects them ($n)"
    }
    Stop-Process -Id $p.Id -Force
    Reset-State
    }

    if (Want "tempdir") {
        Reset-State
    # ===== 31. Temporary files: the settings card, a chosen folder, clearing, and "no room" for the tar of a tar.gz =====
    $td = Join-Path $t "tempdir"; New-Item -ItemType Directory "$td/src", "$td/scratch" | Out-Null
    Set-Content "$td/src/a.txt" -Value ("hello " * 20000) -Encoding ascii
    Push-Location "$td/src"; 7z a -tzip "$td/z.zip" "./a.txt" | Out-Null; 7z a -ttar "$td/t.tar" "./a.txt" | Out-Null; 7z a -tgzip "$td/t.tar.gz" "$td/t.tar" | Out-Null; Pop-Location

    # the settings card, default location
    $p = Start-App "$td\z.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    [System.Windows.Forms.SendKeys]::SendWait("^,"); Start-Sleep -Milliseconds 900
    Check ($null -ne (Wait-Find $win "清除暫存" 6)) "tempdir: the settings page has the temporary folder card"
    Check (Has-Text $win "系統的暫存資料夾" 3) "tempdir: it says the system temporary folder is used by default"
    Check (Has-Text $win "目前使用：*" 5) "tempdir: it shows how much the temporary files use"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 800

    # a chosen folder: files opened from an archive go there; clearing removes what nothing uses
    Set-Content -Path $settingsFile -Value "temp_dir=$td\scratch" -Encoding ascii
    $openRoot = "$td\scratch\LycanZip\open"
    New-Item -ItemType Directory "$openRoot\999999-1-1" -Force | Out-Null; Set-Content "$openRoot\999999-1-1\old.txt" -Value "x" -Encoding ascii
    $env:LYCAN_TEST_NO_OPEN = "1"
    $p = Start-App "$td\z.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Double-Click (Row-Text $win "a.txt")
    $hit = $null; $end = (Get-Date).AddSeconds(12)
    while (-not $hit -and (Get-Date) -lt $end) { $hit = Get-ChildItem $openRoot -Recurse -Filter a.txt -ErrorAction SilentlyContinue | Select-Object -First 1; Start-Sleep -Milliseconds 300 }
    Check ($null -ne $hit) "tempdir: a file opened from an archive is extracted into the chosen temporary folder"
    [System.Windows.Forms.SendKeys]::SendWait("^,"); Start-Sleep -Milliseconds 900
    Check (Has-Text $win "$td\scratch" 5) "tempdir: the card shows the chosen folder"
    $clr = Wait-Find $win "清除暫存" 6
    if ($clr) { Click $clr }
    Check ($null -ne (Wait-Find $win "暫存已清除" 6)) "tempdir: clearing reports what it freed"
    $ok = Find $win "確定"; if ($ok) { Click $ok }
    Start-Sleep -Milliseconds 500
    Check (-not (Test-Path "$openRoot\999999-1-1")) "tempdir: clearing deleted the folder of a process that is gone"
    Remove-Item Env:\LYCAN_TEST_NO_OPEN
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Check ($p.WaitForExit(15000)) "tempdir: the app closes"
    Start-Sleep -Milliseconds 600
    Check (@(Get-ChildItem $openRoot -Directory -ErrorAction SilentlyContinue).Count -eq 0) "tempdir: the temporary folders in the chosen folder were deleted when the app closed"

    # no room for the tar of a tar.gz: the question comes (instead of a plain failure); cancelling goes back to the home page
    Remove-Item $settingsFile -Force -ErrorAction SilentlyContinue
    $env:LYCAN_TEST_FAIL_WRITE_AFTER = "2000"
    $p = Start-Raw @((Q "$td\t.tar.gz"))
    $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "暫存空間不足" 15)) "tempdir: a full scratch folder asks where else to put it"
    Check ($null -ne (Find $win "以後都用這個暫存位置")) "tempdir: the question has 'always use this location'"
    Check ($null -ne (Find $win "選擇其他資料夾…")) "tempdir: the question offers to choose another folder"
    Shot $p "tempspace-question"
    $c = Find $win "取消"; if ($c) { Click $c }
    Check ($null -ne (Wait-Find $win "將檔案拖曳到此處" 8)) "tempdir: cancelling the question goes back to the home page"
    Remove-Item Env:\LYCAN_TEST_FAIL_WRITE_AFTER
    Stop-Process -Id $p.Id -Force
    Reset-State
    }

    if (Want "previewpane") {
        Reset-State
    # ===== Preview pane under the folder tree: archive summary, picture, text, PDF, programs, the larger window, the switch, row height =====
    $pp = Join-Path $t "previewpane"; New-Item -ItemType Directory "$pp\src\docs" -Force | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 64, 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
    $bmp.Save("$pp\src\pic1.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Set-Content "$pp\src\readme.txt" "hello pane" -Encoding ASCII
    foreach ($i in 1..8) { Set-Content "$pp\src\t$i.txt" "content-$i" -Encoding ASCII }
    Set-Content "$pp\src\docs\inner.txt" "inner" -Encoding ASCII
    [IO.File]::WriteAllBytes("$pp\src\setup.exe", [byte[]](@(0x4D, 0x5A) + (@(0) * 254)))
    # a one-page PDF (the offsets of the cross-reference table are counted here)
    $objs = @("1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj`n", "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj`n", "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 120]>>endobj`n")
    $pdf = "%PDF-1.4`n"; $offs = @()
    foreach ($o in $objs) { $offs += $pdf.Length; $pdf += $o }
    $xref = $pdf.Length
    $pdf += "xref`n0 4`n0000000000 65535 f `n" + (($offs | ForEach-Object { "{0:D10} 00000 n `n" -f $_ }) -join "") + "trailer<</Size 4/Root 1 0 R>>`nstartxref`n$xref`n%%EOF`n"
    [IO.File]::WriteAllText("$pp\src\page.pdf", $pdf, [Text.Encoding]::ASCII)
    Push-Location "$pp\src"; 7z a -tzip "$pp\pane.zip" ".\*" | Out-Null; Pop-Location
    $p = Start-App "$pp\pane.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    function Pick($name) { $r = Find-Row $win $name; if ($r) { $r.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select() }; return $r }
    function Large-Window { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id); foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) { if ($w.Current.Name -like "預覽 – *" -or $w.Current.Name -eq "預覽") { return $w } }; return $null }
    # nothing selected: the archive itself
    Check (Has-Text $win "原始大小" 6) "preview pane: with nothing selected it shows the archive (original size row)"
    # a picture: decoded, with its size
    Pick "pic1.png" | Out-Null
    Check (Has-Text $win "pic1.png, 64 × 48 px" 6) "preview pane: a picture is decoded (its accessible name has the size, 64 × 48 px)"
    Check (-not (Has-Text $win "CRC" 1)) "preview pane: a picture fills the pane (no name and details under it)"
    $enl = Wait-Find $win "開啟較大的預覽" 4
    Check ($null -ne $enl) "preview pane: a picture has the 'larger preview' button"
    # text
    Pick "readme.txt" | Out-Null
    Check (Has-Text $win "hello pane*" 6) "preview pane: a text file shows its content"
    Check (-not (Has-Text $win "CRC" 1)) "preview pane: a text fills the pane (no details under it)"
    # a PDF: the first page
    Pick "page.pdf" | Out-Null
    Check (Has-Text $win "page.pdf, 第 1 頁，共 1 頁" 8) "preview pane: a PDF shows its first page"
    # a program is never unpacked for the pane
    Pick "setup.exe" | Out-Null
    Check (Has-Text $win "程式與安裝程式不提供預覽。" 6) "preview pane: a program is not previewed"
    Check (Has-Text $win "CRC" 4) "preview pane: what cannot be previewed keeps its name and details"
    Check ($null -eq (Find $win "開啟較大的預覽")) "preview pane: a program has no 'larger preview' button"
    # a folder: its files
    Pick "docs" | Out-Null
    Check (Has-Text $win "1 個檔案" 6) "preview pane: a folder shows how many files it has"
    # selection moving fast (arrow key held): the pane ends on the last item
    Pick "t1.txt" | Out-Null; Start-Sleep -Milliseconds 300
    try { (Find-Row $win "t1.txt").SetFocus() } catch { }
    [System.Windows.Forms.SendKeys]::SendWait("{DOWN}{DOWN}{DOWN}{DOWN}{DOWN}");
    Check (Has-Text $win "content-6*" 6) "preview pane: after five quick Down arrows it shows the item the selection stopped on (t6.txt)"
    Check (-not (Has-Text $win "content-3*" 1)) "preview pane: the items passed on the way are not left on the screen"
    # the larger window follows the selection; Esc closes it
    Pick "pic1.png" | Out-Null; Start-Sleep -Milliseconds 900
    $enl = Wait-Find $win "開啟較大的預覽" 4
    if ($enl) { $enl.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() }
    $lw = $null; $end = (Get-Date).AddSeconds(8); do { $lw = Large-Window; if (-not $lw) { Start-Sleep -Milliseconds 250 } } while (-not $lw -and (Get-Date) -lt $end)
    $names = (@($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)))) | ForEach-Object { $_.Current.Name }) -join " | "
    Check ($null -ne $lw -and $lw.Current.Name -eq "預覽 – pic1.png") "preview pane: the button opens the larger preview window ('預覽 – pic1.png'; windows: $names; button found: $($null -ne $enl))"
    if ($lw) {
        Check (Has-Text $lw "pic1.png, 64 × 48 px" 6) "larger preview: shows the same picture"
        Check (-not (Has-Text $lw "CRC" 1)) "larger preview: no details under the picture either"
        Pick "readme.txt" | Out-Null; Start-Sleep -Milliseconds 1200
        $lw = Large-Window
        Check ($lw -and $lw.Current.Name -eq "預覽 – readme.txt") "larger preview: follows the selection of the main window"
        Add-Type -Namespace U5 -Name W -ErrorAction SilentlyContinue -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);'
        function PP-Activate($w) { [U5.W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [U5.W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [U5.W]::SetForegroundWindow([IntPtr]$w.Current.NativeWindowHandle) | Out-Null; Start-Sleep -Milliseconds 600 }
        PP-Activate $lw   # (choosing in the main window may have brought that one to the front)
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
        Check ($null -eq (Large-Window)) "larger preview: Esc closes it"
        PP-Activate $win
    }
    # the folder tree rows: 24 high, 26 apart (in DIP)
    $dpi = [Win.D]::GetDpiForWindow([IntPtr]$win.Current.NativeWindowHandle)
    $tc = New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::TreeItem)
    $tr = @($win.FindAll($TS::Descendants, $tc) | ForEach-Object { $_.Current.BoundingRectangle } | Where-Object { $_.Height -gt 0 } | Sort-Object Y)
    if ($tr.Count -ge 2 -and $dpi) {
        $pitch = ($tr[1].Y - $tr[0].Y) * 96 / $dpi
        Check ([math]::Abs($pitch - 26) -le 1) "folder tree: the rows are 26 DIP apart (was 32), measured $([math]::Round($pitch, 1))"
    } else { Check $false "folder tree: at least two rows to measure (found $($tr.Count))" }
    # the switch: Ctrl+Shift+P hides the pane (and is remembered), again shows it
    try { (Find-Row $win "readme.txt").SetFocus() } catch { }; Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait("^+p"); Start-Sleep -Milliseconds 900
    Check (-not (Has-Text $win "hello pane*" 1)) "preview pane: Ctrl+Shift+P hides it"
    Check ((Get-Content $settingsFile -Raw) -match "preview_pane=false") "preview pane: hiding it is saved (preview_pane=false)"
    [System.Windows.Forms.SendKeys]::SendWait("^+p"); Start-Sleep -Milliseconds 900
    Check (Has-Text $win "hello pane*" 6) "preview pane: Ctrl+Shift+P shows it again with the selected item"
    Shot $p "preview-pane"
    Stop-Process -Id $p.Id -Force
    }

    if (Want "settingssearch") {
        Reset-State
    # ===== Settings search: the box in the header finds cards on every tab (also by the English words), Enter goes to the first one =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    # the suggestion list is a popup: search all windows of the process
    function Suggestion($pattern, $sec = 5) {
        $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
        $end = (Get-Date).AddSeconds($sec)
        do {
            foreach ($w in $AE::RootElement.FindAll($TS::Children, $c)) {
                $hit = (All-Elements $w) | Where-Object { $_.Current.Name -like $pattern -and $_.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem } | Select-Object -First 1
                if ($hit) { return $hit }
            }
            Start-Sleep -Milliseconds 250
        } while ((Get-Date) -lt $end)
        return $null
    }
    function Visible-Setting($name) { $e = Find $win $name; return ($null -ne $e) -and (-not $e.Current.IsOffscreen) }
    $sbox = Wait-Find $win "搜尋設定" 6
    Check ($null -ne $sbox) "settings search: the search box is in the header of the settings page"
    $editCond = New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Edit)
    if ($sbox -and $sbox.Current.ControlType -ne [Windows.Automation.ControlType]::Edit) { $inner = $sbox.FindFirst($TS::Descendants, $editCond); if ($inner) { $sbox = $inner } }   # the text box inside the AutoSuggestBox
    Open-SettingsTab $win "關於"
    try { (Find $win "開啟設定檔位置").SetFocus() } catch { }; Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait("^f"); Start-Sleep -Milliseconds 600
    $focused = $AE::FocusedElement
    Check ($focused -and ($focused.Current.Name -eq "搜尋設定" -or $focused.Current.ControlType -eq [Windows.Automation.ControlType]::Edit)) "settings search: Ctrl+F on the settings page focuses the search box"
    # a setting of another tab, by its Chinese title
    Type-Into $sbox "同時執行"; Start-Sleep -Milliseconds 700
    Check ($null -ne (Suggestion "同時執行的工作數*基本*")) "settings search: the suggestions list the card with its tab ('同時執行的工作數 · 基本')"
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 1200
    $end = (Get-Date).AddSeconds(3); while (-not (Visible-Setting "同時執行的工作數") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 250 }   # the tab change and the scroll can take a moment
    Check (Visible-Setting "同時執行的工作數") "settings search: Enter goes to the first card (the General tab is shown)"
    Check (-not (Visible-Setting "開啟設定檔位置")) "settings search: the About tab is not shown any more"
    # the English words find the translated card
    Type-Into $sbox "add context menu"; Start-Sleep -Milliseconds 700
    Check ($null -ne (Suggestion "在右鍵選單加入 LycanZip*檔案總管*")) "settings search: English words find a card of the Chinese interface ('add context menu' -> the Explorer tab)"
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 1200
    Check (Visible-Setting "在右鍵選單加入 LycanZip") "settings search: Enter shows the Explorer tab"
    Start-Sleep -Milliseconds 100; Shot $p "settings-search"   # the card is outlined for a moment
    # nothing found
    Type-Into $sbox "zzqqxxyy"; Start-Sleep -Milliseconds 700
    Check ($null -ne (Suggestion "沒有符合的設定")) "settings search: no match says so"
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 700
    Check (Visible-Setting "在右鍵選單加入 LycanZip") "settings search: Enter with no match stays where it was"
    # Esc: first clears the search, then leaves the settings
    $sbox.SetFocus(); Start-Sleep -Milliseconds 300
    $sl = $null; $sp = $sbox; while ($sp -and -not $sp.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$sl)) { $sp = [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($sp) }
    if ($sl -and $sl.Current.ExpandCollapseState -eq [Windows.Automation.ExpandCollapseState]::Expanded) { [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400 }   # the first Esc only closes an open suggestion list
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600
    $vp = $null; $txt = if ($sbox.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) { $vp.Current.Value } else { "?" }
    Check ($txt -eq "" -and $null -ne (Find $win "設定分頁")) "settings search: Esc clears the search and stays on the settings page (text '$txt')"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 900
    Check ($null -ne (Find $win "解壓縮到此")) "settings search: Esc in the empty search box leaves the settings (back to the archive)"
    Stop-Process -Id $p.Id -Force
    }

    if (Want "settingstabs") {
        Reset-State
    # ===== 32. Settings tabs: one group at a time, the header and the tab strip stay on top =====
    $aboutUrls = Join-Path $t "about_urls.txt"; Remove-Item $aboutUrls -ErrorAction SilentlyContinue; $env:LYCAN_TEST_URL_FILE = $aboutUrls   # the links of the About page write their address here instead of opening a browser
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    $bar = Wait-Find $win "設定分頁" 8
    Check ($null -ne $bar) "settings tabs: the tab strip exists"
    $names = foreach ($e in $bar.FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition)) { $e.Current.Name }
    foreach ($n in "基本", "壓縮", "檢視", "檔案總管", "快速鍵", "關於") { Check ($names -contains $n) "settings tabs: the tab '$n' exists" }
    function Visible-Settings($name) { $e = Find $win $name; return ($null -ne $e) -and (-not $e.Current.IsOffscreen) }
    # the sound sets (Synth is the default, Wave the first set): the combo box is on the first tab; nothing is switched (that would play a sound)
    $soundSet = Find $win "音效組"
    Check ($null -ne $soundSet) "settings: there is a sound set picker (Synth / Wave)"
    # (tab, a control that is on it, a control that is on another tab)
    $cases = @(
        @("基本", "語言", "檔案衝突處理"),
        @("壓縮", "檔案衝突處理", "同時執行的工作數"),
        @("檢視", "記住上次選擇的檢視", "檔案衝突處理"),
        @("檢視", "使用 Windows 的檔案圖示", "檔案衝突處理"),
        @("檔案總管", "在右鍵選單加入 LycanZip", "記住上次選擇的檢視"),
        @("快速鍵", "Ctrl + N", "在右鍵選單加入 LycanZip"),
        @("關於", "開啟設定檔位置", "Ctrl + N"),
        @("關於", "網站", "Ctrl + N"),
        @("關於", "GitHub 上的原始碼", "Ctrl + N"))
    foreach ($c in $cases) {
        Open-SettingsTab $win $c[0]
        Check (Visible-Settings $c[1]) "settings tabs: '$($c[0])' shows '$($c[1])'"
        Check (-not (Visible-Settings $c[2])) "settings tabs: '$($c[0])' does not show '$($c[2])' (another tab)"
    }
    Check (Visible-Settings "還原預設設定") "settings tabs: the reset button stays on every tab"
    # "Restart File Explorer" (Explorer tab): the button asks first; the test only cancels (it must never restart the Explorer of this machine)
    Open-SettingsTab $win "檔案總管"
    $rb = Find $win "重新啟動"
    Check ($null -ne $rb) "settings tabs: the Explorer tab has a button to restart File Explorer"
    if ($rb) {
        Click $rb
        Check ($null -ne (Wait-Find $win "要重新啟動檔案總管嗎？" 6)) "settings tabs: the restart asks first (the question names what happens)"
        $cb = Find $win "取消"; if ($cb) { Click $cb }; Start-Sleep -Milliseconds 600
        Check ($null -eq (Find $win "要重新啟動檔案總管嗎？")) "settings tabs: cancel closes the question"
    }
    # the two links of the About page (the web site and the source on GitHub)
    Open-SettingsTab $win "關於"
    $lw = Find $win "網站"; if ($lw) { Click $lw; Start-Sleep -Milliseconds 700 }
    $lg = Find $win "GitHub 上的原始碼"; if ($lg) { Click $lg; Start-Sleep -Milliseconds 700 }
    $opened = if (Test-Path $aboutUrls) { @(Get-Content $aboutUrls) } else { @() }
    Check ($opened -contains "https://aegisulf.com/lycanzip/") "settings tabs: the 'Web site' link of the About page opens aegisulf.com/lycanzip"
    Check ($opened -contains "https://github.com/Aegisulf/LycanZip") "settings tabs: the 'Source code on GitHub' link opens the repository"
    Remove-Item Env:\LYCAN_TEST_URL_FILE -ErrorAction SilentlyContinue
    Open-SettingsTab $win "基本"
    Shot $p "settings-tabs"
    Stop-Process -Id $p.Id -Force
    # the Microsoft Store version (LYCAN_TEST_STORE_UI=1 pretends to be one): the Windows 11 menu and the file types are built in, the classic menu is not offered
    Reset-State
    $env:LYCAN_TEST_STORE_UI = "1"
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 1500
    Check (Has-Text $win "*這是 Microsoft Store 版*" 6) "store version: the Explorer tab says that the menu and the file types are built in"
    Check ($null -eq (Find $win "註冊 LycanZip 為壓縮檔程式") -and $null -eq (Find $win "重新啟動檔案總管")) "store version: the cards that register (file types, Windows 11 menu) and the Explorer restart are hidden"
    $da = Find $win "開啟預設應用程式設定"
    Check ($null -ne $da -and $da.Current.IsEnabled) "store version: 'Open default apps settings' can be used"
    $sm = $null   # the label has the same name as the switch: take the one that can be toggled
    foreach ($e in $win.FindAll($TS::Descendants, (ByName "在右鍵選單加入 LycanZip"))) { $tp = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp)) { $sm = $e } }
    Check ($null -ne $sm -and $sm.Current.IsEnabled -and $sm.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On") "store version: the menu switch is on and can be changed"
    if ($sm) {
        $sm.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 900
        Check ((Test-Path $settingsFile) -and (Get-Content $settingsFile -Raw -Encoding UTF8) -match "(?m)^shell_menu=false") "store version: switching the menu off writes shell_menu=false (the menu DLL then shows nothing)"
    }
    Remove-Item Env:\LYCAN_TEST_STORE_UI -ErrorAction SilentlyContinue
    Stop-Process -Id $p.Id -Force
    # another copy while the Store version is installed (LYCAN_TEST_STORE_INSTALLED=1): the Store version has the menu and the file types, the switches stay off here
    Reset-State
    $env:LYCAN_TEST_STORE_INSTALLED = "1"
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 1500
    Check (Has-Text $win "*安裝了 Microsoft Store 版的 LycanZip*" 6) "two copies: the Explorer tab says that the Store version provides the menu and the file types"
    $sm = $null
    foreach ($e in $win.FindAll($TS::Descendants, (ByName "在右鍵選單加入 LycanZip"))) { $tp = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp)) { $sm = $e } }
    Check ($null -ne $sm -and -not $sm.Current.IsEnabled) "two copies: the classic menu switch cannot be switched on"
    $env:LYCAN_TEST_STORE_INSTALLED = "0"
    Stop-Process -Id $p.Id -Force
    Reset-State
    }

    if (Want "titlebar") {
        Reset-State
    # ===== 32b. Title strip: the settings button is plain client area (quick clicks never maximize); the empty strip still drags / maximizes =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $h = [IntPtr]$win.Current.NativeWindowHandle
    function Win-Rect { $r = New-Object Win.D+RECT; [Win.D]::GetWindowRect($h, [ref]$r) | Out-Null; return "$($r.L),$($r.T),$($r.R),$($r.B)" }
    function Real-Click($x, $y) { [Win.D]::SetCursorPos([int]$x, [int]$y) | Out-Null; Start-Sleep -Milliseconds 80; [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero) }
    $before = Win-Rect
    $sb = (Find $win "設定").Current.BoundingRectangle
    $cx = $sb.X + $sb.Width / 2; $cy = $sb.Y + $sb.Height / 2
    Real-Click $cx $cy; Real-Click $cx $cy      # two quick clicks, inside the double-click time
    Start-Sleep -Milliseconds 900
    Check ((Win-Rect) -eq $before) "titlebar: two quick clicks on the settings button do not maximize the window"
    Real-Click $cx $cy; Start-Sleep -Milliseconds 900
    Check ($null -ne (Find $win "設定分頁")) "titlebar: a single click on the settings button opens the settings"
    Real-Click $cx $cy; Start-Sleep -Milliseconds 900
    Check ($null -eq (Find $win "設定分頁")) "titlebar: and the next click closes them again"
    for ($i = 0; $i -lt 6; $i++) { Real-Click $cx $cy; Start-Sleep -Milliseconds 700 }   # slow clicks: every one must work (open, close, ...)
    Check ((Win-Rect) -eq $before) "titlebar: slow repeated clicks never maximize either"
    # the empty part of the strip is still the caption: a double-click there maximizes
    $wr = $win.Current.BoundingRectangle
    Real-Click ($wr.X + $wr.Width / 2) ($wr.Y + 14); Real-Click ($wr.X + $wr.Width / 2) ($wr.Y + 14); Start-Sleep -Milliseconds 900
    Check ((Win-Rect) -ne $before) "titlebar: double-clicking the empty strip still maximizes the window"
    Stop-Process -Id $p.Id -Force
    Reset-State
    }

    if (Want "paste") {
        Reset-State
    # ===== 32c. Ctrl+V: files and folders copied elsewhere are added to the new-archive page and to the archive on screen =====
    $ps = Join-Path $t "paste"; New-Item -ItemType Directory "$ps\src\sub", "$ps\arc" -Force | Out-Null
    Set-Content "$ps\src\one.txt" ("one " * 50); Set-Content "$ps\src\sub\inner.txt" ("inner " * 50); Set-Content "$ps\src\two.txt" ("two " * 50)
    $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
    # 1. the new-archive page: the hint is there, and a paste of a file + a folder adds two sources
    $p = Start-Raw @("--add", (Q "$ps\src\two.txt")); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "新增壓縮檔" 10)) "paste: the new-archive page is open"
    Start-Sleep -Milliseconds 1000
    Check (Has-Text $win "*Ctrl+V 貼上檔案*" 4) "paste: the page tells that files can be pasted with Ctrl+V"
    Set-Clipboard -Path "$ps\src\one.txt", "$ps\src\sub"
    [System.Windows.Forms.SendKeys]::SendWait("^v"); Start-Sleep -Milliseconds 1500
    Check (Has-Text $win "來源（3 項）" 6) "paste: a file and a folder were added to the sources (3 in all)"
    [System.Windows.Forms.SendKeys]::SendWait("^v"); Start-Sleep -Milliseconds 1000
    Check (Has-Text $win "來源（3 項）" 3) "paste: pasting the same things again adds nothing twice"
    # with the cursor in the name box the paste is the box's own: no sources are added
    Set-Clipboard -Path "$ps\src\sub\inner.txt"
    $nb = Find $win "檔名"; if ($nb) { $nb.SetFocus() }; Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait("^v"); Start-Sleep -Milliseconds 1200
    Check (Has-Text $win "來源（3 項）" 3) "paste: with the cursor in the name box nothing is added to the sources"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 2. an archive that can be changed: the paste goes into the folder that is shown
    Push-Location "$ps\src"; 7z a -tzip "$ps\arc\work.zip" ".\two.txt" | Out-Null; Pop-Location
    $p = Start-App "$ps\arc\work.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    Set-Clipboard -Path "$ps\src\one.txt", "$ps\src\sub"
    [System.Windows.Forms.SendKeys]::SendWait("^v"); Start-Sleep -Milliseconds 2500
    $end = (Get-Date).AddSeconds(20); $l = ""
    do { $l = (& $cliExe l "$ps\arc\work.zip" 2>&1 | Out-String); if (-not ($l.Contains("one.txt") -and $l.Contains("inner.txt"))) { Start-Sleep -Milliseconds 500 } } while (-not ($l.Contains("one.txt") -and $l.Contains("inner.txt")) -and (Get-Date) -lt $end)
    Check ($l.Contains("one.txt") -and $l.Contains("inner.txt")) "paste: the file and the folder (with its content) are in the archive"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "tplmenu") {
        Reset-State
    # ===== 32d. Which templates the right-click menu lists: a check list on the settings page, a switch in the template menu of the compress page =====
    $presetsFile = Join-Path $exeDir "presets.ini"
    Set-Content $presetsFile "[我的範本]`nformat=zip`n" -Encoding UTF8
    function Box-On($box) { $box.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On" }
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "設定"); Check ($null -ne (Wait-Find $win "同時執行的工作數")) "tplmenu: the settings page opens"
    Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 500
    $names = "一般 ZIP", "一般 7z", "最小體積", "快速", "寄信 / 分享", "備份", "僅儲存不壓縮", "我的範本"
    $want = $false, $false, $true, $false, $false, $true, $true, $true
    for ($i = 0; $i -lt $names.Count; $i++) {
        $e = Find $win $names[$i]
        Check (($null -ne $e) -and ((Box-On $e) -eq $want[$i])) "tplmenu: '$($names[$i])' is $(if ($want[$i]) { 'listed' } else { 'not listed' }) by default"
    }
    # switch one of the built-in ones on and the user's one off: written to settings.ini / presets.ini
    $e = Find $win "一般 ZIP"; if ($e) { Click $e }; Start-Sleep -Milliseconds 700
    $e = Find $win "我的範本"; if ($e) { Click $e }; Start-Sleep -Milliseconds 700
    Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "shell_presets=zip,smallest,backup,store") "tplmenu: settings.ini lists zip, smallest, backup, store"
    Check ((Get-Content $presetsFile -Raw -Encoding UTF8) -match "in_menu=false") "tplmenu: presets.ini says the user's template is hidden"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # the compress page: the template menu has the switch, and it shows the same state
    $src = Join-Path $t "tplsrc.txt"; Set-Content $src ("x" * 200)
    $p = Start-Raw @("--add", (Q $src)); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "新增壓縮檔" 10)) "tplmenu: the compress page opens"
    Start-Sleep -Milliseconds 1000
    Click (Find $win "範本選項"); Start-Sleep -Milliseconds 500
    $sw = Wait-Find $win "顯示在右鍵選單的「用範本壓縮」" 5
    Check ($null -ne $sw) "tplmenu: the template menu has the 'show in the right-click menu' switch"
    if ($sw) {
        Check (Box-On $sw) "tplmenu: the default template (general ZIP, switched on above) shows the switch as on"
        Click $sw; Start-Sleep -Milliseconds 700
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -notmatch "shell_presets=zip,") "tplmenu: switching it changes settings.ini"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $presetsFile -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "hash") {
        Reset-State
    # ===== 32e. Hash values: the dialog of the context menu (--hash), the values, comparing, copying, saving a checksum file; the archive itself from the menu =====
    $hs = Join-Path $t "hash"; New-Item -ItemType Directory $hs -Force | Out-Null
    [IO.File]::WriteAllText("$hs\abc.txt", "abc")
    $sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    $env:LYCAN_TEST_HASH_SAVE = "$hs\saved.sha256"
    Remove-Item $env:LYCAN_TEST_HASH_SAVE -ErrorAction SilentlyContinue
    $p = Start-Raw @("--hash", (Q "$hs\abc.txt")); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "貼上要比對的雜湊值" 15)) "hash: the dialog opens"
    Check (Has-Text $win "*$sha*" 10) "hash: the SHA-256 value of the file is shown"
    Check (Has-Text $win "*352441c2*" 3) "hash: the CRC-32 value is shown"
    Check (Has-Text $win "*900150983cd24fb0d6963f7d28e17f72*" 3) "hash: the MD5 value is shown"
    Check ($null -ne (Find $win "SHA-512")) "hash: SHA-512 can be chosen"
    # compare with a pasted value
    $box = Wait-Find $win "比對的雜湊值" 5
    Type-Into $box $sha.ToUpper()
    Check (Has-Text $win "✓ 相符*" 5) "hash: the pasted value (upper case) matches"
    Type-Into $box ("0" + $sha.Substring(1))
    Check (Has-Text $win "✗ 不相符*" 5) "hash: a wrong value does not match"
    Type-Into $box "xyz"
    Check (Has-Text $win "這不是雜湊值*" 5) "hash: text that is not a hash value is said so"
    Type-Into $box "1234"
    Check (Has-Text $win "長度（4 位）*" 5) "hash: a length that fits no algorithm is said so"
    Type-Into $box ""
    # another algorithm: starts again
    Click (Find $win "SHA-512"); Start-Sleep -Milliseconds 800
    Check (Has-Text $win "*ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f*" 8) "hash: switching SHA-512 on shows its value"
    # copy all and save as a checksum file
    Set-Clipboard -Value "before"
    Click (Find $win "複製全部"); Start-Sleep -Milliseconds 700
    $clip = Get-Clipboard -Raw
    Check (($clip -match "SHA256 \(abc.txt\) = $sha") -and ($clip -match "SHA512 \(abc.txt\) = ddaf35a1")) "hash: 'copy all' puts the lines on the clipboard (several algorithms: ALGO (name) = value)"
    Click (Find $win "儲存為 .sha256…"); Start-Sleep -Milliseconds 1200
    Check (Test-Path $env:LYCAN_TEST_HASH_SAVE) "hash: 'save as .sha256' wrote the file"
    if (Test-Path $env:LYCAN_TEST_HASH_SAVE) {
        $saved = [IO.File]::ReadAllText($env:LYCAN_TEST_HASH_SAVE)
        Check ($saved.Trim() -eq "$sha *abc.txt") "hash: the checksum file is 'value *name' (names relative to its folder)"
    }
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "hash: closing the dialog ends the window that was only its host"
    Remove-Item Env:LYCAN_TEST_HASH_SAVE
    # the archive file itself, from the menu of an open archive
    Push-Location $hs; 7z a -tzip "$hs\a.zip" ".\abc.txt" | Out-Null; Pop-Location
    $zipSha = (Get-FileHash "$hs\a.zip" -Algorithm SHA256).Hash.ToLower()
    $p = Start-App "$hs\a.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
    Click (Wait-Find $win "計算壓縮檔的雜湊值…" 5)
    Check (Has-Text $win "*$zipSha*" 12) "hash: the menu of an open archive hashes the archive file"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 600
    Check ($null -ne (Find $win "解壓縮到此")) "hash: the archive is still open after the dialog (the window stays)"
    # archive information: what the archive is (the menu item), and from there the hash values of the archive
    Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
    Click (Wait-Find $win "壓縮檔資訊…" 5)
    Check (Has-Text $win "1 個檔案、0 個資料夾" 8) "info: the archive information tells how many files and folders there are"
    Check (Has-Text $win "*ZIP*" 3) "info: it tells the format"
    Check ((Has-Text $win "*Deflate*" 3) -or (Has-Text $win "*Store*" 3)) "info: it tells the compression method"
    Check (Has-Text $win "是（可以加入、刪除、重新命名）" 3) "info: it tells that the archive can be changed"
    Check ($null -ne (Find $win "在檔案總管顯示")) "info: it offers to show the archive in Explorer"
    Click (Find $win "計算雜湊值…"); Start-Sleep -Milliseconds 800
    Check (Has-Text $win "*$zipSha*" 12) "info: 'hash values' opens the hash dialog of the archive"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 500
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "checksums") {
        Reset-State
    # ===== 32g. Checksum files: the hash values of a folder, --verify (one file, a changed and a missing file, several at once), the bar over an archive that has a checksum file next to it, writing checksum files with a new archive =====
    $cs = Join-Path $t "checksums"; Remove-Item $cs -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory "$cs\tree\sub" -Force | Out-Null
    [IO.File]::WriteAllText("$cs\tree\a.txt", "alpha"); [IO.File]::WriteAllText("$cs\tree\sub\b.txt", "bravo")
    $shaA = (Get-FileHash "$cs\tree\a.txt" -Algorithm SHA256).Hash.ToLower(); $shaB = (Get-FileHash "$cs\tree\sub\b.txt" -Algorithm SHA256).Hash.ToLower()
    # a folder: every file in it, the names start with the folder, saved beside it
    $env:LYCAN_TEST_HASH_SAVE = "$cs\tree.sha256"
    $p = Start-Raw @("--hash", (Q "$cs\tree")); $win = Get-Win $p
    Check (Has-Text $win "資料夾「tree」：2 個檔案" 15) "checksums: a folder is hashed with all the files in it"
    Check (Has-Text $win "*$shaA*" 10) "checksums: the value of a file in the folder is shown"
    Check (Has-Text $win "tree/sub/b.txt" 3) "checksums: the names of the files are shown from the folder (tree/sub/b.txt)"
    Click (Find $win "儲存為 .sha256…"); Start-Sleep -Milliseconds 1200
    $saved = if (Test-Path "$cs\tree.sha256") { [IO.File]::ReadAllText("$cs\tree.sha256") } else { "" }
    Check (($saved -match "$shaA \*tree/a.txt") -and ($saved -match "$shaB \*tree/sub/b.txt")) "checksums: the checksum file of a folder has the names from the folder, with '/'"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "checksums: closing the dialog ends the window"
    Remove-Item Env:LYCAN_TEST_HASH_SAVE
    # verify: everything matches
    $p = Start-Raw @("--verify", (Q "$cs\tree.sha256")); $win = Get-Win $p
    Check (Has-Text $win "✓ 全部 2 個檔案都相符。" 15) "checksums: --verify says that all the files match"
    Check ($null -eq (Find $win "不相符")) "checksums: nothing is listed when everything matches"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "checksums: the verify window ends with its dialog"
    # one changed, one missing: only those two are listed
    [IO.File]::WriteAllText("$cs\tree\a.txt", "CHANGED"); Rename-Item "$cs\tree\sub\b.txt" "b.hidden"
    $p = Start-Raw @("--verify", (Q "$cs\tree.sha256")); $win = Get-Win $p
    Check (Has-Text $win "✗ 2 個檔案中只有 0 個相符。" 15) "checksums: a changed and a missing file: 0 of 2 match"
    Check ((Has-Text $win "tree/a.txt" 3) -and (Has-Text $win "不相符" 3)) "checksums: the changed file is listed as not matching"
    Check ((Has-Text $win "tree/sub/b.txt" 3) -and (Has-Text $win "找不到" 3)) "checksums: the missing file is listed as not found"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "checksums: the verify window ends again"
    # several checksum files in one window (the other one is fine)
    Rename-Item "$cs\tree\sub\b.hidden" "b.txt"
    [IO.File]::WriteAllText("$cs\other.sha256", "$shaB *tree/sub/b.txt`n")
    $p = Start-Raw @("--verify", (Q "$cs\tree.sha256"), (Q "$cs\other.sha256")); $win = Get-Win $p
    Check (Has-Text $win "2 個校驗檔" 15) "checksums: two checksum files are checked in one window"
    Check (Has-Text $win "2 個校驗檔中有 1 個完全相符。" 5) "checksums: the summary says how many match completely"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "checksums: the window of several checksum files ends"
    # a lone checksum file given to the program is checked, not compressed
    $p = Start-Raw @((Q "$cs\other.sha256")); $win = Get-Win $p
    Check (Has-Text $win "✓ 全部 1 個檔案都相符。" 15) "checksums: a checksum file given to the program is verified"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 10) "checksums: that window ends too"

    # an archive with checksum files next to it: the bar, the hash dialog and the files inside
    [IO.File]::WriteAllText("$cs\tree\a.txt", "alpha")
    Push-Location $cs; 7z a -tzip "$cs\pk.zip" ".\tree" | Out-Null; Pop-Location
    $zipSha = (Get-FileHash "$cs\pk.zip" -Algorithm SHA256).Hash.ToLower()
    [IO.File]::WriteAllText("$cs\pk.zip.sha256", "$zipSha *pk.zip`n")
    [IO.File]::WriteAllText("$cs\pk.sha256", "$shaA *tree/a.txt`n$shaB *tree/sub/b.txt`n")
    $p = Start-App "$cs\pk.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Check (Has-Text $win "這個壓縮檔旁邊有校驗檔" 8) "checksums: opening an archive with checksum files next to it shows the bar"
    $linkA = Wait-Find $win "驗證壓縮檔" 5; $linkB = Find $win "驗證裡面的檔案"
    Check (($null -ne $linkA) -and ($null -ne $linkB)) "checksums: the bar offers both checksum files"
    Click $linkA
    Check (Has-Text $win "✓ 與同資料夾的 pk.zip.sha256 驗證相符。" 15) "checksums: the hash dialog says the archive matches the checksum file next to it"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 600
    Click (Find $win "驗證裡面的檔案")
    Check (Has-Text $win "✓ 全部 2 個檔案都相符。" 15) "checksums: the files inside the archive match the other checksum file"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 600
    # a wrong value
    [IO.File]::WriteAllText("$cs\pk.zip.sha256", ("0" * 64) + " *pk.zip`n")
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 500
    $p = Start-App "$cs\pk.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null
    Click (Wait-Find $win "驗證壓縮檔" 8)
    Check (Has-Text $win "✗ 與同資料夾的 pk.zip.sha256 不相符*" 15) "checksums: a wrong value in the checksum file is reported"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 500
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # no checksum file next to an archive: no bar
    Remove-Item "$cs\pk.zip.sha256", "$cs\pk.sha256"
    $p = Start-App "$cs\pk.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Check ($null -eq (Find $win "這個壓縮檔旁邊有校驗檔")) "checksums: no bar when there is no checksum file next to the archive"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the option: checksum files are written next to a new archive (the settings say so)
    Reset-State
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details`nsums_archive=true`nsums_files=true" -Encoding ascii
    New-Item -ItemType Directory "$cs\src2" -Force | Out-Null
    [IO.File]::WriteAllText("$cs\src2\one.txt", "one"); [IO.File]::WriteAllText("$cs\src2\two.txt", "two")
    $p = Start-Raw @("--compress", "zip", (Q "$cs\src2"))
    $ok = $false; for ($i = 0; $i -lt 60 -and -not $ok; $i++) { Start-Sleep -Milliseconds 500; $ok = (Test-Path "$cs\src2.zip.sha256") -and (Test-Path "$cs\src2.sha256") }
    Check $ok "checksums: with the options on, the new archive gets name.zip.sha256 and name.sha256 next to it"
    if ($ok) {
        $z = (Get-FileHash "$cs\src2.zip" -Algorithm SHA256).Hash.ToLower()
        Check (([IO.File]::ReadAllText("$cs\src2.zip.sha256")).Trim() -eq "$z *src2.zip") "checksums: name.zip.sha256 is the SHA-256 of the archive file"
        $inner = [IO.File]::ReadAllText("$cs\src2.sha256")
        $one = (Get-FileHash "$cs\src2\one.txt" -Algorithm SHA256).Hash.ToLower()
        Check ($inner -match "$one \*src2/one.txt") "checksums: name.sha256 lists the files under the names they have in the archive"
    }
    Check (Wait-Exit $p 20) "checksums: the compact window closes after the checksum files are written"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "firstframe") {
        Reset-State
    # ===== 32h. The first thing a window shows: a window that was started for a request (right-click action, checksum file) is created hidden and shown when it has
    # its screen, so the first visible frame has the shape of the request (the small window), never the home page that changes a moment later =====
    Add-Type -ErrorAction SilentlyContinue -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class FirstFrame {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    // the width of the first visible top-level window of the process (0 = none yet)
    public static int VisibleWidth(uint pid) {
        int found = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            var cn = new StringBuilder(128); GetClassName(h, cn, 128);
            if (!cn.ToString().Contains("WinUIDesktop")) return true;
            RECT r; GetWindowRect(h, out r);
            if (r.R - r.L > 50) found = r.R - r.L;
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
    function First-Width($argList) {
        Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 400
        $pr = Start-Raw $argList
        $w = 0; $end = (Get-Date).AddSeconds(8)
        while ($w -eq 0 -and (Get-Date) -lt $end) { $w = [FirstFrame]::VisibleWidth([uint32]$pr.Id); if ($w -eq 0) { Start-Sleep -Milliseconds 8 } }
        Stop-Process -Id $pr.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300
        return $w
    }
    $ff = Join-Path $t "firstframe"; New-Item -ItemType Directory $ff -Force | Out-Null
    [IO.File]::WriteAllText("$ff\a.txt", "abc"); [IO.File]::WriteAllText("$ff\a.sha256", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad *a.txt`n")
    $homeW = First-Width @("--bogus-option")        # a request that is not understood: the window shows at once (the home page, the normal size)
    Check ($homeW -gt 0) "first frame: a window for arguments that are not understood is shown at once ($homeW px wide)"
    $wHash = First-Width @("--hash", (Q "$ff\a.txt"))
    Check (($wHash -gt 0) -and ($wHash -lt $homeW)) "first frame: the window of a hash request is the small one from the start ($wHash px, the home page is $homeW px)"
    $wVerify = First-Width @("--verify", (Q "$ff\a.sha256"))
    Check (($wVerify -gt 0) -and ($wVerify -lt $homeW)) "first frame: the window of a verify request is the small one from the start ($wVerify px)"
    $wZip = First-Width @("--compress", "zip", (Q "$ff\a.txt"))
    Check (($wZip -gt 0) -and ($wZip -lt $homeW)) "first frame: the window of a compress request is the small one from the start ($wZip px)"
    Reset-State
    }

    if (Want "hashsave") {
        Reset-State
    # ===== 32i. "Calculate and save as name.sha256" (--hash-save): nothing is shown when it is quick; a window comes when it takes long or has something to ask =====
    Add-Type -ErrorAction SilentlyContinue -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class SilentProbe {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    public static bool AnyVisible(uint pid) {
        bool found = false;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            var cn = new StringBuilder(128); GetClassName(h, cn, 128);
            if (cn.ToString().Contains("WinUIDesktop")) found = true;
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
    # runs --hash-save and watches: did a window ever show, and when did the process end
    function Run-Silent($argList, [int]$sec = 30) {
        $pr = Start-Raw $argList
        $seen = $false; $end = (Get-Date).AddSeconds($sec)
        while (-not $pr.HasExited -and (Get-Date) -lt $end) { if ([SilentProbe]::AnyVisible([uint32]$pr.Id)) { $seen = $true }; Start-Sleep -Milliseconds 25 }
        $exited = $pr.HasExited
        if (-not $exited) { Stop-Process -Id $pr.Id -Force -ErrorAction SilentlyContinue }
        return [pscustomobject]@{ Seen = $seen; Exited = $exited }
    }
    $hsv = Join-Path $t "hashsave"; Remove-Item $hsv -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory "$hsv\tree\sub", "$hsv\many" -Force | Out-Null
    [IO.File]::WriteAllText("$hsv\tree\a.txt", "alpha"); [IO.File]::WriteAllText("$hsv\tree\sub\b.txt", "bravo"); [IO.File]::WriteAllText("$hsv\one.txt", "one")
    [IO.File]::WriteAllText("$hsv\many\x.txt", "x"); [IO.File]::WriteAllText("$hsv\many\y.txt", "y")
    function Sha($f) { (Get-FileHash $f -Algorithm SHA256).Hash.ToLower() }
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details`nnotifications=false" -Encoding ascii

    # a folder: the file is beside it, the names start with the folder; no window at all
    $r = Run-Silent @("--hash-save", (Q "$hsv\tree"))
    Check $r.Exited "hashsave: the program ends by itself"
    Check (-not $r.Seen) "hashsave: no window was ever shown when it was quick"
    $s = if (Test-Path "$hsv\tree.sha256") { [IO.File]::ReadAllText("$hsv\tree.sha256") } else { "" }
    Check (($s -match ((Sha "$hsv\tree\a.txt") + " \*tree/a.txt")) -and ($s -match ((Sha "$hsv\tree\sub\b.txt") + " \*tree/sub/b.txt"))) "hashsave: a folder -> name.sha256 beside it, with the names from the folder"
    # one file: name.ext.sha256 beside it
    $r = Run-Silent @("--hash-save", (Q "$hsv\one.txt"))
    $s = if (Test-Path "$hsv\one.txt.sha256") { [IO.File]::ReadAllText("$hsv\one.txt.sha256").Trim() } else { "" }
    Check ((-not $r.Seen) -and ($s -eq ((Sha "$hsv\one.txt") + " *one.txt"))) "hashsave: one file -> one.txt.sha256 beside it"
    # several items: named after the folder they are in, saved there
    $r = Run-Silent @("--hash-save", (Q "$hsv\many\x.txt"), (Q "$hsv\many\y.txt"))
    $s = if (Test-Path "$hsv\many\many.sha256") { [IO.File]::ReadAllText("$hsv\many\many.sha256") } else { "" }
    Check ((-not $r.Seen) -and ($s -match "\*x.txt") -and ($s -match "\*y.txt")) "hashsave: several items -> many.sha256 in the folder they are in"

    # the file is there already: the question, keep both
    $before = [IO.File]::ReadAllText("$hsv\tree.sha256")
    [IO.File]::WriteAllText("$hsv\tree.sha256", "old content`n")
    $p = Start-Raw @("--hash-save", (Q "$hsv\tree")); $win = Get-Win $p
    Check (Has-Text $win "*已經存在*" 15) "hashsave: a file that is there already is asked about"
    Click (Find $win "兩個都保留")
    Check (Wait-Exit $p 20) "hashsave: the program ends after the answer"
    Check (([IO.File]::ReadAllText("$hsv\tree.sha256")) -eq "old content`n") "hashsave: keep both leaves the old file alone"
    $others = @(Get-ChildItem $hsv -Filter "tree*.sha256" | Where-Object { $_.Name -ne "tree.sha256" })
    Check (($others.Count -eq 1) -and ([IO.File]::ReadAllText($others[0].FullName) -eq $before)) "hashsave: ... and the new one has another name ($($others[0].Name))"
    # replace
    $p = Start-Raw @("--hash-save", (Q "$hsv\tree")); $win = Get-Win $p
    Check (Has-Text $win "*已經存在*" 15) "hashsave: asked again"
    Click (Find $win "取代")
    Check (Wait-Exit $p 20) "hashsave: the program ends after 'replace'"
    Check (([IO.File]::ReadAllText("$hsv\tree.sha256")) -eq $before) "hashsave: replace writes the new content over the old file"
    # cancel in the question
    [IO.File]::WriteAllText("$hsv\tree.sha256", "keep me`n")
    $p = Start-Raw @("--hash-save", (Q "$hsv\tree")); $win = Get-Win $p
    Check (Has-Text $win "*已經存在*" 15) "hashsave: asked a third time"
    Click (Find $win "取消")
    Check ((Wait-Exit $p 20) -and (([IO.File]::ReadAllText("$hsv\tree.sha256")) -eq "keep me`n")) "hashsave: cancelling the question changes nothing"

    # it takes long: the window comes with the progress, then the result
    Remove-Item "$hsv\tree*.sha256" -Force
    $env:LYCAN_TEST_HASH_DELAY = "3000"
    $p = Start-Raw @("--hash-save", (Q "$hsv\tree")); $win = Get-Win $p
    Check (Has-Text $win "計算並儲存校驗檔" 15) "hashsave: a slow calculation shows its window"
    Check (Has-Text $win "✓ 已儲存：*" 15) "hashsave: ... and says that it is saved"
    Check ($null -ne (Find $win "在檔案總管顯示")) "hashsave: ... with a button to show it"
    Check (Test-Path "$hsv\tree.sha256") "hashsave: the file is there"
    Click (Find $win "關閉")
    Check (Wait-Exit $p 15) "hashsave: closing the window ends the program"
    # cancel while it runs: nothing is left
    Remove-Item "$hsv\tree.sha256" -Force
    $env:LYCAN_TEST_HASH_DELAY = "20000"
    $p = Start-Raw @("--hash-save", (Q "$hsv\tree")); $win = Get-Win $p
    Check (Has-Text $win "計算並儲存校驗檔" 15) "hashsave: a slow calculation shows its window (to cancel)"
    Click (Find $win "取消")
    Check (Wait-Exit $p 15) "hashsave: cancelling ends the program"
    Check (-not (Test-Path "$hsv\tree.sha256")) "hashsave: cancelling leaves no file"
    Remove-Item Env:LYCAN_TEST_HASH_DELAY

    # with the notifications on (the default) the program waits for the notification and ends
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details" -Encoding ascii
    Remove-Item "$hsv\one.txt.sha256" -Force
    $r = Run-Silent @("--hash-save", (Q "$hsv\one.txt")) 40
    Check ($r.Exited -and (-not $r.Seen) -and (Test-Path "$hsv\one.txt.sha256")) "hashsave: with a notification it still shows no window and ends by itself"
    Reset-State
    }

    if (Want "feedbacka") {
        Reset-State
    # ===== 34. Batch A of the feedback: the note when the pictures go round, the preview menu, the checksum settings of a template, the Home button and the extract-to drop-down, paste, the Night accent, the audio mode button =====
    $fa = Join-Path $t "feedbacka"; Remove-Item $fa -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory "$fa\src" -Force | Out-Null
    foreach ($spec in @(@("one.png", 64), @("two.png", 128))) {
        $bmp = New-Object System.Drawing.Bitmap $spec[1], 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
        $bmp.Save("$fa\src\$($spec[0])", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Push-Location "$fa\src"; 7z a -tzip "$fa\pics.zip" ".\one.png" ".\two.png" | Out-Null; Pop-Location
    function Top-Wins($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); @($AE::RootElement.FindAll($TS::Children, $c)) }
    function Wait-Win($proc, [string]$like, [int]$sec = 15) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (Top-Wins $proc)) { if ($w.Current.Name -like $like) { return $w } }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
        return $null
    }
    function Find-Menu($proc, [string]$name) {   # a menu is not always under the window: search the whole tree of the process
        $c = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)), (ByName $name))
        return $AE::RootElement.FindFirst($TS::Descendants, $c)
    }
    function Find-MenuLike($proc, [string]$like) {   # a long path in a menu is cut in the middle: find it by its end
        $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
        foreach ($e in $AE::RootElement.FindAll($TS::Descendants, $c)) { try { if ($e.Current.Name -like $like) { return $e } } catch { } }
        return $null
    }
    function Click-Center($w) { $r = $w.Current.BoundingRectangle; [Mouse.M]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 150; [Mouse.M]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Mouse.M]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300 }

    # the pictures go round: the note says where we are; the menu of a preview that has no main window cannot open with the associated program
    $p = Start-Raw @("--preview-images", (Q "$fa\pics.zip"))
    $pw = Wait-Win $p "*.png*"
    Check ($null -ne $pw) "feedback A: the preview window opens"
    if ($pw) {
        Wait-Find $pw "1 / 2" 8 | Out-Null
        Click-Center $pw   # the focus on the picture (in the list of thumbnails the arrow keys belong to the list)
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 700
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
        Check (Has-Text $pw "這是第一張圖片" 5) "feedback A: going on after the last picture says that this is the first one"
        Start-Sleep -Milliseconds 1800
        Check (-not (Has-Text $pw "這是第一張圖片" 1)) "feedback A: the note goes away by itself"
        [System.Windows.Forms.SendKeys]::SendWait("{LEFT}")
        Check (Has-Text $pw "這是最後一張圖片" 5) "feedback A: going back before the first picture says that this is the last one"
        Click (Find $pw "選單"); Start-Sleep -Milliseconds 600
        $mi = Find-Menu $p "以關聯程式開啟"
        Check (($null -ne $mi) -and (-not $mi.Current.IsEnabled)) "feedback A: 'open with the associated program' is greyed out when the preview was started from a file"
        $mi2 = Find-Menu $p "複製圖片"
        Check (($null -ne $mi2) -and $mi2.Current.IsEnabled) "feedback A: the other entries of the menu are on"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
        Wait-Exit $p 8 | Out-Null
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the same menu from an archive that is open in the main window: the entry is on
    $p = Start-App "$fa\pics.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    [System.Windows.Forms.SendKeys]::SendWait("%p")
    $pw = Wait-Win $p "*.png*"
    if ($pw) {
        Click (Find $pw "選單"); Start-Sleep -Milliseconds 600
        $mi = Find-Menu $p "以關聯程式開啟"
        Check (($null -ne $mi) -and $mi.Current.IsEnabled) "feedback A: from the main window the entry 'open with the associated program' is on"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300
    } else { Check $false "feedback A: the preview opens from the main window (Alt+P)" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the Home button sits left of "up", the first row starts with the extract button; "extract to" has a drop-down with the folders that were used
    Reset-State
    $p = Start-App "$fa\pics.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $home1 = Find $win "首頁"; $up1 = Find $win "上一層"
    Check (($null -ne $home1) -and ($null -ne $up1) -and ($home1.Current.BoundingRectangle.X -lt $up1.Current.BoundingRectangle.X)) "feedback A: the Home button is left of the Up button"
    Check ($null -eq (Find $win "返回")) "feedback A: the old back arrow is gone from the archive view"
    $ext1 = Find $win "解壓縮到此"
    Check (($null -ne $ext1) -and ($ext1.Current.BoundingRectangle.X -lt $up1.Current.BoundingRectangle.X + 200)) "feedback A: the first row starts with the extract button"
    Click $home1
    Check ($null -ne (Wait-Find $win "開啟壓縮檔" 6)) "feedback A: the Home button goes back to the home page"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the drop-down of "extract to": recent folders (from settings.ini) and the folders of Windows
    Reset-State
    $recent = Join-Path $fa "recent folder"; New-Item -ItemType Directory $recent -Force | Out-Null
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details`nrecent_extract_dirs=$recent" -Encoding UTF8
    $p = Start-App "$fa\pics.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $sb = $null
    foreach ($e in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName "解壓縮到…"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::SplitButton))))) ) { $sb = $e; break }
    if ($sb) {
        $r = $sb.Current.BoundingRectangle
        # the arrow part of the split button is on the right
        [Mouse.M]::SetCursorPos([int]($r.X + $r.Width - 10), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 200
        [Mouse.M]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Mouse.M]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
        $it = Find-MenuLike $p "*recent folder"
        Check ($null -ne $it) "feedback A: the drop-down of 'extract to' lists the folder that was used before"
        Check (($null -ne (Find-Menu $p "桌面")) -and ($null -ne (Find-Menu $p "下載"))) "feedback A: ... and the folders of Windows (Desktop, Downloads)"
        if ($it) {
            Click $it
            $ok = $false; for ($i = 0; $i -lt 30 -and -not $ok; $i++) { Start-Sleep -Milliseconds 500; $ok = @(Get-ChildItem $recent -Recurse -Filter "*.png" -ErrorAction SilentlyContinue).Count -eq 2 }   # (the automatic mode puts a folder of the archive's name around the two files)
            Check $ok "feedback A: choosing a folder in the drop-down extracts there at once"
        }
    } else { Check $false "feedback A: the extract-to button is there" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # a template carries the two checksum settings
    Reset-State
    $p = Start-Raw @("--add", (Q "$fa\src\one.png")); $w2 = Get-Win $p
    Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
    function Toggle-On($w, $name) {   # a switch is a button of the tree (its caption is a text with the same name)
        $e = $w.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))))
        if (-not $e) { return $null }
        return $e.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On"
    }
    Select-Combo $w2 "範本" "一般 ZIP"; Start-Sleep -Milliseconds 400
    Check (((Toggle-On $w2 "產生新壓縮檔的校驗檔") -eq $false) -and ((Toggle-On $w2 "產生放入檔案的校驗檔") -eq $false)) "feedback A: the template 'general ZIP' has no checksum files"
    Select-Combo $w2 "範本" "備份"; Start-Sleep -Milliseconds 500
    Check (((Toggle-On $w2 "產生新壓縮檔的校驗檔") -eq $true) -and ((Toggle-On $w2 "產生放入檔案的校驗檔") -eq $true)) "feedback A: the template 'backup' asks for both checksum files"
    Select-Combo $w2 "範本" "寄信 / 分享"; Start-Sleep -Milliseconds 500
    Check (((Toggle-On $w2 "產生新壓縮檔的校驗檔") -eq $true) -and ((Toggle-On $w2 "產生放入檔案的校驗檔") -eq $false)) "feedback A: the template 'e-mail / share' asks for the one of the archive (every volume)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the right-click compress with a template follows the template (the backup template makes both files); without a template the settings say
    Reset-State
    New-Item -ItemType Directory "$fa\tplsrc" -Force | Out-Null; [IO.File]::WriteAllText("$fa\tplsrc\a.txt", "a"); [IO.File]::WriteAllText("$fa\tplsrc\b.txt", "b")
    $p = Start-Raw @("--compress", "zip", "--template", (Q "備份"), (Q "$fa\tplsrc"))
    $ok = $false; for ($i = 0; $i -lt 60 -and -not $ok; $i++) { Start-Sleep -Milliseconds 500; $ok = (Test-Path "$fa\tplsrc.7z.sha256") -and (Test-Path "$fa\tplsrc.sha256") }
    Check $ok "feedback A: the right-click 'compress with a template' (backup) writes both checksum files"
    Wait-Exit $p 20 | Out-Null; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item "$fa\tplsrc.*" -Force -ErrorAction SilentlyContinue
    $p = Start-Raw @("--compress", "zip", (Q "$fa\tplsrc"))
    Wait-Exit $p 30 | Out-Null; Start-Sleep -Milliseconds 500
    Check ((Test-Path "$fa\tplsrc.zip") -and (-not (Test-Path "$fa\tplsrc.zip.sha256"))) "feedback A: without a template and with the settings off no checksum file is written"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the title shows the percentage while a task runs, and the Night accent
    Reset-State
    $env:LYCAN_TEST_HASH_DELAY = ""
    Remove-Item Env:LYCAN_TEST_HASH_DELAY -ErrorAction SilentlyContinue
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true`nview_mode=details`naccent=night" -Encoding ascii
    $p = Start-App "$fa\pics.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Check ($null -ne $win) "feedback A: the Night accent starts without trouble"
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Check ($null -ne (Wait-Find $win "黑夜" 5)) "feedback A: the accent 'Night' is in the settings"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # the audio window: the playback mode button cycles and the setting is saved
    Reset-State
    $rate = 8000; $n = $rate
    $hdr = [System.Collections.Generic.List[byte]]::new()
    foreach ($part in @([Text.Encoding]::ASCII.GetBytes("RIFF"), [BitConverter]::GetBytes([int](36 + $n)), [Text.Encoding]::ASCII.GetBytes("WAVEfmt "), [BitConverter]::GetBytes([int]16), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int]$rate), [BitConverter]::GetBytes([int]$rate), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int16]8), [Text.Encoding]::ASCII.GetBytes("data"), [BitConverter]::GetBytes([int]$n))) { $hdr.AddRange([byte[]]$part) }
    [IO.File]::WriteAllBytes("$fa\src\song1.wav", ($hdr.ToArray() + ([byte[]](, 128 * $n)))); [IO.File]::WriteAllBytes("$fa\src\song2.wav", ($hdr.ToArray() + ([byte[]](, 128 * $n))))
    Push-Location "$fa\src"; 7z a -tzip "$fa\songs.zip" ".\song1.wav" ".\song2.wav" | Out-Null; Pop-Location
    $env:LYCAN_TEST_MUTE = "1"
    $p = Start-Raw @("--preview-audio", (Q "$fa\songs.zip"))
    $aw = Wait-Win $p "音訊預覽*"
    Check ($null -ne $aw) "feedback A: the player window opens"
    if ($aw) {
        $lb = Find $aw "播放模式：依序播放，播完停止（L）"
        Check ($null -ne $lb) "feedback A: the player has a playback mode button (in order, then stop)"
        [System.Windows.Forms.SendKeys]::SendWait("l"); Start-Sleep -Milliseconds 500
        Check ($null -ne (Find $aw "播放模式：全部循環（L）")) "feedback A: the key L goes to 'repeat all'"
        [System.Windows.Forms.SendKeys]::SendWait("l"); Start-Sleep -Milliseconds 400
        Check ($null -ne (Find $aw "播放模式：單首循環（L）")) "feedback A: ... then 'repeat this track'"
        [System.Windows.Forms.SendKeys]::SendWait("l"); Start-Sleep -Milliseconds 400
        Check ($null -ne (Find $aw "播放模式：隨機播放（L）")) "feedback A: ... then 'shuffle'"
        Check ((Get-Content $settingsFile -Raw) -match "audio_loop=shuffle") "feedback A: the mode is saved in the settings"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
        Wait-Exit $p 8 | Out-Null
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # a task that runs a while: the percentage is in the title of the window (the compact window of a right-click action too)
    Reset-State
    New-Item -ItemType Directory "$fa\big" -Force | Out-Null
    $rnd = New-Object System.Random 5; $buf = New-Object byte[] (1MB)
    $fs = [IO.File]::Create("$fa\big\random.bin"); for ($i = 0; $i -lt 160; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close()
    $p = Start-Raw @("--compress", "zip", (Q "$fa\big"))
    $sawPct = $false; $end = (Get-Date).AddSeconds(60)
    while (-not $p.HasExited -and (Get-Date) -lt $end -and -not $sawPct) { $p.Refresh(); if ($p.MainWindowTitle -match "^\d+% - ") { $sawPct = $true }; Start-Sleep -Milliseconds 60 }
    Check $sawPct "feedback A: while a compression runs the title of the (compact) window starts with the percentage"
    Wait-Exit $p 60 | Out-Null; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item "$fa\big*" -Recurse -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "diaglog") {
        Reset-State
    # ===== 32f. The diagnostic log: written to the folder (LYCAN_LOG_DIR here), the switches in the settings, 'copy diagnostic information' =====
    $lg = Join-Path $t "logs"; Remove-Item $lg -Recurse -Force -ErrorAction SilentlyContinue
    $env:LYCAN_LOG_DIR = $lg
    function Toggle-Is($el) { $el.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On" }
    # the detailed log forced by the environment: what was opened is in the file
    $env:LYCAN_LOG = "debug"
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_LOG
    Start-Sleep -Milliseconds 300
    $logFile = Join-Path $lg "lycanzip.log"
    Check (Test-Path $logFile) "diag log: the log file is written to the log folder"
    $logText = if (Test-Path $logFile) { [IO.File]::ReadAllText($logFile) } else { "" }
    Check ($logText -match " app\] INFO  start: LycanZip ") "diag log: the start of the program is in it (the process is 'app')"
    Check ($logText -match "opened sample.zip: ZIP, \d+ item\(s\), \d+ ms") "diag log: opening the archive is in it (the file name, the format, how long)"
    Check ($logText -notmatch [regex]::Escape($t)) "diag log: no path of the test folder (names only)"
    # the switches in the settings (the About tab)
    Remove-Item $lg -Recurse -Force -ErrorAction SilentlyContinue
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "設定"); Check ($null -ne (Wait-Find $win "同時執行的工作數" 8)) "diag log: the settings page opens"
    Open-SettingsTab $win "關於"; Start-Sleep -Milliseconds 500
    function Find-Switch($name) { $win.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button))))) }
    $dt = Find-Switch "記錄詳細的偵錯訊息"; $pt = Find-Switch "記錄完整路徑"
    Check (($null -ne $dt) -and ($null -ne $pt)) "diag log: the About tab has the two switches"
    Check ((-not (Toggle-Is $dt)) -and (-not (Toggle-Is $pt))) "diag log: both are off by default"
    # the data location (portable here: the test puts the flag next to the exe); the switch itself is not pressed (it restarts the program)
    Check (Has-Text $win "可攜式模式：設定、範本、當機報告與記錄都儲存在程式資料夾*" 3) "data location: the About tab says that the mode is portable (the flag is next to the exe)"
    Check (Has-Text $win "設定檔所在的資料夾：*" 3) "data location: it shows the folder of the settings"
    Check ($null -ne (Find $win "切換為一般模式（儲存在使用者資料夾）")) "data location: it offers to switch to the user folder"
    Check (($null -ne (Find $win "開啟記錄資料夾")) -and ($null -ne (Find $win "複製診斷資訊"))) "diag log: it has 'open the log folder' and 'copy diagnostic information'"
    Click $dt; Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "debug_log=true") "diag log: switching the detailed log on is saved (debug_log=true)"
    Check ((Test-Path $logFile) -and ([IO.File]::ReadAllText($logFile) -match "the detailed log was switched on")) "diag log: the switch itself is in the log"
    Set-Clipboard -Value "before"
    Click (Find $win "複製診斷資訊"); Start-Sleep -Milliseconds 600
    $clip = Get-Clipboard -Raw
    Check (($clip -match "^LycanZip \d") -and ($clip -match "Windows \d+\.\d+\.\d+") -and ($clip -match "7z.dll:") -and ($clip -match "Log folder: ") -and ($clip -match "Detailed log: on")) "diag log: 'copy diagnostic information' has the version, Windows, the engine, the switches and the folder"
    Check ($clip -match "the detailed log was switched on") "diag log: and the last lines of the log"
    Check (Has-Text $win "已複製到剪貼簿" 3) "diag log: it says it was copied"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_LOG_DIR
    Reset-State
    }

    if (Want "win11menu") {
        Reset-State
    # ===== 32g. The Windows 11 context menu switch (settings > Explorer): the sparse package is registered and removed =====
    # Needs Developer Mode (the test registers the unsigned loose manifest that the release flow makes, LYCAN_WIN11_MENU_MANIFEST) and the package
    # is removed again at the end. Registering touches the current user's packages only.
    $w11Manifest = Join-Path $PSScriptRoot "..\build\shellext-pkg\stage\AppxManifest.xml"
    $devMode = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense -eq 1
    function Toggle-Is2($el) { $el.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On" }
    Get-AppxPackage -Name LycanZip.ShellExtension | Remove-AppxPackage   # (a package registered by hand for looking at the menu would make the first checks wrong)
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Click (Find $win "設定"); Wait-Find $win "同時執行的工作數" 8 | Out-Null
    Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 800
    function Find-W11Switch { $win.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName "在 Windows 11 新式右鍵選單加入 LycanZip"), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))) ) }
    $sw = Find-W11Switch
    Check ($null -ne (Find $win "在右鍵選單加入 LycanZip")) "win11 menu: the classic switch is there"
    Check ($null -eq $sw -or $sw.Current.IsOffscreen) "win11 menu: without a menu package (and without the test hook) its card is not shown (it could never be switched on)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    if ($devMode -and (Test-Path $w11Manifest)) {
        Get-AppxPackage -Name LycanZip.ShellExtension | Remove-AppxPackage
        $env:LYCAN_WIN11_MENU_MANIFEST = (Resolve-Path $w11Manifest).Path
        $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
        Click (Find $win "設定"); Wait-Find $win "同時執行的工作數" 8 | Out-Null
        Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 800
        $sw = Find-W11Switch
        Check ($null -ne $sw) "win11 menu: with a package available the settings page has its own switch (next to the classic one, not merged)"
        Check (Has-Text $win "未啟用。" 4) "win11 menu: with a package available the status says it is off"
        Check ($sw.Current.IsEnabled -and -not (Toggle-Is2 $sw)) "win11 menu: and the switch can be switched on"
        Click $sw; Start-Sleep -Seconds 4
        Check ($null -ne (Wait-Find $win "已加入" 8)) "win11 menu: switching it on says it was added"
        $ok = Find $win "確定"; if ($ok) { Click $ok }
        Start-Sleep -Milliseconds 1200
        Check ($null -ne (Get-AppxPackage -Name LycanZip.ShellExtension)) "win11 menu: the package LycanZip.ShellExtension is registered for the user"
        Check (Has-Text $win "已啟用。" 6) "win11 menu: the status says it is on"
        $sw = Find-W11Switch; Check (Toggle-Is2 $sw) "win11 menu: the switch is on"
        Click $sw; Start-Sleep -Seconds 4
        Check ($null -eq (Get-AppxPackage -Name LycanZip.ShellExtension)) "win11 menu: switching it off removes the package"
        Check (Has-Text $win "未啟用。" 6) "win11 menu: the status says it is off again"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Remove-Item Env:LYCAN_WIN11_MENU_MANIFEST
        Get-AppxPackage -Name LycanZip.ShellExtension | Remove-AppxPackage
    } else { Write-Host "skip: win11 menu registration (needs Developer Mode and build\shellext-pkg\stage\AppxManifest.xml, made by the release flow)" }
    Reset-State
    }

    if (Want "stalepath") {
        Reset-State
    # ===== 32h. The menus and file associations point to another folder (a moved portable folder): the program says so and offers to fix it =====
    # The registrations of the test live under HKCU\Software\LycanZipTest_stale (LYCAN_TEST_REG_ROOT); the real ones are not touched.
    $sroot = "Software\LycanZipTest_stale"
    $clsid = "{5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A20}"
    function Seed-Stale {
        Remove-Item "HKCU:\$sroot" -Recurse -Force -ErrorAction SilentlyContinue
        function Set-Reg($sub, $name, $value) { $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($sub); $k.SetValue($name, $value); $k.Close() }   # (reg.exe mangles the quotes)
        Set-Reg "$sroot\Classes\*\shell\LycanZipFiles" "ExplorerCommandHandler" $clsid
        Set-Reg "$sroot\Classes\CLSID\$clsid\InprocServer32" "" "C:\OldPlace\LycanZipShell.dll"
        Set-Reg "$sroot\Classes\LycanZip.zip\shell\open\command" "" "`"C:\OldPlace\LycanZip.exe`" `"%1`""
        Set-Reg "$sroot\Software\LycanZip\Capabilities" "ApplicationName" "LycanZip"
    }
    function Reg-Default($key) { (Get-ItemProperty -LiteralPath "Registry::HKEY_CURRENT_USER\$key" -ErrorAction SilentlyContinue).'(default)' }
    $env:LYCAN_TEST_REG_ROOT = $sroot
    Remove-Item Env:LYCAN_NO_STALE_CHECK
    $title = "選單指向另一份 LycanZip"
    $dllNow = Join-Path (Split-Path $exe -Parent) ("LycanZipShell-" + (Get-Content (Join-Path $PSScriptRoot "..\VERSION") -Raw).Trim() + ".dll")   # the version is in the name of the menu DLL
    # 1) nothing registered: no prompt
    Remove-Item "HKCU:\$sroot" -Recurse -Force -ErrorAction SilentlyContinue
    $p = Start-App $null; $win = Get-Win $p; Start-Sleep -Seconds 4
    Check ($null -eq (Find $win $title)) "stale path: with nothing registered there is no prompt"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 2) registered for another folder: the prompt names the menus and the file associations; 'later' changes nothing
    Seed-Stale
    $p = Start-App $null; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win $title 12)) "stale path: the prompt appears when the registrations point to another folder"
    Check (Has-Text $win "右鍵選單、檔案關聯目前指向另一份 LycanZip*" 3) "stale path: it names the right-click menu and the file associations"
    Check (Has-Text $win "*目前使用：C:\OldPlace*" 3) "stale path: it says where they point now"
    Click (Find $win "稍後"); Start-Sleep -Milliseconds 600
    Check ((Reg-Default "$sroot\Classes\CLSID\$clsid\InprocServer32") -eq "C:\OldPlace\LycanZipShell.dll") "stale path: 'later' registers nothing"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 3) 'use the current folder' points both to this program
    $p = Start-App $null; $win = Get-Win $p
    Wait-Find $win $title 12 | Out-Null; Click (Find $win "改用這一份")
    Check ($null -ne (Wait-Find $win "已更新" 8)) "stale path: after the click it says it was updated"
    Click (Find $win "確定"); Start-Sleep -Milliseconds 600
    Check ((Reg-Default "$sroot\Classes\CLSID\$clsid\InprocServer32") -eq $dllNow) "stale path: the menu now points to the DLL next to this exe"
    Check ((Reg-Default "$sroot\Classes\LycanZip.zip\shell\open\command") -eq "`"$exe`" `"%1`"") "stale path: the file associations point to this exe"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p = Start-App $null; $win = Get-Win $p; Start-Sleep -Seconds 4
    Check ($null -eq (Find $win $title)) "stale path: once they point here the prompt is gone"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 4) 'do not remind me again' is remembered for this exe
    Seed-Stale
    $p = Start-App $null; $win = Get-Win $p
    Wait-Find $win $title 12 | Out-Null; Click (Find $win "不再提醒"); Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "stale_menu_ignored=") "stale path: 'do not remind me again' is saved (stale_menu_ignored)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p = Start-App $null; $win = Get-Win $p; Start-Sleep -Seconds 4
    Check ($null -eq (Find $win $title)) "stale path: and the prompt does not come back"
    Check ((Reg-Default "$sroot\Classes\CLSID\$clsid\InprocServer32") -eq "C:\OldPlace\LycanZipShell.dll") "stale path: nothing was registered behind the user's back"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 5) an update in the same folder (the menu DLL of the older version is registered): no question, it is pointed to the new DLL, the old DLLs are removed,
    #    and a notice offers to restart Explorer (the test only looks at it and does not click it)
    $exeDir = Split-Path $exe -Parent
    Remove-Item "HKCU:\$sroot" -Recurse -Force -ErrorAction SilentlyContinue
    function Set-Reg5($sub, $name, $value) { $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($sub); $k.SetValue($name, $value); $k.Close() }
    Set-Reg5 "$sroot\Classes\*\shell\LycanZipFiles" "ExplorerCommandHandler" $clsid
    Set-Reg5 "$sroot\Classes\CLSID\$clsid\InprocServer32" "" (Join-Path $exeDir "LycanZipShell-0.0.1.dll")
    Copy-Item $dllNow (Join-Path $exeDir "LycanZipShell-0.0.1.dll") -Force
    $p = Start-App $null; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "右鍵選單已更新" 12)) "stale path: after an update in the same folder a notice says that the menu was updated"
    Check ($null -ne (Find $win "重新啟動檔案總管")) "stale path: and it offers to restart File Explorer"
    Check ($null -eq (Find $win $title)) "stale path: the same folder is not asked about"
    Check ((Reg-Default "$sroot\Classes\CLSID\$clsid\InprocServer32") -eq $dllNow) "stale path: the menu points to the DLL of this version now"
    Start-Sleep -Milliseconds 500
    Check (-not (Test-Path (Join-Path $exeDir "LycanZipShell-0.0.1.dll"))) "stale path: the menu DLL of the other version was removed"
    Check (Test-Path $dllNow) "stale path: the DLL of this version is still there"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item "HKCU:\$sroot" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_REG_ROOT; $env:LYCAN_NO_STALE_CHECK = "1"
    Reset-State
    }

    if (Want "compactdone") {
        Reset-State
        Remove-Item -LiteralPath $settingsFile -Force   # the defaults: the compact window does not close by itself
    # ===== 32i. The compact window (right-click actions): the Done button, a fixed size, stays open by default =====
    Add-Type -Namespace N4 -Name Style -ErrorAction SilentlyContinue -MemberDefinition '[DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);'
    New-Item -ItemType Directory "$t\cd1" -Force | Out-Null; Copy-Item "$t\sample.zip" "$t\cd1\sample.zip"
    $p = Start-Raw @("--extract-here", (Q "$t\cd1\sample.zip")); $win = Get-Win $p
    $done = Wait-Find $win "完成" 20
    Check ($null -ne $done) "compact done: after a successful extraction the window shows a 'Done' button"
    Check (Test-Path "$t\cd1\sample") "compact done: the files were extracted"
    Start-Sleep -Seconds 3
    Check (-not $p.HasExited) "compact done: and it does not close by itself (the default)"
    $p.Refresh(); $style = [N4.Style]::GetWindowLong($p.MainWindowHandle, -16)
    Check ((($style -band 0x40000) -eq 0) -and (($style -band 0x10000) -eq 0)) "compact done: the window cannot be resized or maximized (no sizing border, no maximize button)"
    Check (($style -band 0x20000) -ne 0) "compact done: but it can still be minimized"
    $done = Find $win "完成"
    Check ($done.Current.HasKeyboardFocus) "compact done: the button has the keyboard focus (Enter presses it)"
    Click $done
    Check (Wait-Exit $p 10) "compact done: clicking 'Done' closes the window"
    # Esc closes it as well
    Remove-Item "$t\cd1\sample" -Recurse -Force
    $p = Start-Raw @("--extract-here", (Q "$t\cd1\sample.zip")); $win = Get-Win $p
    Wait-Find $win "完成" 20 | Out-Null; Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Check (Wait-Exit $p 10) "compact done: Esc closes the finished window"
    # the option 'close the window' (off by default) still closes it by itself
    Remove-Item "$t\cd1\sample" -Recurse -Force
    Set-Content -Path $settingsFile -Value "compact_close_when_done=true" -Encoding ascii
    $p = Start-Raw @("--extract-here", (Q "$t\cd1\sample.zip"))
    Check (Wait-Exit $p 20) "compact done: with 'close the window' on it still closes by itself"
    Reset-State
    }

    if (Want "dataafterend") {
        Reset-State
    # ===== 32j. A zip whose packed size is 6 bytes more than its deflate stream (7-Zip: "data after the end of the payload"): extracted, with a note =====
    $da = Join-Path $t "dae"; New-Item -ItemType Directory $da | Out-Null
    Set-Content "$da\mk.py" -Encoding UTF8 -Value @'
import struct, sys, zlib
payload = b"LycanZip data-after-end test. " * 30
c = zlib.compressobj(9, zlib.DEFLATED, -15); d = c.compress(payload) + c.flush() + b"\x11" * 6
name = b"font.ttf"; crc = zlib.crc32(payload)
z = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 8, 0, 0x21, crc, len(d), len(payload), len(name), 0) + name + d
cd = len(z)
z += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 8, 0, 0x21, crc, len(d), len(payload), len(name), 0, 0, 0, 0, 0, 0) + name
z += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, len(z) - cd, cd, 0)
open(sys.argv[1], "wb").write(z)
'@
    python "$da\mk.py" "$da\odd.zip"
    $p = Start-Raw @("--extract-here", (Q "$da\odd.zip")); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "完成" 20)) "data after end: the extraction finishes (the Done button)"
    Check (Test-Path "$da\font.ttf") "data after end: the file is extracted, not deleted"
    Check ((Get-Item "$da\font.ttf").Length -eq 900) "data after end: with its whole content (900 bytes)"
    Check (Has-Text $win "*結尾有多餘資料*" 3) "data after end: the finished card says that the archive has data after the end (a note, not an error)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "keepdamaged") {
        Reset-State
    # ===== 32k. An archive that is cut off: what was extracted stays, the card says which files have errors; the setting switches it off =====
    $kd = Join-Path $t "kd"; New-Item -ItemType Directory "$kd\src" | Out-Null
    Set-Content "$kd\mk.py" -Encoding UTF8 -Value @'
import random, sys
random.seed(5)
open(sys.argv[1] + "/big.txt", "w").write("".join("line %d of the big file\n" % random.getrandbits(32) for _ in range(120000)))
'@
    python "$kd\mk.py" "$kd\src"
    Set-Content "$kd\src\a.txt" "small and good"
    Push-Location "$kd\src"; 7z a -tzip "$kd\full.zip" ".\a.txt" ".\big.txt" | Out-Null; Pop-Location
    $full = [IO.File]::ReadAllBytes("$kd\full.zip")
    foreach ($case in "keep", "discard") {
        New-Item -ItemType Directory "$kd\$case" | Out-Null
        [IO.File]::WriteAllBytes("$kd\$case\cut.zip", $full[0..([int]($full.Length * 0.6))])
        Set-Content -Path $settingsFile -Value ("compact_close_when_done=false`nkeep_damaged=" + $(if ($case -eq "keep") { "true" } else { "false" })) -Encoding ascii
        $p = Start-Raw @("--extract-here", (Q "$kd\$case\cut.zip")); $win = Get-Win $p
        Check ($null -ne (Wait-Find $win "關閉" 25)) "keep damaged ($case): the window ends with a 'Close' button (something failed)"
        Check (Test-Path "$kd\$case\cut\a.txt") "keep damaged ($case): the good file was extracted"
        if ($case -eq "keep") {
            $part = "$kd\keep\cut\big.txt"
            Check ((Test-Path $part) -and ((Get-Item $part).Length -gt 0) -and ((Get-Item $part).Length -lt (Get-Item "$kd\src\big.txt").Length)) "keep damaged: the cut file is kept as far as it came out"
            Check (Has-Text $win "*已解壓縮並保留*個有錯誤的檔案*big.txt*" 4) "keep damaged: the card names the kept file"
        } else {
            Check (-not (Test-Path "$kd\discard\cut\big.txt")) "keep damaged (off): the cut file is deleted, as before"
        }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Reset-State
    }

    if (Want "dragimage") {
        Reset-State
    # ===== 32l. Dragging out of an archive: the picture next to the cursor (the icon, the name and what it is) =====
    # While the button is held over Explorer the screen is looked at: the dark card of the drag image is near the cursor (the card is (36,38,44); nothing else in the test is).
    $di = Join-Path $t "di"; New-Item -ItemType Directory "$di\target" | Out-Null; Copy-Item "$t\sample.zip" "$di\sample.zip"
    $p = Start-App "$di\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800; $p.Refresh()
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$di\target`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*target*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "drag image: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        $row = $null; foreach ($i in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.FindFirst($TS::Descendants, (ByName "readme.txt"))) { $row = $i; break } }
        Check ($null -ne $row) "drag image: the row to drag is there"
        if ($row) {
            $r = $row.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 1200
            # look at the screen around the cursor
            $bmp = New-Object System.Drawing.Bitmap 520, 260
            Add-Type -Namespace Cap -Name G -ErrorAction SilentlyContinue -MemberDefinition '[DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h); [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc); [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, int rop);'
            $g = [System.Drawing.Graphics]::FromImage($bmp); $hd = $g.GetHdc(); $sd = [Cap.G]::GetDC([IntPtr]::Zero)
            [Cap.G]::BitBlt($hd, 0, 0, 520, 260, $sd, $tx - 100, $ty - 60, 0x40CC0020) | Out-Null   # SRCCOPY | CAPTUREBLT: the drag image is a layered window
            [Cap.G]::ReleaseDC([IntPtr]::Zero, $sd) | Out-Null; $g.ReleaseHdc($hd); $g.Dispose()
            $dark = 0
            for ($y = 0; $y -lt 260; $y += 2) { for ($x = 0; $x -lt 520; $x += 2) { $c = $bmp.GetPixel($x, $y); if ([math]::Abs($c.R - 36) -lt 14 -and [math]::Abs($c.G - 38) -lt 14 -and [math]::Abs($c.B - 44) -lt 14) { $dark++ } } }
            $bmp.Save((Join-Path $env:TEMP "lycan_dragimage.png")); $bmp.Dispose()
            Check ($dark -gt 400) "drag image: the card with the icon and the name is next to the cursor ($dark dark pixels)"
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 2
            Check (Test-Path "$di\target\readme.txt") "drag image: and the drag still drops the file"
        }
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "previewleak") {
        Reset-State
    # ===== 19. Opening and closing the preview window again and again must not leak memory =====
    # Before, the ImageOpened handler held a strong reference to the preview window and often did not fire, so the whole preview window (with the decode cache) was still alive after closing: about 200 MB leaked per open/close.
    # UI Automation enumeration is not used here (the UIA provider in the tested program accumulates memory itself): UIA only picks the first picture, the rest uses the keyboard, EnumWindows and WM_CLOSE.
    $pl = Join-Path $t "previewleak"; New-Item -ItemType Directory "$pl/src" | Out-Null
    Set-Content "$pl/mk.py" -Encoding UTF8 -Value @'
import os, sys
from PIL import Image
for i in range(1, 21):
    Image.effect_noise((1600, 1200), 60).convert("RGB").save(os.path.join(sys.argv[1], "img%02d.png" % i))
'@
    python "$pl/mk.py" "$pl/src"
    Push-Location "$pl/src"; 7z a -tzip "$pl/imgs.zip" "./*" | Out-Null; Pop-Location
    Add-Type -Namespace N3 -Name W -ErrorAction SilentlyContinue -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
    function PL-Hwnd($procId) {
        $script:plFound = [IntPtr]::Zero
        $cb = [N3.W+EnumProc]{ param($h, $l)
            $pid2 = 0; [N3.W]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
            if ($pid2 -eq $procId -and [N3.W]::IsWindowVisible($h)) {
                $sb = New-Object System.Text.StringBuilder 256; [N3.W]::GetWindowText($h, $sb, 256) | Out-Null
                if ($sb.ToString() -like "*.png*") { $script:plFound = $h; return $false }
            }
            return $true }
        [N3.W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
        $script:plFound
    }
    function PL-Cycle($procId) {
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $h = [IntPtr]::Zero; $end = (Get-Date).AddSeconds(10)
        do { $h = PL-Hwnd $procId; if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 } } while ($h -eq [IntPtr]::Zero -and (Get-Date) -lt $end)
        if ($h -eq [IntPtr]::Zero) { return $false }
        Start-Sleep -Milliseconds 1500
        [N3.W]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
        Start-Sleep -Milliseconds 700
        [System.Windows.Forms.SendKeys]::SendWait("{DOWN}"); Start-Sleep -Milliseconds 300
        return $true
    }
    $p = Start-App "$pl/imgs.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Seconds 2
    $row = Find-Row $win "img01.png"
    $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
    $allOk = $true
    foreach ($i in 1..3) { if (-not (PL-Cycle $p.Id)) { $allOk = $false } }          # warm up
    Start-Sleep -Seconds 3; $p.Refresh(); $m0 = [int]($p.PrivateMemorySize64 / 1MB)
    foreach ($i in 1..10) { if (-not (PL-Cycle $p.Id)) { $allOk = $false } }
    Start-Sleep -Seconds 3; $p.Refresh(); $m1 = [int]($p.PrivateMemorySize64 / 1MB)
    Check $allOk "previewleak: the preview window opened and closed in every cycle"
    Check ((PL-Hwnd $p.Id) -eq [IntPtr]::Zero) "previewleak: no preview window is left after closing"
    $growth = $m1 - $m0
    Check ($growth -lt 100) "previewleak: private memory grew by $growth MB over 10 open/close cycles (limit 100 MB; the first leak was ~200 MB per cycle, the second ~6 MB per cycle, now ~1.5 MB per cycle)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "multipreview") {
        Reset-State
    # ===== 20. Preview window of each of several windows =====
    $mp = Join-Path $t "multipreview"; New-Item -ItemType Directory "$mp/a", "$mp/b" | Out-Null
    Set-Content "$mp/mk.py" -Encoding UTF8 -Value @'
import os, sys
from PIL import Image
base = sys.argv[1]
for d, color in (("a", (200, 40, 40)), ("b", (40, 80, 200))):
    for i in (1, 2, 3):
        Image.new("RGB", (160 + i * 10, 120), color).save(os.path.join(base, d, "%s_%d.png" % (d, i)))
'@
    python "$mp/mk.py" $mp
    Push-Location "$mp/a"; 7z a -tzip "$mp/A.zip" "./*" | Out-Null; Pop-Location
    Push-Location "$mp/b"; 7z a -tzip "$mp/B.zip" "./*" | Out-Null; Pop-Location
    Add-Type -Namespace U4 -Name W -ErrorAction SilentlyContinue -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);'
    function MP-Activate($w) { [U4.W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [U4.W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [U4.W]::SetForegroundWindow([IntPtr]$w.Current.NativeWindowHandle) | Out-Null; Start-Sleep -Milliseconds 700 }
    function MP-Wins($procId) { @($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)))) | Where-Object { $_.Current.Name -ne "" } }
    function MP-Previews($procId) { @(MP-Wins $procId | Where-Object { $_.Current.Name -like "*.png*" }) }
    function MP-Main($procId, $zip) { MP-Wins $procId | Where-Object { $_.Current.Name -like "LycanZip - $zip*" } | Select-Object -First 1 }
    function MP-Wait($procId, [scriptblock]$cond, [int]$sec = 10) { $end = (Get-Date).AddSeconds($sec); do { if (& $cond) { return $true }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end); return $false }
    function MP-Preview-From($procId, $zip, $img) {
        $m = MP-Main $procId $zip; MP-Activate $m
        $row = Find-Row $m $img
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        [System.Windows.Forms.SendKeys]::SendWait("%p")
    }

    $p = Start-Process $exe -ArgumentList (Q "$mp/A.zip") -PassThru
    Check (MP-Wait $p.Id { (MP-Wins $p.Id).Count -ge 1 }) "multipreview: archive A opens"
    Start-Process $exe -ArgumentList (Q "$mp/B.zip")
    Check (MP-Wait $p.Id { (MP-Wins $p.Id).Count -ge 2 }) "multipreview: archive B opens in its own window"
    Start-Sleep -Seconds 1

    MP-Preview-From $p.Id "A.zip" "a_1.png"
    Check (MP-Wait $p.Id { (MP-Previews $p.Id | Where-Object { $_.Current.Name -like "a_1.png*A.zip*" }).Count -eq 1 }) "multipreview: window A's preview opens (title shows image and archive: a_1.png ... A.zip)"
    MP-Preview-From $p.Id "B.zip" "b_1.png"
    Check (MP-Wait $p.Id { (MP-Previews $p.Id | Where-Object { $_.Current.Name -like "b_1.png*B.zip*" }).Count -eq 1 }) "multipreview: window B gets its own preview"
    Check ((MP-Previews $p.Id).Count -eq 2) "multipreview: both previews are open at the same time (the old one is not replaced)"

    # Previewing another picture from the same main window: reuses its own preview window, adds no window, and does not touch the ones of others
    MP-Preview-From $p.Id "A.zip" "a_2.png"
    Check (MP-Wait $p.Id { (MP-Previews $p.Id | Where-Object { $_.Current.Name -like "a_2.png*A.zip*" }).Count -eq 1 }) "multipreview: previewing another image in window A reuses A's preview"
    Check ((MP-Previews $p.Id).Count -eq 2) "multipreview: still two preview windows (no extra window)"
    Check ((MP-Previews $p.Id | Where-Object { $_.Current.Name -like "b_1.png*B.zip*" }).Count -eq 1) "multipreview: B's preview was not touched"

    # Close main window A: only the preview of A closes
    (MP-Main $p.Id "A.zip").GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Check (MP-Wait $p.Id { (MP-Previews $p.Id | Where-Object { $_.Current.Name -like "*A.zip*" }).Count -eq 0 }) "multipreview: closing main window A closes A's preview"
    Check ((MP-Previews $p.Id | Where-Object { $_.Current.Name -like "b_1.png*B.zip*" }).Count -eq 1) "multipreview: B's preview is still open"
    Check ($null -ne (MP-Main $p.Id "B.zip")) "multipreview: main window B is still open"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "crash") {
        Reset-State
    # ===== 21. Crash records and recovery =====
    # LYCAN_TEST_CRASH=milliseconds: after that time the App causes an access violation on purpose (0xC0000005). In portable mode the crash folder is crashes next to the exe.
    $cr = Join-Path $t "crash"; New-Item -ItemType Directory $cr | Out-Null
    Set-Content "$cr/f.txt" "x"
    Push-Location $cr; 7z a -tzip "$cr/CrashTest.zip" "./f.txt" | Out-Null; Pop-Location
    $crashDir = Join-Path $exeDir "crashes"
    if (Test-Path $crashDir) { Remove-Item $crashDir -Recurse -Force }
    New-Item -ItemType Directory $crashDir | Out-Null
    try {
        # A hand-written old report (with BOM): a task cut off half way, with a .lycantmp next to the output file; plus the "output file itself" to check that it is not deleted
        Set-Content "$cr/fake.zip.lycantmp" "partial"
        Set-Content "$cr/keep.zip" "keep me"
        Set-Content "$crashDir/crash-20200101-000000-1.txt" -Encoding UTF8 -Value ("LycanZip crash report`ntime=2020-01-01 00:00:00`nversion=0.0.0`nexception=0xC0000005`naddress=test`ndump=`n[state]`ntask`tCompress`tRunning`tfake.zip`t$($cr -replace '/', '\')\fake.zip`ntask`tCompress`tRunning`tkeep.zip`t$($cr -replace '/', '\')\other.zip")

        # 1. Really crash once
        $env:LYCAN_TEST_CRASH = "2500"
        try { $p = Start-App "$cr/CrashTest.zip" } finally { Remove-Item Env:LYCAN_TEST_CRASH -ErrorAction SilentlyContinue }
        Check ($p.WaitForExit(25000)) "crash: the process ends by itself (deliberate access violation)"
        Check ($p.HasExited -and $p.ExitCode -eq -1073741819) "crash: exit code is 0xC0000005 (access violation)"
        $dmp = Get-ChildItem $crashDir -Filter "crash-*.dmp" | Select-Object -First 1
        $txt = Get-ChildItem $crashDir -Filter "crash-*.txt" | Where-Object { $_.Name -notlike "*20200101*" } | Select-Object -First 1
        Check (($null -ne $dmp) -and $dmp.Length -gt 10KB) "crash: a minidump was written"
        Check ($null -ne $txt) "crash: a text report was written"
        if ($txt) {
            $report = Get-Content $txt.FullName -Raw -Encoding UTF8
            Check ($report -match "exception=0xC0000005") "crash: the report has the exception code"
            Check ($report -match "window\t[^\r\n]*CrashTest\.zip") "crash: the report lists the archive that was open"
        }

        # 2. Next start: the notice, our compression temporary files are cleaned up, one click reopens the archives of last time
        $p = Start-Process $exe -PassThru; $win = Get-Win $p
        Check ($null -ne (Wait-Find $win "LycanZip 上次意外結束" 12)) "crash: the next start explains that LycanZip closed unexpectedly"
        Check ($null -ne (Find $win "重新開啟 1 個壓縮檔")) "crash: the dialog offers to reopen the archive that was open"
        Check (-not (Test-Path "$cr/fake.zip.lycantmp")) "crash: the leftover '<output>.lycantmp' of an unfinished compression was removed"
        Check ((Test-Path "$cr/keep.zip") -and ((Get-Content "$cr/keep.zip" -Raw) -match "keep me")) "crash: a file that is not a '.lycantmp' leftover is never touched"
        Click (Find $win "重新開啟 1 個壓縮檔")
        $end = (Get-Date).AddSeconds(12); $reopened = $false
        do { $reopened = @(@($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)))) | Where-Object { $_.Current.Name -like "*CrashTest.zip*" }).Count -ge 1; if (-not $reopened) { Start-Sleep -Milliseconds 300 } } while (-not $reopened -and (Get-Date) -lt $end)
        Check $reopened "crash: 'reopen' opens the archive that was open when it crashed"
        Check (@(Get-ChildItem $crashDir -Filter "*.seen.txt").Count -ge 2) "crash: the reports are marked as seen (kept for bug reports)"

        # 3. Start once more: no notice any more
        $p = Start-App; $win = Get-Win $p; Wait-Find $win "解壓縮到此" 6 | Out-Null
        Start-Sleep -Seconds 2
        Check ($null -eq (Find $win "LycanZip 上次意外結束")) "crash: the question is not asked again"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

        # 4. No crash report (the process ended by hand) is not mistaken for a crash
        $p = Start-App "$cr/CrashTest.zip"; $win = Get-Win $p; Start-Sleep -Seconds 2
        Stop-Process -Id $p.Id -Force
        $p = Start-App; $win = Get-Win $p; Start-Sleep -Seconds 3
        Check ($null -eq (Find $win "LycanZip 上次意外結束")) "crash: killing the process by hand is not mistaken for a crash"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    } finally {
        Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        if (Test-Path $crashDir) { Remove-Item $crashDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
    }

    if (Want "previewonly") {
        Reset-State
    # ===== 33. --preview-images / --preview-audio: only the preview window shows (no main window) =====
    $po = Join-Path $t "previewonly"; New-Item -ItemType Directory "$po/src/a", "$po/src/b/deep", "$po/src/none" -Force | Out-Null
    foreach ($spec in @(@("a\pic1.png", 64), @("b\deep\pic2.png", 128))) {
        $bmp = New-Object System.Drawing.Bitmap $spec[1], 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
        $bmp.Save("$po\src\$($spec[0])", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    # a short silent 8-bit WAV
    $rate = 8000; $n = $rate
    $hdr = [System.Collections.Generic.List[byte]]::new()
    foreach ($part in @([Text.Encoding]::ASCII.GetBytes("RIFF"), [BitConverter]::GetBytes([int](36 + $n)), [Text.Encoding]::ASCII.GetBytes("WAVEfmt "), [BitConverter]::GetBytes([int]16), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int]$rate), [BitConverter]::GetBytes([int]$rate), [BitConverter]::GetBytes([int16]1), [BitConverter]::GetBytes([int16]8), [Text.Encoding]::ASCII.GetBytes("data"), [BitConverter]::GetBytes([int]$n))) { $hdr.AddRange([byte[]]$part) }
    $wav = $hdr.ToArray() + ([byte[]](, 128 * $n))
    [IO.File]::WriteAllBytes("$po\src\b\song.wav", $wav)
    Set-Content "$po\src\none\only.txt" "text" -Encoding ascii
    Push-Location "$po\src"; 7z a -tzip "$po\media.zip" ".\a" ".\b" | Out-Null; 7z a -tzip "$po\text.zip" ".\none" | Out-Null; Pop-Location
    $env:LYCAN_TEST_MUTE = "1"
    function Top-Windows($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); @($AE::RootElement.FindAll($TS::Children, $c) | Where-Object { -not $_.Current.IsOffscreen -or $_.Current.Name }) }
    function Wait-Window($proc, [string]$like, [int]$sec = 15) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (Top-Windows $proc)) { if ($w.Current.Name -like $like) { return $w } }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
        return $null
    }

    # images: both folders' pictures, in one list; the main window never shows
    $p = Start-Raw @("--preview-images", (Q "$po\media.zip"))
    $pw = Wait-Window $p "pic*.png*"
    Check ($null -ne $pw) "preview-only images: the preview window opens by itself"
    if ($pw) {
        Check ($null -ne (Wait-Find $pw "1 / 2" 8)) "preview-only images: every level of the archive is listed (1 / 2)"
        $mainShown = @(Top-Windows $p | Where-Object { $_.Current.Name -notlike "pic*.png*" -and $_.Current.BoundingRectangle.Width -gt 100 -and -not $_.Current.IsOffscreen })
        Check ($mainShown.Count -eq 0) "preview-only images: no main window is on screen ($($mainShown.Count))"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
        Check (Wait-Exit $p 12) "preview-only images: closing the preview ends the process"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # audio
    $p = Start-Raw @("--preview-audio", (Q "$po\media.zip"))
    $aw = Wait-Window $p "音訊預覽*"
    Check ($null -ne $aw) "preview-only audio: the player window opens by itself"
    if ($aw) {
        $aw.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
        Check (Wait-Exit $p 12) "preview-only audio: closing the player ends the process"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # nothing to preview: the normal window opens with a note
    $p = Start-Raw @("--preview-images", (Q "$po\text.zip"))
    $w4 = Get-Win $p
    Check ($null -ne (Wait-Find $w4 "此壓縮檔中沒有可預覽的圖片" 12)) "preview-only images: an archive without images opens normally and says so"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_MUTE -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "shiftopen") {
        Reset-State
    # ===== B batch: an archive opened with Shift held (setting: Shift / Ctrl / not used) goes straight to the picture preview =====
    $so = Join-Path $t "shiftopen"; New-Item -ItemType Directory "$so/src/a", "$so/src/none" -Force | Out-Null
    foreach ($n in 1..2) { $bmp = New-Object System.Drawing.Bitmap (60 + $n * 10), 40; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Orange); $g.Dispose(); $bmp.Save("$so\src\a\shot$n.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }
    Set-Content "$so\src\none\only.txt" "text" -Encoding ascii
    Push-Location "$so\src"; 7z a -tzip "$so\pics.zip" ".\a" | Out-Null; 7z a -tzip "$so\text.zip" ".\none" | Out-Null; Pop-Location
    # a held key is passed with LYCAN_TEST_KEYS (injected key presses are not always seen by another process)
    function Top-Windows2($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); @($AE::RootElement.FindAll($TS::Children, $c) | Where-Object { -not $_.Current.IsOffscreen -or $_.Current.Name }) }
    function Wait-Window2($proc, [string]$like, [int]$sec = 12) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (Top-Windows2 $proc)) { if ($w.Current.Name -like $like) { return $w } }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
        return $null
    }
    function Start-Held([string]$key, [string]$arc) {
        $env:LYCAN_TEST_KEYS = $key
        $pr = Start-Raw @((Q $arc))
        Remove-Item Env:LYCAN_TEST_KEYS -ErrorAction SilentlyContinue
        return $pr
    }
    # Shift (the default): the preview opens, no main window
    $p = Start-Held "shift" "$so\pics.zip"
    $pw = Wait-Window2 $p "shot*.png*"
    Check ($null -ne $pw) "shift open: with Shift held the picture preview opens by itself"
    $mainShown = @(Top-Windows2 $p | Where-Object { $_.Current.Name -notlike "shot*.png*" -and $_.Current.BoundingRectangle.Width -gt 100 -and -not $_.Current.IsOffscreen })
    Check ($mainShown.Count -eq 0) "shift open: no main window is on screen ($($mainShown.Count))"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # no key: the normal window
    $p = Start-Raw @((Q "$so\pics.zip")); $w = Get-Win $p
    Check ($null -ne (Wait-Find $w "shot1.png" 12)) "shift open: without a key the archive opens in the normal window"
    Check ($null -eq (Wait-Window2 $p "shot*.png*" 2)) "shift open: and no preview window"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # Shift, but no pictures inside: the normal window with the note
    $p = Start-Held "shift" "$so\text.zip"; $w = Get-Win $p
    Check ($null -ne (Wait-Find $w "此壓縮檔中沒有可預覽的圖片" 12)) "shift open: an archive without pictures opens normally and says so"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # the setting: Ctrl instead of Shift
    Set-Content $settingsFile "welcome_done=true`nopen_preview_key=ctrl" -Encoding UTF8
    $p = Start-Held "shift" "$so\pics.zip"; $w = Get-Win $p
    Check ($null -eq (Wait-Window2 $p "shot*.png*" 3)) "shift open: with the key set to Ctrl, Shift does nothing"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p = Start-Held "ctrl" "$so\pics.zip"
    Check ($null -ne (Wait-Window2 $p "shot*.png*")) "shift open: with the key set to Ctrl, Ctrl opens the preview"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Set-Content $settingsFile "welcome_done=true`nopen_preview_key=off" -Encoding UTF8
    $p = Start-Held "shift" "$so\pics.zip"; $w = Get-Win $p
    Check ($null -eq (Wait-Window2 $p "shot*.png*" 3)) "shift open: switched off, Shift does nothing"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "passwords") {
        Reset-State
    # ===== Password manager: "Remember this password" saves it (encrypted); the next archive opens with it, no question; the settings tab =====
    $store = Join-Path $exeDir "passwords.dat"
    function Toggle-On($el) { $tp = $null; return ($el -and $el.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp) -and $tp.Current.ToggleState -eq [Windows.Automation.ToggleState]::On) }
    Remove-Item "$t\enc" -Recurse -Force -ErrorAction SilentlyContinue   # (the section 'password' extracts there too)
    $p = Start-App "$t\enc.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    $pw = Wait-Find $win "密碼" 8
    $remember = Wait-Find $win "記住這個密碼" 4
    Check ($null -ne $remember -and -not (Toggle-On $remember)) "passwords: the dialog has 'Remember this password', not ticked by default"
    if ($pw -and $remember) {
        $remember.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 300
        Type-Into (Find $win "密碼") "Secret123"; Click (Find $win "確定")
        $r = Wait-Task $win "^完成　" 20
        Check ($null -ne $r) "passwords: the extraction with the typed password completes"
        Check (Test-Path $store) "passwords: ticked, the password is saved (passwords.dat)"
        $raw = if (Test-Path $store) { [IO.File]::ReadAllBytes($store) } else { @() }
        $ascii = [Text.Encoding]::ASCII.GetString($raw); $wide = [Text.Encoding]::Unicode.GetString($raw)
        Check ($raw.Length -gt 0 -and $ascii -notmatch "Secret123" -and $wide -notmatch "Secret123" -and $ascii.StartsWith("LZPW")) "passwords: the file is encrypted (the password does not appear in it)"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    # another process, an archive with encrypted names and the same password: opened with the saved one, no question
    $p = Start-App "$t\hdr.7z"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "解壓縮到此" 15)) "passwords: an archive with encrypted names opens with the saved password"
    Check ($null -eq (Find $win "需要密碼")) "passwords: without asking"
    Check (Has-Text $win "已使用儲存的密碼" 6) "passwords: and says that a saved password was used"
    # the settings tab: the saved password is listed hidden, the eye shows it
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "密碼"; Start-Sleep -Milliseconds 800
    Check ($null -ne (Wait-Find $win "已隱藏的密碼" 4)) "passwords: the settings tab lists it, hidden"
    $eye = Find $win "顯示密碼"
    if ($eye) { $eye.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 400 }
    Check ($null -ne (Wait-Find $win "Secret123" 3)) "passwords: the eye button shows it"
    Check ($null -ne (Find $win "自動嘗試已儲存的密碼")) "passwords: the tab has the switch 'Try saved passwords automatically'"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    # trying switched off: the question comes again
    Set-Content $settingsFile "welcome_done=true`nauto_try_passwords=false" -Encoding UTF8
    $p = Start-App "$t\hdr.7z"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "需要密碼" 10)) "passwords: with 'Try saved passwords' off the archive asks again"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "masterpw") {
        Reset-State
    # ===== Master password: set it; a locked list is unlocked in the password dialog and then tried; lock / unlock / turn off; a new
    # archive takes a saved password; forgotten: everything is deleted =====
    $store = Join-Path $exeDir "passwords.dat"
    function Scheme() { if (Test-Path $store) { [IO.File]::ReadAllBytes($store)[8] } else { -1 } }
    function Enabled($el) { $el -and $el.Current.IsEnabled }
    function Find-Edit($w, $name) { $w.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Edit))))) }
    function Wait-Edit($w, $name, [int]$sec = 6) { $end = (Get-Date).AddSeconds($sec); do { $e = Find-Edit $w $name; if ($e) { return $e }; Start-Sleep -Milliseconds 150 } while ((Get-Date) -lt $end); return $null }
    function Find-InProc($proc, $name, [int]$sec = 6) {   # a flyout is a window of its own, not under the main window
        $pc = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in $AE::RootElement.FindAll($TS::Children, $pc)) { $e = $w.FindFirst($TS::Descendants, (ByName $name)); if ($e) { return $e } }; Start-Sleep -Milliseconds 200 } while ((Get-Date) -lt $end)
        return $null
    }
    Remove-Item "$t\enc" -Recurse -Force -ErrorAction SilentlyContinue
    # a saved password first ("Remember this password")
    $p = Start-App "$t\enc.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" | Out-Null
    Click (Find $win "解壓縮到此")
    if (Wait-Find $win "記住這個密碼" 8) {
        Click (Find $win "記住這個密碼"); Start-Sleep -Milliseconds 300
        Type-Into (Find-Edit $win "密碼") "Secret123"; Click (Find $win "確定")
        Wait-Task $win "^完成　" 20 | Out-Null
    }
    Check ((Scheme) -eq 1) "master password: a saved password, without a master password (DPAPI only)"
    # set the master password: OK only when it is long enough and typed twice the same
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "密碼"; Start-Sleep -Milliseconds 600
    Check (Has-Text $win "關閉。已儲存的密碼只受*" 4) "master password: the card says it is off"
    # (the dialog that adds a password: its button says Save, not the "Save to: {0}" of the new archive page)
    Click (Find $win "新增密碼")
    if (Wait-Find $win "儲存" 4) { Check (-not (Has-Text $win "儲存到*" 1)) "passwords: the add dialog's button is 'Save'"; Click (Find $win "取消") }
    else { Check $false "passwords: the add dialog's button is 'Save'" }
    Start-Sleep -Milliseconds 500
    Click (Find $win "設定主密碼")
    $new = Wait-Edit $win "新的主密碼"
    if ($new) {
        Type-Into $new "abc"; Type-Into (Find-Edit $win "再次輸入密碼") "abc"
        Check (-not (Enabled (Find $win "確定"))) "master password: 3 characters are too short (OK disabled)"
        $before = $new.Current.BoundingRectangle
        Type-Into $new "d1"; Type-Into (Find-Edit $win "再次輸入密碼") "d2"
        $after = $new.Current.BoundingRectangle
        Check ($before.Width -eq $after.Width -and $before.Y -eq $after.Y) "master password: the dialog keeps its size when the mismatch note appears"
        Check (-not (Enabled (Find $win "確定"))) "master password: two different entries (OK disabled)"
        Check (Has-Text $win "兩次輸入的密碼不一致*" 2) "master password: and it says they don't match"
        (Find-Edit $win "再次輸入密碼").SetFocus(); [System.Windows.Forms.SendKeys]::SendWait("{BACKSPACE}1"); Start-Sleep -Milliseconds 300
        Check (Enabled (Find $win "確定")) "master password: the same 5 characters twice (OK enabled)"
        Click (Find $win "確定")
    } else { Check $false "master password: the dialog to set it opens" }
    Check (Has-Text $win "開啟。已儲存的密碼已解鎖*" 6) "master password: set, the card says the list is unlocked until LycanZip closes"
    Check ((Scheme) -eq 2) "master password: passwords.dat is saved under the master password (scheme 2)"
    $raw = if (Test-Path $store) { [IO.File]::ReadAllBytes($store) } else { @() }
    Check ($raw.Length -gt 0 -and [Text.Encoding]::Unicode.GetString($raw) -notmatch "Secret123" -and [Text.Encoding]::ASCII.GetString($raw) -notmatch "Secret123") "master password: the password does not appear in the file"
    # lock now: the list is hidden; unlock: a wrong master password is refused, the right one shows it again
    Click (Find $win "立即鎖定"); Start-Sleep -Milliseconds 500
    Check (Has-Text $win "已儲存的密碼已鎖定。用主密碼解鎖*" 4) "master password: locked, the list says so instead of the passwords"
    Check ($null -eq (Find $win "已隱藏的密碼")) "master password: and no saved password is listed"
    Click (Find $win "解鎖")
    $mb = Wait-Edit $win "主密碼"
    if ($mb) {
        Type-Into $mb "wrong"; Click (Find $win "解鎖")
        Check (Has-Text $win "主密碼錯誤。" 8) "master password: a wrong master password is refused"
        Type-Into (Find-Edit $win "主密碼") "abcd1"; Click (Find $win "解鎖")
    }
    Check ($null -ne (Wait-Find $win "已隱藏的密碼" 8)) "master password: unlocked, the saved password is listed again"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # another run: locked; the password dialog of an archive can unlock the list, and then the saved password is used
    $p = Start-App "$t\hdr.7z"; $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "需要密碼" 12)) "master password: locked, an archive with encrypted names asks"
    $mb = Wait-Edit $win "主密碼" 4
    Check ($null -ne $mb -and $null -ne (Find $win "解鎖並嘗試")) "master password: the dialog has the master password field and 'Unlock and try'"
    if ($mb) {
        Type-Into $mb "wrong"; Click (Find $win "解鎖並嘗試")
        Check (Has-Text $win "主密碼錯誤。" 8) "master password: in the dialog a wrong master password is refused"
        Type-Into (Find-Edit $win "主密碼") "abcd1"; Click (Find $win "解鎖並嘗試")
        Check ($null -ne (Wait-Find $win "解壓縮到此" 15)) "master password: unlocked in the dialog, the archive opens with the saved password"
        Check (Has-Text $win "已使用儲存的密碼" 6) "master password: and says that a saved password was used"
    }
    # a new archive: "Use a saved password" fills the password (the list is unlocked in this run)
    $cm = Join-Path $t "masterpw_add"; New-Item -ItemType Directory $cm -Force | Out-Null; Set-Content "$cm\a.txt" "hello"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    $p = Start-Raw @("--add", (Q "$cm\a.txt")); $w2 = Get-Win $p
    Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
    $pick = Wait-Find $w2 "使用已儲存的密碼" 6
    Check ($null -ne $pick) "master password: the new archive page has 'Use a saved password'"
    if ($pick) {
        Click $pick
        $mb = Wait-Edit $w2 "主密碼"
        Check ($null -ne $mb) "master password: locked (a new run), picking asks for the master password first"
        if ($mb) { Type-Into $mb "abcd1"; Click (Find $w2 "解鎖") }
        $item = Find-InProc $p "已隱藏的密碼" 8
        Check ($null -ne $item -and $null -ne (Find-InProc $p "S••••3" 2)) "master password: the list shows the saved one as its first and last character"
        if ($item) { Click $item; Start-Sleep -Milliseconds 500 }
        $pwBox = Find-Edit $w2 "密碼"
        $vp = $null
        Check ($pwBox -and $pwBox.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$vp) -and $vp.Current.Value -eq "Secret123") "master password: choosing it fills the password of the new archive"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # turn it off (needs the master password): DPAPI only again, the saved password stays
    $p = Start-App; $win = Get-Win $p
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "密碼"; Start-Sleep -Milliseconds 600
    Click (Find $win "解鎖")
    if ($mb = Wait-Edit $win "主密碼") { Type-Into $mb "abcd1"; Click (Find $win "解鎖") }
    Wait-Find $win "關閉主密碼" 8 | Out-Null
    Click (Find $win "關閉主密碼")
    if ($cur = Wait-Edit $win "目前的主密碼") { Type-Into $cur "abcd1"; Click (Find $win "確定") }
    Check (Has-Text $win "關閉。已儲存的密碼只受*" 8) "master password: turned off with the master password"
    Check ((Scheme) -eq 1 -and $null -ne (Find $win "已隱藏的密碼")) "master password: the file is DPAPI only again and the saved password stays"
    # set again, then forgotten: in a new run the list is locked; "Forgot" deletes everything
    Click (Find $win "設定主密碼")
    if ($new = Wait-Edit $win "新的主密碼") { Type-Into $new "zzzz"; Type-Into (Find-Edit $win "再次輸入密碼") "zzzz"; Click (Find $win "確定") }
    Wait-Find $win "立即鎖定" 8 | Out-Null
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    $p = Start-App; $win = Get-Win $p
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "密碼"; Start-Sleep -Milliseconds 600
    Check (Has-Text $win "開啟。已儲存的密碼已鎖定。" 4) "master password: a new run starts locked"
    Click (Find $win "忘記主密碼？")
    if ($del = Wait-Find $win "全部刪除" 6) { Click $del }
    Check (Has-Text $win "關閉。已儲存的密碼只受*" 6) "master password: forgotten, the master password is gone"
    Check (-not (Test-Path $store)) "master password: and with it every saved password (passwords.dat deleted)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $store -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "repack") {
        Reset-State
    # ===== Repacking: convert an encrypted zip to 7z (the password kept), remove the password in place, upgrade an old Shift-JIS zip,
    # convert several archives from the command line (the context menu), and the menu item is off by default =====
    function Find-InProc($proc, $name, [int]$sec = 6) {   # menus and flyouts are windows of their own
        $pc = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in $AE::RootElement.FindAll($TS::Children, $pc)) { $e = $w.FindFirst($TS::Descendants, (ByName $name)); if ($e) { return $e } }; Start-Sleep -Milliseconds 200 } while ((Get-Date) -lt $end)
        return $null
    }
    function Find-Edit($w, $name) { $w.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Edit))))) }
    function More-Item($proc, $win, $name) {   # opens "More options" and returns the item (null when it is not shown)
        Click (Find $win "其他選項"); Start-Sleep -Milliseconds 600
        $i = Find-InProc $proc $name 2
        if ($i -and -not $i.Current.IsOffscreen) { return $i }
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300
        return $null
    }
    function Seven-Ok($file, $pw) { cmd /c "7z t -p$pw `"$file`" >nul 2>&1"; return $LASTEXITCODE -eq 0 }   # (through cmd: the error output of 7z would stop the script)
    $rp = Join-Path $t "repack"; Remove-Item $rp -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $rp | Out-Null
    Copy-Item "$t\enc.zip" "$rp\conv.zip"
    Copy-Item "$t\enc.zip" "$rp\nopw.zip"

    # convert from the main window: the menu items of an encrypted zip
    $p = Start-App "$rp\conv.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null
    $item = More-Item $p $win "轉換壓縮檔格式…"
    Check ($null -ne $item) "repack: More options has 'Convert archive format...'"
    if ($item) { [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300 }
    Check ($null -ne (More-Item $p $win "移除壓縮檔的密碼")) "repack: an encrypted zip offers 'Remove archive password'"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300
    Check ($null -eq (More-Item $p $win "升級壓縮檔")) "repack: a zip with UTF-8 names offers no upgrade"
    $item = More-Item $p $win "轉換壓縮檔格式…"
    if ($item) {
        Click $item
        $pwBox = Wait-Find $win "密碼" 8
        if ($pwBox -and (Wait-Find $win "需要密碼" 2)) { Type-Into (Find-Edit $win "密碼") "Secret123"; Click (Find $win "確定") }
        Check (Has-Text $win "轉換壓縮檔格式" 8) "repack: the page is titled 'Convert archive format'"
        Check ($null -ne (Wait-Find $win "新壓縮檔的密碼" 4)) "repack: the page asks what the new archive's password is"
        Check ($null -eq (Find $win "加入檔案")) "repack: no 'add files' in the convert mode"
        Select-Combo $win "範本" "一般 7z"; Start-Sleep -Milliseconds 400
        Click (Find $win "開始轉換")
        $r = Wait-Task $win "^完成　" 30
        Check ($null -ne $r) "repack: the conversion completes ($r)"
        Check ((Test-Path "$rp\conv.7z") -and (Seven-Ok "$rp\conv.7z" "Secret123") -and -not (Seven-Ok "$rp\conv.7z" "")) "repack: conv.7z is made, with the original password"
        Check (Test-Path "$rp\conv.zip") "repack: the original stays"
        $del = Wait-Find $win "刪除原壓縮檔" 4
        Check ($null -ne $del) "repack: the card offers 'Delete the original archive'"
        if ($del) {
            Click $del
            Check (Has-Text $win "刪除原壓縮檔？" 4) "repack: deleting asks first"
            Check (Has-Text $win "新的壓縮檔已經通過完整性測試*" 2) "repack: and says the new archive passed the test"
            Click (Find $win "取消"); Start-Sleep -Milliseconds 400
        }
    } else { Check $false "repack: the convert item can be clicked" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # remove the password: no question, replaced after the test, opened again without a password
    $p = Start-App "$rp\nopw.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null
    $item = More-Item $p $win "移除壓縮檔的密碼"
    if ($item) {
        Click $item
        if (Wait-Find $win "需要密碼" 6) { Type-Into (Find-Edit $win "密碼") "Secret123"; Click (Find $win "確定") }
        Check (Has-Text $win "已移除密碼*" 30) "repack: removing the password says it was done (tested)"
        Check ((Seven-Ok "$rp\nopw.zip" "") -and -not (Test-Path "$rp\nopw.zip.lycantmp")) "repack: the archive opens without a password now, no scratch file left"
        Check ($null -eq (More-Item $p $win "移除壓縮檔的密碼")) "repack: and no longer offers to remove a password"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    } else { Check $false "repack: 'Remove archive password' can be clicked" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # upgrade: an old zip whose names are Shift-JIS without the UTF-8 mark
    $sj = Join-Path $rp "sjis"; New-Item -ItemType Directory "$sj\テスト" -Force | Out-Null
    foreach ($n in @("日本語のファイル.txt", "テスト\説明書.txt", "アーカイブの中身.txt")) { Set-Content -LiteralPath (Join-Path $sj $n) "x" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($sj, "$rp\old.zip", [System.IO.Compression.CompressionLevel]::Optimal, $false, [Text.Encoding]::GetEncoding(932))
    $p = Start-App "$rp\old.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null
    Check (Has-Text $win "*可透過升級壓縮檔來避免此問題。" 6) "repack: the encoding notice says that upgrading avoids the problem"
    $link = Wait-Find $win "升級壓縮檔" 4
    Check ($null -ne $link) "repack: and has an 'Upgrade archive' link"
    if ($link) {
        Click $link
        # first a dialog: the names as they will be stored, with the encoding to choose (it cannot be undone)
        Check (Has-Text $win "升級後檔名會照下面的樣子*" 6) "repack: upgrading asks first, showing how the names will be stored"
        Check ($null -ne (Wait-Find $win "日本語のファイル.txt" 3)) "repack: the dialog lists the names decoded as detected (Shift-JIS)"
        $dlgCombo = $null
        foreach ($c in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ComboBox)))) { if ($c.Current.Name -eq "檔名編碼" -and -not $c.Current.IsOffscreen) { $dlgCombo = $c } }
        function Count-Name($n) { @($win.FindAll($TS::Descendants, (ByName $n))).Count }   # (the list behind the dialog shows the names too)
        $before = Count-Name "日本語のファイル.txt"
        if ($dlgCombo) {
            $dlgCombo.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
            $opt = Find-InProc $p "Big5 (繁體中文)" 3
            if ($opt) { $opt.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500 }
            Check ((Count-Name "日本語のファイル.txt") -lt $before) "repack: another encoding in the dialog changes the names shown"
            $dlgCombo.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
            $opt = Find-InProc $p "Shift-JIS (日文)" 3
            if ($opt) { $opt.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500 }
        } else { Check $false "repack: the dialog has an encoding drop-down" }
        Click (Find $win "升級")
        Check (Has-Text $win "已升級壓縮檔*" 20) "repack: upgraded (tested, opened again)"
        $z = [System.IO.Compression.ZipFile]::OpenRead("$rp\old.zip")
        $names = @($z.Entries | ForEach-Object { $_.FullName }); $z.Dispose()
        Check ($names -contains "日本語のファイル.txt") "repack: the names are UTF-8 now (read without a code page)"
        Check ($null -eq (More-Item $p $win "升級壓縮檔")) "repack: nothing to upgrade any more"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # converting an old zip from the window: the page says the names are written as they are shown now
    [System.IO.Compression.ZipFile]::CreateFromDirectory($sj, "$rp\old2.zip", [System.IO.Compression.CompressionLevel]::Optimal, $false, [Text.Encoding]::GetEncoding(932))
    $p = Start-App "$rp\old2.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null
    $item = More-Item $p $win "轉換壓縮檔格式…"
    if ($item) { Click $item }
    Check (Has-Text $win "*檔名會照目前顯示的樣子*" 8) "repack: converting an old zip warns that the names are written as shown now"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # changing an old archive: a Shift-JIS zip keeps its names without asking; a Shift-JIS tar asks before writing every name in UTF-8
    function New-Folder($win, $name) {
        Click (Find $win "加入到壓縮檔"); Start-Sleep -Milliseconds 400
        Click (Wait-Find $win "新增資料夾…" 5)
        if ($box = Wait-Find $win "名稱" 6) { Type-Into $box $name; Click (Find $win "確定") }
    }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($sj, "$rp\old3.zip", [System.IO.Compression.CompressionLevel]::Optimal, $false, [Text.Encoding]::GetEncoding(932))
    $p = Start-App "$rp\old3.zip"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null; Start-Sleep -Milliseconds 800
    New-Folder $win "new"
    Check ((Has-Text $win "已修改壓縮檔*" 15) -and $null -eq (Find $win "以 UTF-8 儲存檔名？")) "repack: changing an old zip asks nothing"
    $z = [System.IO.Compression.ZipFile]::Open("$rp\old3.zip", "Read", [Text.Encoding]::GetEncoding(932))
    $names = @($z.Entries | ForEach-Object { $_.FullName }); $z.Dispose()
    Check ($names -contains "日本語のファイル.txt") "repack: and its old names stay Shift-JIS (read as Shift-JIS)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500
    $tarPy = Join-Path $rp "mktar.py"
    Set-Content $tarPy -Encoding UTF8 -Value @'
import sys, tarfile, io
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT, encoding="cp932") as t:
    for n in ["日本語のファイル.txt", "テスト/説明書.txt", "アーカイブの中身.txt"]:
        data = b"x"
        i = tarfile.TarInfo(n); i.size = len(data); t.addfile(i, io.BytesIO(data))
'@
    python $tarPy "$rp\old.tar"
    $p = Start-App "$rp\old.tar"; $win = Get-Win $p
    Wait-Find $win "解壓縮到此" 15 | Out-Null; Start-Sleep -Milliseconds 800
    New-Folder $win "new"
    Check (Has-Text $win "以 UTF-8 儲存檔名？" 6) "repack: changing an old tar asks first (all names become UTF-8)"
    Check ($null -ne (Wait-Find $win "日本語のファイル.txt" 2)) "repack: the question shows the names as they will be stored"
    Click (Find $win "取消"); Start-Sleep -Milliseconds 800
    Check (-not (Has-Text $win "已修改壓縮檔*" 2)) "repack: cancelling changes nothing"
    New-Folder $win "new"
    if (Wait-Find $win "以 UTF-8 儲存檔名？" 6) { Click (Find $win "繼續") }
    Check (Has-Text $win "已修改壓縮檔*" 15) "repack: continuing changes the tar"
    New-Folder $win "new2"
    Check ((Has-Text $win "已修改壓縮檔*" 15) -and $null -eq (Find $win "以 UTF-8 儲存檔名？")) "repack: the question comes only once"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # the context menu: --convert with two archives, each keeps its name
    Push-Location $sj; 7z a -tzip "$rp\a1.zip" . | Out-Null; 7z a -tzip "$rp\a2.zip" . | Out-Null; Pop-Location
    $p = Start-Raw @("--convert", (Q "$rp\a1.zip"), (Q "$rp\a2.zip")); $w2 = Get-Win $p
    Check (Has-Text $w2 "轉換壓縮檔格式" 10) "repack: --convert shows the convert page"
    Select-Combo $w2 "範本" "一般 7z"; Start-Sleep -Milliseconds 400
    Check (Has-Text $w2 "每個壓縮檔沿用原本的檔名*" 4) "repack: with two archives each keeps its name"
    Click (Find $w2 "開始轉換")
    $end = (Get-Date).AddSeconds(30); do { Start-Sleep -Milliseconds 400 } while (-not ((Test-Path "$rp\a1.7z") -and (Test-Path "$rp\a2.7z")) -and (Get-Date) -lt $end)
    Check ((Seven-Ok "$rp\a1.7z" "") -and (Seven-Ok "$rp\a2.7z" "")) "repack: both archives are converted (a1.7z, a2.7z)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500

    # the context menu item is off by default (settings, Explorer tab)
    $p = Start-App; $win = Get-Win $p
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "檔案總管"; Start-Sleep -Milliseconds 600
    $cb = Wait-Find $win "轉換壓縮檔格式…" 4
    $tp = $null
    Check ($cb -and $cb.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp) -and $tp.Current.ToggleState -eq [Windows.Automation.ToggleState]::Off) "repack: the context menu item exists and is off by default"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "reading") {
        Reset-State
    # ===== Reading mode: a comic book archive opens in it; the whole book in order, two pages side by side, right to left, remembered =====
    $rd = Join-Path $t "reading"; New-Item -ItemType Directory "$rd/src/ch1", "$rd/src/ch2" -Force | Out-Null
    Remove-Item (Join-Path $exeDir "reading.tsv") -ErrorAction SilentlyContinue
    # eight pages: seven upright ones (each a different width, so the status line tells them apart), the last one a two-page picture
    foreach ($n in 1..8) {
        $folder = if ($n -le 5) { "ch1" } else { "ch2" }
        $w = if ($n -eq 8) { 240 } else { 60 + $n }
        $bmp = New-Object System.Drawing.Bitmap $w, 120; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::SteelBlue); $g.Dispose()
        $bmp.Save("$rd\src\$folder\p0$n.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Push-Location "$rd\src"; 7z a -tzip "$rd\book.cbz" ".\ch1" ".\ch2" | Out-Null; Pop-Location
    function Reader-Windows($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); @($AE::RootElement.FindAll($TS::Children, $c)) }
    function Wait-Reader($proc, [int]$sec = 12) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (Reader-Windows $proc)) { if ($w.Current.Name -like "p0*.png*") { return $w } }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
        return $null
    }
    function Counter-Is($w, $text, $sec = 6) { Has-Text $w $text $sec }
    Add-Type -Namespace U6 -Name W -ErrorAction SilentlyContinue -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);'
    function Front($w) { [U6.W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [U6.W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [U6.W]::SetForegroundWindow([IntPtr]$w.Current.NativeWindowHandle) | Out-Null; Start-Sleep -Milliseconds 500 }
    function Press($w, $keys) { Front $w; [System.Windows.Forms.SendKeys]::SendWait($keys); Start-Sleep -Milliseconds 900 }

    $p = Start-Raw @((Q "$rd\book.cbz"))
    $rw = Wait-Reader $p
    Check ($null -ne $rw) "reading: a .cbz opens straight in the picture preview"
    if ($rw) {
        $rb = Wait-Find $rw "閱讀模式" 6
        $tp = $null
        Check ($rb -and $rb.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp) -and $tp.Current.ToggleState -eq [Windows.Automation.ToggleState]::On) "reading: in the reading mode"
        Check (Counter-Is $rw "1 / 8") "reading: the cover is alone (1 / 8)"
        Press $rw " "
        Check (Counter-Is $rw "2–3 / 8") "reading: the next spread shows two pages (2–3 / 8)"
        Press $rw " "
        Check (Counter-Is $rw "4–5 / 8") "reading: and the next two (4–5 / 8)"
        Press $rw " "
        Check (Has-Text $rw "ch2" 3) "reading: entering another folder names it (ch2)"
        Check (Counter-Is $rw "6–7 / 8") "reading: the book goes on across folders (6–7 / 8)"
        Press $rw " "
        Check (Counter-Is $rw "8 / 8") "reading: a two-page picture is shown alone (8 / 8)"
        Press $rw " "
        Check (Has-Text $rw "這是最後一張圖片" 3) "reading: at the end it says so instead of starting again"
        Check (Counter-Is $rw "8 / 8" 2) "reading: and stays on the last page"
        Press $rw "{HOME}"
        Press $rw "{LEFT}"
        Check (Counter-Is $rw "2–3 / 8") "reading: right to left, the left arrow is the next page"
        Press $rw "d"
        Check ($null -ne (Wait-Find $rw "由左至右" 4)) "reading: D switches the direction (left to right)"
        Press $rw "{RIGHT}"
        Check (Counter-Is $rw "4–5 / 8") "reading: left to right, the right arrow is the next page"
        Press $rw "s"
        Check (Counter-Is $rw "4 / 8") "reading: S switches to one page at a time"
        Press $rw "{ESC}"
        Start-Sleep -Milliseconds 800
        $store = Join-Path $exeDir "reading.tsv"
        Check ((Test-Path $store) -and ((Get-Content $store -Raw -Encoding UTF8) -match "ch1/p04\.png\t0\t1")) "reading: closing remembers the page, the direction and one page (reading.tsv)"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 600
    $p = Start-Raw @((Q "$rd\book.cbz"))
    $rw = Wait-Reader $p
    if ($rw) {
        Check (Has-Text $rw "從第 4 頁繼續" 4) "reading: opening the book again continues where it was left"
        Check (Counter-Is $rw "4 / 8") "reading: on that page, one page at a time as it was left"
        Check ($null -ne (Wait-Find $rw "由左至右" 4)) "reading: with the direction it was left in"
        Press $rw "b"
        $tp = $null; $rb = Find $rw "閱讀模式"
        Check ($rb -and $rb.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$tp) -and $tp.Current.ToggleState -eq [Windows.Automation.ToggleState]::Off) "reading: B switches the reading mode off"
    } else { Check $false "reading: the book opens again" }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 600
    # the setting off: a comic book archive opens the file list
    Set-Content $settingsFile "welcome_done=true`ncomic_reading_mode=false" -Encoding UTF8
    $p = Start-Raw @((Q "$rd\book.cbz")); $w = Get-Win $p
    Check ($null -ne (Wait-Find $w "ch1" 12)) "reading: with the setting off a comic book archive opens in the normal window"
    Check ($null -eq (Wait-Reader $p 2)) "reading: and no preview window"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $exeDir "reading.tsv") -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "stripdrag") {
        Reset-State
    # ===== B batch: a picture is dragged out of the thumbnail strip of the preview window into Explorer =====
    $sd = Join-Path $t "stripdrag"; New-Item -ItemType Directory "$sd/src", "$sd/target" -Force | Out-Null
    foreach ($n in 1..3) { $bmp = New-Object System.Drawing.Bitmap (60 + $n * 10), 40; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Orange); $g.Dispose(); $bmp.Save("$sd\src\strip$n.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }
    Push-Location "$sd\src"; 7z a -tzip "$sd\strip.zip" "strip1.png" "strip2.png" "strip3.png" | Out-Null; Pop-Location
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$sd\target`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*target*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "strip drag: an Explorer window for the target folder is open"
    $p = Start-Raw @("--preview-images", (Q "$sd\strip.zip"))
    $pc = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
    $pw = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 300; $pw = $AE::RootElement.FindAll($TS::Children, $pc) | Where-Object { $_.Current.Name -like "strip*.png*" } | Select-Object -First 1 } while (-not $pw -and (Get-Date) -lt $end)
    Check ($null -ne $pw) "strip drag: the preview window is open"
    if ($ex -and $pw) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow([IntPtr]$pw.Current.NativeWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1500
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        $tile = $null
        foreach ($i in $pw.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.Current.Name -eq "1") { $tile = $i; break } }
        Check ($null -ne $tile) "strip drag: the tile of the second picture is there"
        if ($tile) {
            $r = $tile.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 800
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            $end = (Get-Date).AddSeconds(8)
            while (-not (Test-Path "$sd\target\strip2.png") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
            Check (Test-Path "$sd\target\strip2.png") "strip drag: the picture of the tile is in the target folder"
            Check (-not (Test-Path "$sd\target\strip1.png") -and -not (Test-Path "$sd\target\strip3.png")) "strip drag: only that picture"
            if (Test-Path "$sd\target\strip2.png") { Check ((Get-Item "$sd\target\strip2.png").Length -eq (Get-Item "$sd\src\strip2.png").Length) "strip drag: the content is whole" }
        }
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "lockopen") {
        Reset-State
    # ===== B batch: the archive file is locked while its window is open (setting, on by default) =====
    $lo = Join-Path $t "lockopen"; New-Item -ItemType Directory $lo | Out-Null
    Copy-Item "$t\sample.zip" "$lo\a.zip"; Copy-Item "$t\sample.zip" "$lo\b.zip"
    function Can-Write($path) { try { $f = [IO.File]::Open($path, "Open", "ReadWrite", "ReadWrite"); $f.Close(); return $true } catch { return $false } }
    function Can-Read($path) { try { $f = [IO.File]::Open($path, "Open", "Read", "ReadWrite"); $f.Close(); return $true } catch { return $false } }
    $p = Start-App "$lo\a.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    Check (-not (Can-Write "$lo\a.zip")) "lock open: while the window shows the archive, another program cannot write to it"
    Check (Can-Read "$lo\a.zip") "lock open: but it can be read"
    $moved = $true; try { Rename-Item "$lo\a.zip" "a2.zip" -ErrorAction Stop } catch { $moved = $false }
    Check (-not $moved) "lock open: and it cannot be renamed or deleted"
    # changing the archive from the window still works (add an item)
    $new = Join-Path $lo "added.txt"; "x" | Set-Content $new
    $win2 = $win
    Check ($null -ne (Find $win "加入")) "lock open: the Add button is there"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 800
    Check (Can-Write "$lo\a.zip") "lock open: closing the window releases the file"
    # switched off in the settings file: not locked
    Set-Content $settingsFile "welcome_done=true`nlock_open_archive=no" -Encoding UTF8
    $p = Start-App "$lo\b.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $moved2 = $true; try { Rename-Item "$lo\b.zip" "b2.zip" -ErrorAction Stop } catch { $moved2 = $false }
    Check $moved2 "lock open: with the setting off the file can be renamed again (as before: only writing is refused)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "updatecheck") {
        Reset-State
    # ===== the optional check for a new version: a local server answers latest.json (LYCAN_UPDATE_URL) =====
    $uc = Join-Path $t "updatecheck"; New-Item -ItemType Directory $uc -Force | Out-Null
    Set-Content "$uc\latest.json" '{"version": "99.0.0", "released": "2099-01-01", "page": "https://aegisulf.com/lycanzip/download/", "changelog": "https://aegisulf.com/lycanzip/changelog/"}' -Encoding ascii
    $srv = Start-Process python -ArgumentList "-m", "http.server", "18765", "--bind", "127.0.0.1", "--directory", "`"$uc`"" -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $env:LYCAN_UPDATE_URL = "http://127.0.0.1:18765/latest.json"
    try {
        # off by default: no notice, even though the server answers
        Set-Content $settingsFile "welcome_done=true`nview_mode=details" -Encoding UTF8
        $p = Start-App; $win = Get-Win $p; Wait-Find $win "開啟壓縮檔" | Out-Null; Start-Sleep -Seconds 5
        Check ($null -eq (Find $win "有新版本 99.0.0")) "update check: off by default, no notice"
        # the button in the About tab asks anyway and says the result next to it
        Click (Find $win "設定"); Start-Sleep -Milliseconds 900
        Open-SettingsTab $win "關於"; Start-Sleep -Milliseconds 500
        $btn = Wait-Find $win "立即檢查"
        Check ($null -ne $btn) "update check: the 'Check now' button is in the About tab"
        if ($btn) { Click $btn; Start-Sleep -Seconds 4
            Check ($null -ne (Find $win "有新版本 99.0.0")) "update check: the button shows that version 99.0.0 is available" }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 600
        # switched on: the notice comes by itself; the last check time is remembered
        Set-Content $settingsFile "welcome_done=true`nview_mode=details`ncheck_updates=yes" -Encoding UTF8
        $p = Start-App; $win = Get-Win $p
        $note = Wait-Find $win "有新版本 99.0.0"
        Check ($null -ne $note) "update check: switched on, the notice appears by itself"
        Start-Sleep -Milliseconds 800
        Check ((Get-Content $settingsFile -Raw) -match "last_update_check=\d{9,}") "update check: the time of the check is remembered"
        # skip this version
        $skip = Find $win "略過這個版本"
        if ($skip) { Click $skip; Start-Sleep -Milliseconds 800 }
        Check ((Get-Content $settingsFile -Raw) -match "skip_update_version=99\.0\.0") "update check: 'Skip this version' is remembered"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 600
        # the next check is not due for a week, so force it: an old time, the version is skipped
        $cur = (Get-Content $settingsFile -Raw) -replace "last_update_check=\d+", "last_update_check=1"
        Set-Content $settingsFile $cur -Encoding UTF8
        $p = Start-App; $win = Get-Win $p; Wait-Find $win "開啟壓縮檔" | Out-Null; Start-Sleep -Seconds 5
        Check ($null -eq (Find $win "有新版本 99.0.0")) "update check: a skipped version is not announced again"
    } finally {
        Remove-Item Env:\LYCAN_UPDATE_URL -ErrorAction SilentlyContinue
        Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    }
    Reset-State
    }

    if (Want "installupdate") {
        Reset-State
    # ===== the installed copy updates itself: <root>\current\LycanZip.exe with <root>\Update.exe (a stand-in that writes its command line), a signed feed on this computer =====
    $vpkExe = Join-Path $root "build\tools\vpk\vpk.exe"
    $inst = Join-Path $t "inst"; $feedRoot = Join-Path $t "inst_feed"
    if (-not (Test-Path $vpkExe) -or -not (Test-Path (Join-Path $exeDir "velopack_libc.dll")) -or -not (Test-Path (Join-Path $PSScriptRoot "sign-update.ps1"))) {
        Write-Host "installupdate: skipped (needs the installer tools of the release flow: Velopack, its packer and the update signing script)"
    } else {
        New-Item -ItemType Directory "$inst\current", "$inst\settings", $feedRoot -Force | Out-Null
        & robocopy $exeDir "$inst\current" /E /XF settings.ini LycanZip.portable /NFL /NDL /NJH /NJS /NP | Out-Null
        Copy-Item (Join-Path $root "build\debug\bin\echo_args.exe") "$inst\Update.exe"
        $manifest = @'
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd">
<metadata>
<id>Aegisulf.LycanZipTest</id>
<title>LycanZip</title>
<description>LycanZip</description>
<authors>Aegisulf</authors>
<version>0.9.1</version>
<channel>win</channel>
<mainExe>LycanZip.exe</mainExe>
<os>win</os>
<rid>win</rid>
</metadata>
</package>
'@
        Set-Content "$inst\current\sq.version" $manifest -Encoding UTF8
        # two versions of a stand-in program in the feed, signed by a temporary key (and by another key for the second feed)
        $d1 = Join-Path $t "inst_v1"; $d2 = Join-Path $t "inst_v2"; New-Item -ItemType Directory $d1, $d2 -Force | Out-Null
        Copy-Item (Join-Path $root "build\debug\bin\echo_args.exe") "$d1\app.exe"; Copy-Item (Join-Path $root "build\debug\bin\echo_args.exe") "$d2\app.exe"
        Set-Content "$d1\n.txt" ("one " * 500); Set-Content "$d2\n.txt" ("two " * 500)
        $feedMade = Join-Path $t "inst_made"
        foreach ($x in @(@("0.9.1", $d1), @("99.0.0", $d2))) { & $vpkExe pack --packId Aegisulf.LycanZipTest --packVersion $x[0] --packDir $x[1] --mainExe app.exe --runtime win-x64 --outputDir $feedMade --skip-updates | Out-Null }
        function New-InstKey($name) {
            $out = & (Join-Path $PSScriptRoot "new-update-key.ps1") -KeyFile (Join-Path $t "$name.blob") -NoPatch -Force
            return @{ File = (Join-Path $t "$name.blob"); Pub = ((($out | Where-Object { $_ -like "Public key*" }) -split ": ")[1]).Trim() }
        }
        $ik1 = New-InstKey "ikey1"; $ik2 = New-InstKey "ikey2"
        Copy-Item $feedMade "$feedRoot\ok" -Recurse; Copy-Item $feedMade "$feedRoot\bad" -Recurse
        & (Join-Path $PSScriptRoot "sign-update.ps1") -Feed "$feedRoot\ok\releases.win.json" -KeyFile $ik1.File | Out-Null
        & (Join-Path $PSScriptRoot "sign-update.ps1") -Feed "$feedRoot\bad\releases.win.json" -KeyFile $ik2.File | Out-Null
        Set-Content "$feedRoot\latest.json" '{"version": "99.0.0", "released": "2099-01-01", "page": "https://aegisulf.com/lycanzip/download/", "changelog": "https://aegisulf.com/lycanzip/changelog/"}' -Encoding ascii
        $srvI = Start-Process python -ArgumentList "-m", "http.server", "18766", "--bind", "127.0.0.1", "--directory", "`"$feedRoot`"" -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 2
        $saved = @{}; foreach ($n in "LYCAN_UPDATE_URL", "LYCAN_UPDATE_FEED", "LYCAN_TEST_UPDATE_PUBKEY", "LYCAN_SETTINGS_DIR", "LYCAN_ECHO_OUT", "LYCAN_LOG_DIR", "LYCAN_LOG") { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
        $env:LYCAN_UPDATE_URL = "http://127.0.0.1:18766/latest.json"; $env:LYCAN_TEST_UPDATE_PUBKEY = $ik1.Pub
        $env:LYCAN_SETTINGS_DIR = "$inst\settings"; $env:LYCAN_ECHO_OUT = "$inst\echo.txt"; $env:LYCAN_LOG_DIR = "$inst\logs"; $env:LYCAN_LOG = "debug"
        $instExe = "$inst\current\LycanZip.exe"
        try {
            function Start-Inst($feed) { $env:LYCAN_UPDATE_FEED = "http://127.0.0.1:18766/$feed/"; Set-Content "$inst\settings\settings.ini" "welcome_done=true`nview_mode=details`ncheck_updates=yes" -Encoding UTF8; Start-Process $instExe -PassThru }
            # a feed that another key signed: the update is refused, nothing is changed, the program goes on
            $p = Start-Inst "bad"; $win = Get-Win $p
            $note = Wait-Find $win "有新版本 99.0.0"
            Check ($null -ne $note) "install update: an installed copy announces the new version"
            $btn = Wait-Find $win "立即安裝" 5
            Check ($null -ne $btn) "install update: the notice of an installed copy has an 'Install now' button (not a link to the download page)"
            Check ($null -eq (Find $win "下載")) "install update: and no download link"
            if ($btn) {
                Start-Sleep -Seconds 4   # the background download is tried first and refused
                Check (-not (Find $win "重新啟動並更新")) "install update: a feed that fails the signature check is not downloaded in the background (no 'Restart and update')"
                Click $btn; Start-Sleep -Seconds 4
                Check (Has-Text $win "*無法下載更新*" 10) "install update: a feed that fails the signature check is refused, with a message"
                Check (-not $p.HasExited) "install update: and the program goes on"
                Check (-not (Test-Path "$inst\echo.txt")) "install update: the update program was not started"
                Check (-not (Get-ChildItem "$inst\packages" -Filter *.nupkg -ErrorAction SilentlyContinue)) "install update: nothing was downloaded"
            }
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 700
            # the signed feed: the question, the download, the update program is started and the program ends
            $p = Start-Inst "ok"; $win = Get-Win $p
            # the update is downloaded in the background after the automatic check; the notice then offers "Restart and update"
            $btn = Wait-Find $win "重新啟動並更新" 60
            Check ($null -ne $btn) "install update: the update is downloaded in the background and the notice offers 'Restart and update'"
            Check (Has-Text $win "*版本 99.0.0 已可安裝*" 5) "install update: the notice names the version"
            Check ((Get-ChildItem "$inst\packages" -Filter "*99.0.0*.nupkg" -ErrorAction SilentlyContinue)) "install update: the package was downloaded before anything was asked"
            Check (-not (Test-Path "$inst\echo.txt")) "install update: nothing was installed by itself"
            if ($btn) {
                Click $btn
                $exited = Wait-Exit $p 60
                Check $exited "install update: the program ends when the user restarts to update"
                # the update program (Velopack's own, taken out of the package) replaced the folder `current` and started the new version, which here is a stand-in that writes its command line
                $endEcho = (Get-Date).AddSeconds(30); while (-not (Test-Path "$inst\echo.txt") -and (Get-Date) -lt $endEcho) { Start-Sleep -Milliseconds 300 }; Start-Sleep -Milliseconds 800; $echoSeen = Test-Path "$inst\echo.txt"
                Check ($echoSeen -and ((Get-Content "$inst\echo.txt" -Raw -Encoding Unicode) -match "--veloapp-updated 99\.0\.0")) "install update: the update program applied the package and started the new version"
                Check ((Test-Path "$inst\current\app.exe") -and ((Get-Content "$inst\current\sq.version" -Raw) -match "<version>99\.0\.0</version>")) "install update: the folder current now holds version 99.0.0"
                Check ((Get-ChildItem "$inst\packages" -Filter "*99.0.0*.nupkg" -ErrorAction SilentlyContinue)) "install update: the package is in the packages folder (it passed the hash check)"
            } else { Check $false "install update: the 'Install now' button of the signed feed" }
        } finally {
            foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
            Stop-Process -Id $srvI.Id -Force -ErrorAction SilentlyContinue
            Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force
            # Velopack's own update program registers what it applied like an installed app (Installed apps, Start menu and desktop
            # shortcuts of the test package): none of that may stay behind on the machine. It does that after the test is done with it, so
            # first wait (up to 30 s) until nothing runs from the test folder any more
            $endInst = (Get-Date).AddSeconds(30)
            while ((Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($inst, [StringComparison]::OrdinalIgnoreCase) }) -and (Get-Date) -lt $endInst) { Start-Sleep -Milliseconds 500 }
            Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($inst, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
            Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Aegisulf.LycanZipTest" -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Aegisulf.LycanZipTest.lnk", "$([Environment]::GetFolderPath('Desktop'))\Aegisulf.LycanZipTest.lnk" -Force -ErrorAction SilentlyContinue
        }
    }
    Reset-State
    }

    if (Want "virustotal") {
        Reset-State
    # ===== VirusTotal: the SHA-256 of the archive (More options) or of a file in it (right-click) opens the page of the hash; the address goes to a file here, no browser =====
    $vt = Join-Path $t "virustotal"; New-Item -ItemType Directory $vt -Force | Out-Null
    [IO.File]::WriteAllText("$vt\abc.txt", "abc")
    Push-Location $vt; 7z a -tzip "$vt\a.zip" ".\abc.txt" | Out-Null; Pop-Location
    $shaAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    $zipSha = (Get-FileHash "$vt\a.zip" -Algorithm SHA256).Hash.ToLower()
    $urlFile = Join-Path $vt "urls.txt"; Remove-Item $urlFile -ErrorAction SilentlyContinue
    $env:LYCAN_TEST_URL_FILE = $urlFile
    function Url-Count { if (Test-Path $urlFile) { @(Get-Content $urlFile).Count } else { 0 } }
    function Wait-Url($want, [int]$sec = 15) { $end = (Get-Date).AddSeconds($sec); do { if ((Test-Path $urlFile) -and (@(Get-Content $urlFile) -contains $want)) { return $true }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $end); return $false }
    function Right-Click($el) { $r = $el.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300; [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700 }
    $p = Start-App "$vt\a.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    # the archive: More options
    Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
    # (The tooltips of the menu items are texts of the catalog now: "Scan for malware" showed its string id once. A tooltip window cannot be read reliably through UI Automation, so it is not tested here.)
    $vtItem = Find $win "在 VirusTotal 查詢"
    Check ($null -ne $vtItem) "virustotal: More options has the lookup"
    if ($vtItem) { Click $vtItem }
    Check (Wait-Url "https://www.virustotal.com/gui/search/$zipSha") "virustotal: the page of the SHA-256 of the archive file is opened"
    Start-Sleep -Milliseconds 800
    Check ($null -eq (Find $win "貼上要比對的雜湊值")) "virustotal: the small window closes by itself once the page is opened"
    # a file in the archive
    $row = Find-Row $win "abc.txt"
    Check ($null -ne $row) "virustotal: the file is listed"
    if ($row) {
        Right-Click $row
        $vtFile = Find $win "在 VirusTotal 查詢"
        Check ($null -ne $vtFile) "virustotal: the right-click menu of a file has the lookup"
        if ($vtFile) { Click $vtFile }
        Check (Wait-Url "https://www.virustotal.com/gui/search/$shaAbc") "virustotal: the page of the SHA-256 of the file (calculated in the archive) is opened"
    }
    # the hash dialog: a button next to the SHA-256 value
    Start-Sleep -Milliseconds 800
    $before = Url-Count
    Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
    Click (Wait-Find $win "計算壓縮檔的雜湊值…" 5)
    Check (Has-Text $win "*$zipSha*" 12) "virustotal: the hash dialog shows the SHA-256"
    $btn = Find $win "在 VirusTotal 查詢"
    Check ($null -ne $btn) "virustotal: and a button to look it up"
    if ($btn) { Click $btn; Start-Sleep -Milliseconds 800 }
    Check ((Url-Count) -eq ($before + 1)) "virustotal: the button opens the page (once)"
    Click (Find $win "關閉"); Start-Sleep -Milliseconds 400
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_URL_FILE
    Reset-State
    }

    if (Want "colscroll") {
        Reset-State
    # ===== The details view scrolls sideways when the columns are wider than the window (the header follows), and the width of a column can be dragged (a double click restores it) =====
    $cs2 = Join-Path $t "colscroll"; New-Item -ItemType Directory $cs2 -Force | Out-Null
    Copy-Item "$t\sample.zip" "$cs2\sample.zip"
    Set-Content $settingsFile "welcome_done=true`nview_mode=details`nlist_columns=15`nlist_col_widths=name=500" -Encoding UTF8
    $p = Start-App "$cs2\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $colList = Find $win "壓縮檔內容"
    $sp = $null
    $can = $colList -and $colList.TryGetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern, [ref]$sp) -and $sp.Current.HorizontallyScrollable
    Check ([bool]$can) "colscroll: wider columns than the window: the list scrolls sideways (no column is hidden)"
    foreach ($n in "依名稱排序", "依類型排序", "依大小排序", "依壓縮後大小排序", "依修改時間排序") { Check ($null -ne (Find $win $n)) "colscroll: the header '$n' is there although the window is narrow" }
    # the name column is 500 wide: at the left only the name header is in the window; scrolled to the end the name header is gone and the last column shows
    $hdrName = Find $win "依名稱排序"; $hdrMod = Find $win "依修改時間排序"; $hdrType = Find $win "依類型排序"
    Check ((-not $hdrName.Current.IsOffscreen) -and $hdrMod.Current.IsOffscreen) "colscroll: at the left the name header shows and the last column is out of the window"
    if ($can) {
        $sp.SetScrollPercent(100, -1); Start-Sleep -Milliseconds 900
        Check ((-not (Find $win "依修改時間排序").Current.IsOffscreen) -and (Find $win "依名稱排序").Current.IsOffscreen) "colscroll: scrolled to the end the header shows the last column (the header follows the list)"
        $t2 = (Find $win "依類型排序").Current.BoundingRectangle; $s2 = (Find $win "依大小排序").Current.BoundingRectangle
        Check (($s2.X - $t2.X) -gt 30 -and ($s2.X - $t2.X) -lt 600) "colscroll: the column headers keep their order and distance when scrolled"
        # a row's name is scrolled out of the window together with its header
        $sp.SetScrollPercent(0, -1); Start-Sleep -Milliseconds 700
    }
    # drag the grip at the right edge of the type column
    $grips = @($win.FindAll($TS::Descendants, (ByName "調整欄位寬度")) | Sort-Object { $_.Current.BoundingRectangle.X })
    Check ($grips.Count -ge 4) "colscroll: the column headers have handles to drag ($($grips.Count))"
    if ($grips.Count -ge 4) {
        $g = $grips[1]; $gr = $g.Current.BoundingRectangle   # name, type, size, ...
        $sizeBefore = (Find $win "依大小排序").Current.BoundingRectangle.X
        $scrC = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        function MoveAbsCS($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scrC.Width), [uint32]($y * 65535 / $scrC.Height), 0, [UIntPtr]::Zero) }
        $gx = [int]($gr.X + $gr.Width / 2); $gy = [int]($gr.Y + $gr.Height / 2)
        MoveAbsCS $gx $gy; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        foreach ($dx in 20, 40, 60, 80) { MoveAbsCS ($gx + $dx) $gy; Start-Sleep -Milliseconds 80 }
        [Win.D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 800
        $sizeAfter = (Find $win "依大小排序").Current.BoundingRectangle.X
        Check (($sizeAfter - $sizeBefore) -gt 50) "colscroll: dragging the handle of the type column widens it ($([int]$sizeBefore) -> $([int]$sizeAfter))"
        Check ((Get-Content $settingsFile -Raw) -match "list_col_widths=[^\r\n]*type=\d+") "colscroll: the width is saved (list_col_widths has type=)"
        # a double click on the handle restores the default width
        $grips = @($win.FindAll($TS::Descendants, (ByName "調整欄位寬度")) | Sort-Object { $_.Current.BoundingRectangle.X })
        $gr = $grips[1].Current.BoundingRectangle; $gx = [int]($gr.X + $gr.Width / 2); $gy = [int]($gr.Y + $gr.Height / 2)
        MoveAbsCS $gx $gy; Start-Sleep -Milliseconds 300
        foreach ($i in 1..2) { [Win.D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }
        Start-Sleep -Milliseconds 800
        $sizeBack = (Find $win "依大小排序").Current.BoundingRectangle.X
        Check ([math]::Abs($sizeBack - $sizeBefore) -lt 6) "colscroll: a double click on the handle restores the default width ($([int]$sizeBack))"
        Check ((Get-Content $settingsFile -Raw) -notmatch "list_col_widths=[^\r\n]*type=") "colscroll: and the saved width of the column is gone"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "newcols") {
        Reset-State
    # ===== The columns that are off by default: ratio, encrypted, method, CRC (from the column menu); each one sorts =====
    $nc = Join-Path $t "newcols"; New-Item -ItemType Directory "$nc\src" -Force | Out-Null
    [IO.File]::WriteAllText("$nc\src\abc.txt", "abc")
    [IO.File]::WriteAllText("$nc\src\big.txt", ("hello " * 400))
    [IO.File]::WriteAllText("$nc\src\mid.txt", ("0123456789" * 7))
    Push-Location "$nc\src"; 7z a -tzip "$nc\a.zip" abc.txt big.txt mid.txt | Out-Null; Pop-Location
    Set-Content $settingsFile "welcome_done=true`nview_mode=details`nlist_columns=11" -Encoding UTF8
    $p = Start-App "$nc\a.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
    Check (($null -eq (Find $win "依壓縮率排序")) -and ($null -eq (Find $win "依 CRC 排序")) -and ($null -eq (Find $win "依方式排序")) -and ($null -eq (Find $win "依加密排序"))) "newcols: the new columns are off by default"
    # tick them in the menu of the column headers
    $hd = Find $win "依名稱排序"; $hr = $hd.Current.BoundingRectangle
    [Win.D]::SetCursorPos([int]($hr.X + $hr.Width + 300), [int]($hr.Y + $hr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 400
    [Win.D]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 800
    function Find-Menu($n) { $c = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)), (ByName $n)); return $AE::RootElement.FindFirst($TS::Descendants, $c) }
    foreach ($name in "壓縮率", "方式", "CRC") {
        $mi = Find-Menu $name
        if (-not $mi) { Check $false "newcols: the column menu offers '$name'"; continue }
        try { $mi.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle() } catch { try { $mi.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() } catch { } }
        Start-Sleep -Milliseconds 700
        # the menu closes after a click: open it again for the next one
        [Win.D]::SetCursorPos([int]($hr.X + $hr.Width + 300), [int]($hr.Y + $hr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); [Win.D]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 700
    }
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 500
    Check ((Get-Content $settingsFile -Raw) -match "list_columns=(\d+)") "newcols: the choice is saved"
    $cols = [int]([regex]::Match((Get-Content $settingsFile -Raw), "list_columns=(\d+)").Groups[1].Value)
    Check ((($cols -band 16) -ne 0) -and (($cols -band 64) -ne 0) -and (($cols -band 128) -ne 0)) "newcols: ratio (16), CRC (64) and method (128) are in list_columns ($cols)"
    Check (($null -ne (Find $win "依壓縮率排序")) -and ($null -ne (Find $win "依 CRC 排序")) -and ($null -ne (Find $win "依方式排序"))) "newcols: the three headers are there"
    function Row-Texts() {
        $out = @()
        foreach ($it in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) {
            $tx = @(); foreach ($e in $it.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Text)))) { $tx += $e.Current.Name }
            if ($tx -contains "TXT") { $out += ,$tx }
        }
        return $out
    }
    $rows = Row-Texts
    $abc = $rows | Where-Object { $_ -contains "abc.txt" } | Select-Object -First 1
    Check (($null -ne $abc) -and ($abc -contains "352441C2")) "newcols: the CRC column shows the CRC-32 of the file (abc = 352441C2)"
    $big = $rows | Where-Object { $_ -contains "big.txt" } | Select-Object -First 1
    Check (($null -ne $big) -and (@($big | Where-Object { $_ -match '^\d+%$' }).Count -eq 1) -and ([int](($big | Where-Object { $_ -match '^\d+%$' }) -replace '%','') -lt 20)) "newcols: the ratio column shows a percentage (big.txt packs well)"
    Check (($null -ne $big) -and (@($big | Where-Object { $_ -match 'Deflate|Store|Copy|LZMA' }).Count -ge 1)) "newcols: the method column shows the method ($(if ($big) { ($big | Where-Object { $_ -match 'Deflate|Store|Copy|LZMA' }) -join ',' }))"
    # sort by CRC: ascending, then descending
    Click (Find $win "依 CRC 排序"); Start-Sleep -Milliseconds 800
    $crcs = @(Row-Texts | ForEach-Object { $_ | Where-Object { $_ -match '^[0-9A-F]{8}$' } })
    $asc = ($crcs -join ",") -eq ((@($crcs) | Sort-Object { [Convert]::ToUInt32($_, 16) }) -join ",")
    Check ($crcs.Count -eq 3 -and $asc) "newcols: sorting by CRC puts the values in ascending order ($($crcs -join ' '))"
    Click (Find $win "依 CRC 排序"); Start-Sleep -Milliseconds 800
    $crcs2 = @(Row-Texts | ForEach-Object { $_ | Where-Object { $_ -match '^[0-9A-F]{8}$' } })
    $desc = ($crcs2 -join ",") -eq ((@($crcs2) | Sort-Object { [Convert]::ToUInt32($_, 16) } -Descending) -join ",")
    Check ($crcs2.Count -eq 3 -and $desc) "newcols: and again in descending order"
    # sort by ratio: the best packed (smallest %) first
    Click (Find $win "依壓縮率排序"); Start-Sleep -Milliseconds 800
    $pct = @(Row-Texts | ForEach-Object { $_ | Where-Object { $_ -match '^\d+%$' } } | ForEach-Object { [int]($_ -replace '%', '') })
    Check ($pct.Count -eq 3 -and (($pct -join ",") -eq ((@($pct) | Sort-Object) -join ","))) "newcols: sorting by ratio puts the best packed first ($($pct -join ' '))"
    # the Properties window of a file: the technical information of the archive
    function Right-ClickNc($el) { $r = $el.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300; [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700 }
    $row = Find-Row $win "abc.txt"
    if ($row) {
        Right-ClickNc $row
        $prop = Find $win "內容"
        Check ($null -ne $prop) "newcols: the right-click menu of a file has Properties"
        if ($prop) {
            Click $prop
            Check (Has-Text $win "*技術資訊*" 8) "newcols: Properties has a technical information part"
            Check (Has-Text $win "*壓縮方式：*" 3) "newcols: it tells the method"
            Check (Has-Text $win "*從壓縮檔的第*位元組開始*" 3) "newcols: and where the item starts in the archive"
            Click (Find $win "確定"); Start-Sleep -Milliseconds 500
        }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "solidstream") {
        Reset-State
    # ===== C batch: several files dragged out of a SOLID archive are fed by one pass over the archive (no unpacking to the temporary folder first) =====
    $ss = Join-Path $t "solidstream"; New-Item -ItemType Directory "$ss/src", "$ss/target" -Force | Out-Null
    $rng = New-Object System.Random 42
    foreach ($n in 1..4) { $b = New-Object byte[] (1500000 + $n * 100000); $rng.NextBytes($b); [IO.File]::WriteAllBytes("$ss\src\part$n.bin", $b) }
    Push-Location "$ss\src"; 7z a -t7z -ms=on "$ss\solid.7z" part1.bin part2.bin part3.bin part4.bin | Out-Null; Pop-Location
    $tmpRoot = Join-Path $env:TEMP "LycanZip\open"
    function Drag-Dirs2 { @(Get-ChildItem $tmpRoot -Filter "drag-*" -ErrorAction SilentlyContinue).Count }
    $p = Start-App "$ss\solid.7z"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$ss\target`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*target*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "solid stream: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        $row = $null
        foreach ($i in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.FindFirst($TS::Descendants, (ByName "part2.bin"))) { $row = $i; break } }
        Check ($null -ne $row) "solid stream: the rows of the archive are there"
        if ($row) {
            $r = $row.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 400
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 500
            [System.Windows.Forms.SendKeys]::SendWait("^a"); Start-Sleep -Milliseconds 600
            $before = Drag-Dirs2
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            $t0 = Get-Date
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            $end = (Get-Date).AddSeconds(40)
            while ((@(Get-ChildItem "$ss\target" -Filter "part*.bin" -ErrorAction SilentlyContinue | Where-Object { $_.Length -eq (Get-Item "$ss\src\$($_.Name)").Length }).Count -lt 4) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 400 }
            $ok = $true; foreach ($n in 1..4) { if (-not (Test-Path "$ss\target\part$n.bin") -or (Get-Item "$ss\target\part$n.bin").Length -ne (Get-Item "$ss\src\part$n.bin").Length) { $ok = $false } }
            Check $ok "solid stream: all four files arrived whole ($([int]((Get-Date) - $t0).TotalSeconds) s)"
            if ($ok) { Check ((Get-FileHash "$ss\target\part3.bin").Hash -eq (Get-FileHash "$ss\src\part3.bin").Hash) "solid stream: and the content is right" }
            Check ((Drag-Dirs2) -le $before) "solid stream: nothing was unpacked to the temporary folder first"
        }
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "convertwall") {
        Reset-State
    # ===== C batch: the picture preview saves a picture in another format, and sets it as the desktop picture =====
    # (the save dialog and the real desktop picture are replaced by LYCAN_TEST_SAVE_IMAGE and LYCAN_TEST_WALLPAPER_DIR)
    $cw = Join-Path $t "convertwall"; New-Item -ItemType Directory "$cw/src", "$cw/out" -Force | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 120, 80; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Transparent); $g.FillEllipse([System.Drawing.Brushes]::OrangeRed, 10, 10, 90, 60); $g.Dispose()
    $bmp.Save("$cw\src\conv1.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Push-Location "$cw\src"; 7z a -tzip "$cw\conv.zip" conv1.png | Out-Null; Pop-Location
    function Top-Windows3($proc) { $c = New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id); @($AE::RootElement.FindAll($TS::Children, $c) | Where-Object { -not $_.Current.IsOffscreen -or $_.Current.Name }) }
    function Wait-Window3($proc, [string]$like, [int]$sec = 12) {
        $end = (Get-Date).AddSeconds($sec)
        do { foreach ($w in (Top-Windows3 $proc)) { if ($w.Current.Name -like $like) { return $w } }; Start-Sleep -Milliseconds 250 } while ((Get-Date) -lt $end)
        return $null
    }
    function Magic($path, [int]$n) { $b = [IO.File]::ReadAllBytes($path); ($b[0..($n - 1)] | ForEach-Object { $_.ToString("X2") }) -join "" }
    $env:LYCAN_TEST_MUTE = "1"
    foreach ($case in @(@("jpg", "FFD8FF"), @("bmp", "424D"), @("gif", "47494638"), @("png", "89504E47"))) {
        $target = "$cw\out\saved." + $case[0]
        if (Test-Path $target) { Remove-Item $target }
        $env:LYCAN_TEST_SAVE_IMAGE = $target
        $p = Start-Raw @("--preview-images", (Q "$cw\conv.zip"))
        $pw = Wait-Window3 $p "conv1.png*"
        if ($pw) { Start-Sleep -Milliseconds 1200; [System.Windows.Forms.SendKeys]::SendWait("^s") }
        $end = (Get-Date).AddSeconds(10); while (-not (Test-Path $target) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
        Start-Sleep -Milliseconds 400
        Check (Test-Path $target) "convert: saving as .$($case[0]) writes the file"
        if (Test-Path $target) {
            Check ((Magic $target ($case[1].Length / 2)) -eq $case[1]) "convert: the .$($case[0]) file is really that format"
            try { $img = [System.Drawing.Image]::FromFile($target); $okSize = ($img.Width -eq 120 -and $img.Height -eq 80); $img.Dispose() } catch { $okSize = $false }
            Check $okSize "convert: and it is the same picture (120 x 80)"
        }
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item Env:LYCAN_TEST_SAVE_IMAGE -ErrorAction SilentlyContinue
    # the desktop picture: the question, then the copy of the picture is kept
    $wd = Join-Path $cw "wall"; $env:LYCAN_TEST_WALLPAPER_DIR = $wd
    $p = Start-Raw @("--preview-images", (Q "$cw\conv.zip"))
    $pw = Wait-Window3 $p "conv1.png*"
    Check ($null -ne $pw) "wallpaper: the preview window is open"
    if ($pw) {
        # the menu: right click on the picture opens the same one
        $r = $pw.Current.BoundingRectangle
        [Win.D]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 800
        $item = $null; $end = (Get-Date).AddSeconds(6)
        do { foreach ($e in $AE::RootElement.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)))) { try { if ($e.Current.Name -like "設成桌布*") { $item = $e; break } } catch { } }; if (-not $item) { Start-Sleep -Milliseconds 300 } } while (-not $item -and (Get-Date) -lt $end)
        Check ($null -ne $item) "wallpaper: the menu has 'set as desktop picture'"
        if ($item) {
            Click $item; Start-Sleep -Milliseconds 1200
            $btn = $null; $end = (Get-Date).AddSeconds(6)
            do { foreach ($e in $pw.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))) { if ($e.Current.Name -eq "設成桌布") { $btn = $e; break } }; if (-not $btn) { Start-Sleep -Milliseconds 300 } } while (-not $btn -and (Get-Date) -lt $end)
            Check ($null -ne $btn) "wallpaper: the question has the button 'set as desktop picture'"
            if ($btn) {
                Check ($null -ne (Find $pw "圖片位置")) "wallpaper: and the choice of the position"
                Click $btn
                $end = (Get-Date).AddSeconds(8); while (-not (Test-Path $wd) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
                Start-Sleep -Milliseconds 800
                $saved = @(Get-ChildItem $wd -Filter "wallpaper-*.png" -ErrorAction SilentlyContinue)
                Check ($saved.Count -eq 1 -and $saved[0].Length -gt 100) "wallpaper: a copy of the picture is kept in the folder"
            }
        }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_WALLPAPER_DIR, Env:LYCAN_TEST_MUTE -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "shellitems") {
        Reset-State
    # ===== 34. Settings > Explorer: which context-menu entries are shown =====
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "檔案總管"
    function Shell-Box($name) {
        $c = New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::CheckBox)))
        $win.FindFirst($TS::Descendants, $c)
    }
    function Box-On($el) { $el.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On" }
    function Settings-Text { if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw -Encoding UTF8 } else { "" } }
    $add = Shell-Box "加入壓縮檔…"; $audio = Shell-Box "音訊預覽"; $imgs = Shell-Box "圖形預覽"
    Check (($null -ne $add) -and ($null -ne $audio) -and ($null -ne $imgs)) "shell items: the check boxes exist"
    Check ((Box-On $add) -and (Box-On $imgs) -and -not (Box-On $audio)) "shell items: defaults (add and image preview on, audio preview off)"
    # turning the audio preview on and the add entry off is saved as a list
    $audio.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 500
    $add.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 700
    $st = Settings-Text
    Check ($st -match "shell_items=([a-z0-9,]+)") "shell items: shell_items is written once something differs from the defaults"
    $items = if ($Matches) { $Matches[1] -split "," } else { @() }
    Check (($items -contains "audio") -and ($items -notcontains "add") -and ($items -contains "fmt1")) "shell items: the list has audio, not add, and keeps the other defaults ($($items -join ','))"
    # back to the defaults: the key becomes empty (the defaults follow future changes)
    $audio.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 500
    $add.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 700
    Check ((Settings-Text) -match "(?m)^shell_items=\s*$") "shell items: all defaults again = empty value"
    # the formats of the two 'Compress to' entries
    Select-Combo $win "第一個「壓縮成」的格式" "tar.gz"; Start-Sleep -Milliseconds 700
    Select-Combo $win "第二個「壓縮成」的格式" "ZIP"; Start-Sleep -Milliseconds 700
    $st = Settings-Text
    Check (($st -match "shell_format_1=tar.gz") -and ($st -match "shell_format_2=zip")) "shell items: the formats are saved"
    Shot $p "settings-shellitems"
    Stop-Process -Id $p.Id -Force
    # after a restart the choices are still there
    $p = Start-App "$t\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    Open-SettingsTab $win "檔案總管"
    $combo1 = Find-Combo $win "第一個「壓縮成」的格式"
    Check (($null -ne $combo1) -and ((Combo-Selected $combo1) -like "tar.gz*")) "shell items: the format is remembered after a restart"
    Stop-Process -Id $p.Id -Force
    Reset-State
    }

    if (Want "macmeta") {
        Reset-State
    # ===== 35. macOS metadata (__MACOSX, ._ files, .DS_Store) and Thumbs.db =====
    $mm = Join-Path $t "macmeta"; New-Item -ItemType Directory "$mm/src/proj", "$mm/src/__MACOSX/proj", "$mm/thumbs/sub" -Force | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 64, 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
    $bmp.Save("$mm\src\proj\pic1.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Set-Content "$mm\src\proj\._pic1.png" "appledouble" -Encoding ascii
    Set-Content "$mm\src\proj\.DS_Store" "x" -Encoding ascii
    Set-Content "$mm\src\__MACOSX\proj\._pic1.png" "appledouble" -Encoding ascii
    Push-Location "$mm\src"; 7z a -tzip "$mm\mac.zip" ".\proj" ".\__MACOSX" | Out-Null; Pop-Location
    Set-Content "$mm\thumbs\a.txt" "a" -Encoding ascii; Set-Content "$mm\thumbs\Thumbs.db" "cache" -Encoding ascii; Set-Content "$mm\thumbs\sub\thumbs.DB" "cache" -Encoding ascii; Set-Content "$mm\thumbs\sub\b.txt" "b" -Encoding ascii

    # browsing: the junk is hidden by default, the View menu shows it
    $p = Start-App "$mm\mac.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 900
    # one real folder left: the view opens it (the single-folder rule), so its files are what is listed
    Check ($null -ne (Find-Row $win "pic1.png")) "macmeta: the real folder is opened and its file is listed"
    Check (($null -eq (Find-Row $win "._pic1.png")) -and ($null -eq (Find-Row $win ".DS_Store"))) "macmeta: ._ files and .DS_Store are hidden by default"
    Click (Find $win "檢視"); Start-Sleep -Milliseconds 500
    $item = Find $win "顯示 macOS 中繼資料"
    Check ($null -ne $item) "macmeta: the View menu has 'show macOS metadata'"
    if ($item) { Click $item; Start-Sleep -Milliseconds 900 }
    Check (($null -ne (Find-Row $win "._pic1.png")) -and ($null -ne (Find-Row $win ".DS_Store"))) "macmeta: the View menu choice shows the ._ file and .DS_Store"
    Click (Find $win "上一層"); Start-Sleep -Milliseconds 700
    Check ($null -ne (Find-Row $win "__MACOSX")) "macmeta: one level up the __MACOSX folder is listed"
    Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "hide_mac_metadata=false") "macmeta: the choice is saved"
    # the preview never collects the junk, even while it is shown
    Double-Click (Row-Text $win "proj"); Start-Sleep -Milliseconds 800
    $imgRow = Find-Row $win "pic1.png"
    if ($imgRow) {
        $imgRow.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        Check ($null -ne $pw) "macmeta: the image preview opens"
        if ($pw) {
            Check ($null -ne (Wait-Find $pw "1 / 1" 8)) "macmeta: the preview has one image (the ._ file is not collected)"
            [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600
        }
    }
    Stop-Process -Id $p.Id -Force

    # extraction leaves the junk out, and the one real folder is placed as it is
    $out = Join-Path $mm "out"; New-Item -ItemType Directory $out | Out-Null
    Copy-Item "$mm\mac.zip" "$out\mac.zip"
    Reset-State
    $p = Start-Raw @("--extract-here", (Q "$out\mac.zip"))
    Check (Wait-Exit $p 25) "macmeta: extract-here finishes"
    Check ((Test-Path "$out\proj\pic1.png") -and -not (Test-Path "$out\mac")) "macmeta: the real folder is placed directly (no wrapping folder)"
    Check (-not (Test-Path "$out\__MACOSX") -and -not (Test-Path "$out\proj\._pic1.png") -and -not (Test-Path "$out\proj\.DS_Store")) "macmeta: the metadata was not extracted"

    # settings: the switches exist; turning extraction skipping off keeps the junk
    Reset-State
    $p = Start-App "$mm\mac.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 600
    Click (Find $win "設定"); Start-Sleep -Milliseconds 900
    function Tog($name) { $c = New-Object Windows.Automation.AndCondition((ByName $name), (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::Button))); $win.FindFirst($TS::Descendants, $c) }
    Open-SettingsTab $win "壓縮"
    $skipMeta = Tog "解壓縮時略過 macOS 中繼資料"; $skipThumbs = Tog "自動略過 Thumbs.db"
    Check (($null -ne $skipMeta) -and ($null -ne $skipThumbs)) "macmeta: the extraction and Thumbs.db switches exist"
    Check (($skipMeta.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On") -and ($skipThumbs.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq "On")) "macmeta: both are on by default"
    $skipMeta.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle(); Start-Sleep -Milliseconds 700
    Open-SettingsTab $win "檢視"
    Check ($null -ne (Tog "隱藏 macOS 中繼資料")) "macmeta: the hide switch is on the View tab"
    Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "skip_mac_metadata=false") "macmeta: skip_mac_metadata is saved"
    Stop-Process -Id $p.Id -Force
    $out2 = Join-Path $mm "out2"; New-Item -ItemType Directory $out2 | Out-Null
    Copy-Item "$mm\mac.zip" "$out2\mac.zip"
    $p = Start-Raw @("--extract-here", (Q "$out2\mac.zip"))
    Check (Wait-Exit $p 25) "macmeta: extract-here (skipping off) finishes"
    Check ((Test-Path "$out2\mac\__MACOSX") -and (Test-Path "$out2\mac\proj\._pic1.png")) "macmeta: with skipping off everything is extracted (in a folder named after the archive: two roots)"

    # compression: Thumbs.db stays out of added folders
    Reset-State
    $p = Start-Raw @("--compress", "zip", (Q "$mm\thumbs"))
    Check (Wait-Exit $p 25) "macmeta: compress finishes"
    $names = (7z l -ba "$mm\thumbs.zip" | ForEach-Object { ($_ -split '\s+')[-1] }) -join ","
    Check (($names -match "a.txt") -and ($names -match "b.txt") -and ($names -notmatch "(?i)thumbs.db")) "macmeta: Thumbs.db is not in the new archive ($names)"
    Reset-State
    }

    if (Want "dupnames") {
        Reset-State
    # ===== 36. Files with the same name in different folders: the thumbnail strip follows the right one =====
    $dn = Join-Path $t "dupnames"
    foreach ($i in 1, 2, 3) {
        New-Item -ItemType Directory "$dn\src\f$i" -Force | Out-Null
        $bmp = New-Object System.Drawing.Bitmap (64 * $i), 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
        $bmp.Save("$dn\src\f$i\pic.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    Push-Location "$dn\src"; 7z a -tzip "$dn\dup.zip" ".\f1" ".\f2" ".\f3" | Out-Null; Pop-Location
    $p = Start-Raw @("--preview-images", (Q "$dn\dup.zip"))
    $pw = $null; $end = (Get-Date).AddSeconds(15); do { foreach ($w in ($AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id))))) { if ($w.Current.Name -like "pic.png*") { $pw = $w } }; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
    Check ($null -ne $pw) "dupnames: the preview opens"
    if ($pw) {
        function Selected-Tile { $tiles = @($pw.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))); $r = @(); for ($i = 0; $i -lt $tiles.Count; $i++) { if ($tiles[$i].GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $r += $i } }; return $r }
        Check ($null -ne (Wait-Find $pw "1 / 3" 8)) "dupnames: three pictures with the same name are listed (1 / 3)"
        Check ((@(Selected-Tile) -join ",") -eq "0") "dupnames: the first tile is selected ($(@(Selected-Tile) -join ','))"
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 900
        [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 900
        Check ($null -ne (Wait-Find $pw "3 / 3" 8)) "dupnames: Right twice shows 3 / 3"
        Check ((@(Selected-Tile) -join ",") -eq "2") "dupnames: the third tile (not the first with the same name) is selected ($(@(Selected-Tile) -join ','))"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "dragorder") {
        Reset-State
    # ===== 36b. Dragging out a folder of an archive that lists folders after what is in them (a real case: a game's zip): Explorer
    # must not ask "the destination already has a folder named ..." for every folder =====
    $dg = Join-Path $t "dragorder"; $dn = "lzorder" + (Get-Date -Format HHmmss); New-Item -ItemType Directory "$dg\$dn" | Out-Null
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zs = [IO.File]::Create("$dg\order.zip"); $za = New-Object IO.Compression.ZipArchive($zs, [IO.Compression.ZipArchiveMode]::Create)
    foreach ($n in "top/a/b/c/", "top/a/b/c/deep.txt", "top/", "top/a/", "top/x.txt", "top/a/b/", "top/a/y.txt", "other/") {
        $en = $za.CreateEntry($n); if (-not $n.EndsWith("/")) { $sw = New-Object IO.StreamWriter($en.Open()); $sw.Write("content of $n"); $sw.Dispose() }
    }
    $za.Dispose(); $zs.Dispose()
    $p = Start-App "$dg\order.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$dg\$dn`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*$dn*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "drag order: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        $row = $null; foreach ($i in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.FindFirst($TS::Descendants, (ByName "top"))) { $row = $i; break } }
        Check ($null -ne $row) "drag order: the folder row is there"
        if ($row) {
            $r = $row.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 3
        }
        # a question of Explorer's copy engine (a dialog of the explorer process): note it and close it
        $exPid = $ex.Current.ProcessId; $asked = @()
        foreach ($w in $AE::RootElement.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $exPid)))) {
            if ($w.Current.ClassName -ne "CabinetWClass" -and $w.Current.ClassName -ne "Shell_TrayWnd" -and $w.Current.Name) { $asked += $w.Current.Name; try { $w.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { } }
        }
        Check ($asked.Count -eq 0) "drag order: Explorer asks nothing about folders that are already there ($($asked -join ' / '))"
        $all = { (Test-Path "$dg\$dn\top\a\b\c\deep.txt") -and (Test-Path "$dg\$dn\top\x.txt") -and (Test-Path "$dg\$dn\top\a\y.txt") }
        $end = (Get-Date).AddSeconds(15); while (-not (& $all) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 400 }   # Explorer copies in the background
        Check (& $all) "drag order: the whole tree arrived ($((Get-ChildItem "$dg\$dn" -Recurse -File | ForEach-Object Name) -join ', '))"
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }

    if (Want "dragtemp") {
        Reset-State
    # ===== 37. Dragging out with the setting 'stream files' off: the files are unpacked to the temporary folder first =====
    Set-Content $settingsFile "drag_out_virtual=false`n" -Encoding ascii
    $dg = Join-Path $t "dragtemp"; $dn = "lztemp" + (Get-Date -Format HHmmss); New-Item -ItemType Directory "$dg\$dn" | Out-Null; Copy-Item "$t\sample.zip" "$dg\sample.zip"
    $tmpRoot = Join-Path $env:TEMP "LycanZip\open"
    function Drag-Dirs { @(Get-ChildItem $tmpRoot -Filter "drag-*" -ErrorAction SilentlyContinue).Count }
    $p = Start-App "$dg\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 800
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$dg\$dn`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*$dn*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "drag to temp: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        $row = $null; foreach ($i in $win.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)))) { if ($i.FindFirst($TS::Descendants, (ByName "readme.txt"))) { $row = $i; break } }
        $before = Drag-Dirs
        if ($row) {
            $r = $row.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 3
        }
        $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$dg\$dn\readme.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 400 }  # the unpacking happens when Explorer asks: give it time
        Check (Test-Path "$dg\$dn\readme.txt") "drag to temp: the file is extracted at the target"
        Check ((Drag-Dirs) -gt $before) "drag to temp: the files were unpacked to the temporary folder first"
        # the root of the folder tree: a folder named after the archive (through the temporary folder as well)
        $tr = Find $win "資料夾樹"; $rootText = $null
        foreach ($e in $tr.FindAll($TS::Descendants, (ByName "sample.zip"))) { if ($e.Current.ControlType.ProgrammaticName -eq "ControlType.Text") { $rootText = $e } }
        if ($rootText) {
            $r = $rootText.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 3
        }
        $end = (Get-Date).AddSeconds(20); while (-not (Test-Path "$dg\$dn\sample\readme.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 400 }
        Check (Test-Path "$dg\$dn\sample\readme.txt") "drag to temp: the root of the tree dropped on Explorer is a folder named after the archive"
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "treemenu") {
        Reset-State
    # ===== 38. The folder tree: right-click menus, delete the archive, drag a folder / the archive out =====
    $tm = Join-Path $t "treemenu"; New-Item -ItemType Directory $tm -Force | Out-Null
    Copy-Item "$t\sample.zip" "$tm\sample.zip"
    $p = Start-App "$tm\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    function Tree-Text($name) { $tr = Find $win "資料夾樹"; foreach ($e in $tr.FindAll($TS::Descendants, (ByName $name))) { if ($e.Current.ControlType.ProgrammaticName -eq "ControlType.Text") { return $e } }; return $null }
    function Right-Click($el) { $r = $el.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300; [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700 }
    $root = Tree-Text "sample.zip"
    Check ($null -ne $root) "tree: the root node shows the archive's name"
    if ($root) {
        Right-Click $root
        Check (($null -ne (Find $win "刪除壓縮檔…")) -and ($null -ne (Find $win "內容")) -and ($null -ne (Find $win "在檔案總管顯示")) -and ($null -ne (Find $win "測試完整性"))) "tree: right-click on the root: the archive's menu (delete, properties, show in Explorer, test)"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    }
    $folder = Tree-Text "資料夾"
    Check ($null -ne $folder) "tree: the folder node is listed"
    if ($folder) {
        Right-Click $folder
        Check (($null -ne (Find $win "解壓選取項目到此處")) -and ($null -ne (Find $win "複製路徑")) -and ($null -ne (Find $win "開啟資料夾"))) "tree: right-click on a folder: the same menu as the list's folders"
        Check ($null -eq (Find $win "刪除壓縮檔…")) "tree: a folder has no 'delete the archive'"
        $x = Find $win "解壓選取項目到此處"; if ($x) { Click $x }
        $end = (Get-Date).AddSeconds(15); while (-not (Test-Path "$tm\資料夾\a.txt") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
        Check ((Test-Path "$tm\資料夾\a.txt") -and (Test-Path "$tm\資料夾\sub\b.bin")) "tree: 'extract here' on a folder puts the folder (with its name) next to the archive"
    }

    # delete the archive: asks (with a check box), moves it to the Recycle Bin, back to the home page
    $root = Tree-Text "sample.zip"
    if ($root) {
        Right-Click $root
        $d = Find $win "刪除壓縮檔…"; if ($d) { Click $d }
        Check ($null -ne (Wait-Find $win "刪除壓縮檔？" 6)) "delete archive: the question appears"
        Check ($null -ne (Find $win "以後直接刪除，不再詢問")) "delete archive: with a 'do not ask again' check box"
        Click (Find $win "取消"); Start-Sleep -Milliseconds 600
        Check (Test-Path "$tm\sample.zip") "delete archive: cancelling keeps the file"
        Right-Click $root
        $d = Find $win "刪除壓縮檔…"; if ($d) { Click $d }
        Wait-Find $win "刪除壓縮檔？" 6 | Out-Null
        $cb = Find $win "以後直接刪除，不再詢問"; if ($cb) { Click $cb; Start-Sleep -Milliseconds 300 }
        Click (Find $win "移到資源回收筒")
        $end = (Get-Date).AddSeconds(10); while ((Test-Path "$tm\sample.zip") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
        Check (-not (Test-Path "$tm\sample.zip")) "delete archive: the archive is gone"
        Check ($null -ne (Wait-Find $win "開啟壓縮檔" 6)) "delete archive: back on the home page"
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "confirm_delete_archive=false") "delete archive: the check box turned the question off"
    }
    # asked no more: the next delete goes straight through
    Copy-Item "$t\sample.zip" "$tm\second.zip"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 500
    $p = Start-App "$tm\second.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $root = Tree-Text "second.zip"
    if ($root) {
        Right-Click $root
        $d = Find $win "刪除壓縮檔…"; if ($d) { Click $d }
        $end = (Get-Date).AddSeconds(10); while ((Test-Path "$tm\second.zip") -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
        Check (-not (Test-Path "$tm\second.zip")) "delete archive: with the question off the archive is deleted at once"
        Check ($null -ne (Wait-Find $win "開啟壓縮檔" 6)) "delete archive: and the home page is shown"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # drag out of the tree: the root = a folder named after the archive; a folder = the folder
    Reset-State
    $dn = "lztree" + (Get-Date -Format HHmmss); New-Item -ItemType Directory "$tm\$dn" | Out-Null
    Copy-Item "$t\sample.zip" "$tm\sample.zip"
    $p = Start-App "$tm\sample.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Start-Process explorer.exe "`"$tm\$dn`""
    $ex = $null; $end = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 400
         $ex = $AE::RootElement.FindAll($TS::Children, [Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ClassName -eq "CabinetWClass" -and $_.Current.Name -like "*$dn*" } | Select-Object -First 1
    } while (-not $ex -and (Get-Date) -lt $end)
    Check ($null -ne $ex) "tree drag: an Explorer window for the target folder is open"
    if ($ex) {
        $half = [int]($scr.Width / 2); $hgt = [int]($scr.Height - 120)
        [Win.D]::ShowWindow([IntPtr]$ex.Current.NativeWindowHandle, 9) | Out-Null; [Win.D]::MoveWindow([IntPtr]$ex.Current.NativeWindowHandle, $half, 0, $half, $hgt, $true) | Out-Null
        [Win.D]::MoveWindow($p.MainWindowHandle, 0, 0, $half, $hgt, $true) | Out-Null; Start-Sleep -Milliseconds 1200
        function MoveAbs($x, $y) { [Win.D]::mouse_event(0x8001, [uint32]($x * 65535 / $scr.Width), [uint32]($y * 65535 / $scr.Height), 0, [UIntPtr]::Zero) }
        function Drag-TreeItem($name) {
            $el = Tree-Text $name; if (-not $el) { return }
            $r = $el.Current.BoundingRectangle; $sx = [int]($r.X + $r.Width / 2); $sy = [int]($r.Y + $r.Height / 2)
            $er = $ex.Current.BoundingRectangle; $tx = [int]($er.X + $er.Width / 2); $ty = [int]($er.Y + $er.Height * 0.6)
            [Win.D]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 500
            [Win.D]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            for ($i = 1; $i -le 25; $i++) { MoveAbs ($sx + 4 * $i) ($sy + 2 * $i); Start-Sleep -Milliseconds 25 }
            $sx += 100; $sy += 50
            for ($i = 1; $i -le 25; $i++) { MoveAbs ([int]($sx + ($tx - $sx) * $i / 25)) ([int]($sy + ($ty - $sy) * $i / 25)); Start-Sleep -Milliseconds 40 }
            Start-Sleep -Milliseconds 600
            [Win.D]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Seconds 3
        }
        Drag-TreeItem "資料夾"
        Check ((Test-Path "$tm\$dn\資料夾\a.txt") -and (Test-Path "$tm\$dn\資料夾\sub\b.bin")) "tree drag: a folder dropped on Explorer keeps its name and tree"
        Drag-TreeItem "sample.zip"
        Check ((Test-Path "$tm\$dn\sample\readme.txt") -and (Test-Path "$tm\$dn\sample\資料夾\a.txt")) "tree drag: the root dropped on Explorer is a folder named after the archive (sample\)"
        try { $ex.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "previewfix") {
        Reset-State
    # ===== 39. Image preview: Enter works wherever the focus is; a preview closes when its window shows another archive =====
    $pf = Join-Path $t "previewfix"; New-Item -ItemType Directory "$pf\src" -Force | Out-Null
    $bmp = New-Object System.Drawing.Bitmap 64, 48; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::CornflowerBlue); $g.Dispose()
    $bmp.Save("$pf\src\pic1.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Push-Location "$pf\src"; 7z a -tzip "$pf\one.zip" ".\pic1.png" | Out-Null; 7z a -tzip "$pf\two.zip" ".\pic1.png" | Out-Null; Pop-Location
    $env:LYCAN_TEST_NO_OPEN = "1"   # the associated program is not started; the extraction for it still runs
    $p = Start-App "$pf\one.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 900
    $imgRow = Find-Row $win "pic1.png"
    Check ($null -ne $imgRow) "preview fix: the archive lists the picture"
    if ($imgRow) {
        $imgRow.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        Check ($null -ne $pw) "preview fix: the preview opens"
        if ($pw) {
            Start-Sleep -Milliseconds 1200
            # the first thing that has the focus is a toolbar button: Enter used to press that button instead
            [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 2500
            $done = (All-Elements $win) | Where-Object { $_.Current.Name -like "完成*" } | Select-Object -First 1
            Check ($null -ne $done) "preview fix: Enter opens the picture with its program (the main window ran the extraction)"
        }
    }
    Remove-Item Env:LYCAN_TEST_NO_OPEN -ErrorAction SilentlyContinue
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # one window for every archive: the previews of the old archive must not stay
    Reset-State
    Set-Content $settingsFile "open_archive_in=replace`n" -Encoding ascii
    $p = Start-App "$pf\one.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 900
    $imgRow = Find-Row $win "pic1.png"
    if ($imgRow) {
        $imgRow.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 500
        [System.Windows.Forms.SendKeys]::SendWait("%p")
        $pw = $null; $end = (Get-Date).AddSeconds(12); do { $pw = Preview-Window; if (-not $pw) { Start-Sleep -Milliseconds 300 } } while (-not $pw -and (Get-Date) -lt $end)
        Check ($null -ne $pw) "preview fix: a preview is open on the first archive"
        $q = Start-Raw @((Q "$pf\two.zip"))   # forwarded to this window, which shows it instead of one.zip
        Start-Sleep -Seconds 4
        Check ($null -ne (Find $win "two.zip")) "preview fix: the window now shows the second archive"
        Check ($null -eq (Preview-Window)) "preview fix: the preview of the first archive was closed"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "smartstore") {
        Reset-State
    # ===== 40. Smart store: the compress page has the switch (on by default); already-compressed files go in as they are =====
    $ss = Join-Path $t "smartstore"; New-Item -ItemType Directory $ss -Force | Out-Null
    $rnd = New-Object Random 11; $rb = New-Object byte[] (200KB); $rnd.NextBytes($rb); [IO.File]::WriteAllBytes("$ss\photo.jpg", $rb)
    Set-Content "$ss\notes.txt" ("hello world " * 20000)
    $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
    function Packed-Size($arc, $name) {
        $o = & $cliExe l $arc 2>&1 | Out-String
        foreach ($line in ($o -split "`n")) { $c = $line.TrimEnd("`r") -split "`t"; if ($c.Count -ge 4 -and $c[3] -like "*$name*") { return @([uint64]$c[1], [uint64]$c[2]) } }
        return $null
    }
    foreach ($round in 1, 2) {
        $p = Start-Raw @("--add", (Q "$ss\photo.jpg"), (Q "$ss\notes.txt"))
        $w2 = Get-Win $p
        Wait-Find $w2 "新增壓縮檔" 10 | Out-Null; Start-Sleep -Milliseconds 1000
        $expander = $null
        foreach ($e in $w2.FindAll($TS::Descendants, (ByName "進階選項"))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pt)) { $expander = $pt; break } }
        if ($expander) { $expander.Expand(); Start-Sleep -Milliseconds 800 }
        $tg = $null
        foreach ($e in $w2.FindAll($TS::Descendants, (ByName "略過已壓縮的檔案"))) { $pt = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$pt)) { $tg = $pt; break } }
        Check ($null -ne $tg) "smart store ($round): the compress page has the 'store compressed files' switch"
        if ($tg) {
            if ($round -eq 1) { Check ($tg.Current.ToggleState -eq "On") "smart store: the switch is on by default" }
            else { $tg.Toggle(); Start-Sleep -Milliseconds 500; Check ($tg.Current.ToggleState -eq "Off") "smart store: the switch can be turned off" }
        }
        if ($expander) { $expander.Collapse(); Start-Sleep -Milliseconds 500 }
        Remove-Item "$ss\out.zip" -ErrorAction SilentlyContinue
        Click (Find $w2 "開始壓縮")
        Check (Wait-Exit $p 30) "smart store ($round): the window closes after compressing"
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        $outZip = Get-ChildItem $ss -Filter "*.zip" | Select-Object -First 1
        Check ($null -ne $outZip) "smart store ($round): an archive was made"
        if ($outZip) {
            $jpg = Packed-Size $outZip.FullName "photo.jpg"
            $txt = Packed-Size $outZip.FullName "notes.txt"
            if ($round -eq 1) { Check (($null -ne $jpg) -and $jpg[0] -eq $jpg[1]) "smart store: the jpg is stored as it is (packed size = size)" }
            else { Check (($null -ne $jpg) -and $jpg[1] -gt $jpg[0]) "smart store: switched off, the jpg is packed again (a little bigger)" }
            Check (($null -ne $txt) -and $txt[1] -lt $txt[0] / 4) "smart store ($round): the text file is compressed"
            Remove-Item $outZip.FullName -Force
        }
    }
    Reset-State
    }

    if (Want "modify") {
        Reset-State
    # ===== 41. Changing the archive: new folder, rename (F2), delete (Delete), add files; what a format that cannot be changed says =====
    $md = Join-Path $t "modify"; New-Item -ItemType Directory $md -Force | Out-Null
    Copy-Item "$t\sample.zip" "$md\work.zip"
    $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
    function Listing($arc) { (& $cliExe l $arc 2>&1 | Out-String) }
    function Wait-Listing($arc, $needle, [bool]$present, $sec = 20) {
        $end = (Get-Date).AddSeconds($sec)
        do { $l = Listing $arc; $has = $l.Contains($needle); if ($has -ne $present) { Start-Sleep -Milliseconds 400 } } while (($has -ne $present) -and (Get-Date) -lt $end)
        return ($has -eq $present)
    }
    $addPath = Join-Path $md "added file.txt"; Set-Content $addPath ("added " * 100)
    $env:LYCAN_TEST_ADD_PATHS = $addPath
    $p = Start-App "$md\work.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200

    # the toolbar button and its menu
    $addBtn = Find $win "加入到壓縮檔"
    Check ($null -ne $addBtn) "modify: the toolbar has the 'add' button"
    if ($addBtn) {
        Click $addBtn; Start-Sleep -Milliseconds 500
        Check (($null -ne (Find $win "加入檔案…")) -and ($null -ne (Find $win "加入資料夾…")) -and ($null -ne (Find $win "新增資料夾…"))) "modify: the add menu offers files, folder and new folder"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 300
    }

    # new folder: a bad name keeps the question open, a good one makes the folder and shows it selected
    Click (Find $win "加入到壓縮檔"); Start-Sleep -Milliseconds 400
    Click (Wait-Find $win "新增資料夾…" 5)
    $nameBox = Wait-Find $win "名稱" 6
    Check ($null -ne $nameBox) "modify: new folder asks for a name"
    if ($nameBox) {
        Type-Into $nameBox "a:b"; Click (Find $win "確定"); Start-Sleep -Milliseconds 600
        Check (Has-Text $win "名稱不可為空白*" 4) "modify: a name with a colon is refused and the question stays"
        Type-Into (Find $win "名稱") "新資料夾"; Click (Find $win "確定")
    }
    Check (Wait-Listing "$md\work.zip" "新資料夾" $true) "modify: the new folder is in the archive"
    Check (Has-Text $win "已修改壓縮檔*" 12) "modify: the window says what was changed"
    Check ($null -ne (Row-Text $win "新資料夾")) "modify: the archive was opened again and shows the new folder"
    Start-Sleep -Milliseconds 800

    # rename with F2
    $row = Find-Row $win "readme.txt"
    Check ($null -ne $row) "modify: the list shows readme.txt"
    if ($row) {
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        [System.Windows.Forms.SendKeys]::SendWait("{F2}")
        $nameBox = Wait-Find $win "名稱" 6
        Check ($null -ne $nameBox) "modify: F2 asks for the new name"
        if ($nameBox) { Type-Into $nameBox "說明.txt"; Click (Find $win "確定") }
        Check ((Wait-Listing "$md\work.zip" "說明.txt" $true) -and -not (Listing "$md\work.zip").Contains("readme.txt")) "modify: readme.txt is renamed in the archive"
        Start-Sleep -Milliseconds 1500
    }

    # delete: asks, offers 'do not ask again'; cancel keeps it; confirm removes it
    $row = Find-Row $win "說明.txt"
    Check ($null -ne $row) "modify: the renamed item is in the list (selected after the change)"
    if ($row) {
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        [System.Windows.Forms.SendKeys]::SendWait("{DELETE}")
        Check ($null -ne (Wait-Find $win "從壓縮檔刪除？" 6)) "modify: Delete asks first"
        Check ($null -ne (Find $win "以後直接刪除，不再詢問")) "modify: the question has a 'do not ask again' check box"
        Click (Find $win "取消"); Start-Sleep -Milliseconds 600
        Check ((Listing "$md\work.zip").Contains("說明.txt")) "modify: cancelling keeps the item"
        [System.Windows.Forms.SendKeys]::SendWait("{DELETE}")
        Wait-Find $win "從壓縮檔刪除？" 6 | Out-Null
        Click (Find $win "以後直接刪除，不再詢問"); Start-Sleep -Milliseconds 300
        Click (Find $win "刪除")
        Check (Wait-Listing "$md\work.zip" "說明.txt" $false) "modify: confirming removes the item from the archive"
        Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "confirm_delete_entries=false") "modify: the check box turned the question off"
        Start-Sleep -Milliseconds 1500
    }

    # add files (the test hook stands in for the picker); the second time the name is taken: it asks, then replaces
    Click (Find $win "加入到壓縮檔"); Start-Sleep -Milliseconds 400
    Click (Wait-Find $win "加入檔案…" 5)
    Check (Wait-Listing "$md\work.zip" "added file.txt" $true) "modify: the added file is in the archive"
    Start-Sleep -Milliseconds 1500
    Set-Content $addPath ("changed " * 500)   # another size: the replacement can be seen in the listing
    Click (Find $win "加入到壓縮檔"); Start-Sleep -Milliseconds 400
    Click (Wait-Find $win "加入檔案…" 5)
    Check ($null -ne (Wait-Find $win "有同名的項目" 6)) "modify: adding a name that exists asks before replacing"
    Check (($null -ne (Find $win "更新並加入")) -and ($null -ne (Find $win "僅更新現有"))) "modify: the question offers replace, update and add, and update existing only"
    Click (Find $win "加入")
    Check (Wait-Listing "$md\work.zip" "`t4002`t" $true) "modify: replacing goes through (the new content is in the archive)"
    Start-Sleep -Milliseconds 1500
    $left = @(Get-ChildItem $md -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "work.zip.?*" } | ForEach-Object { $_.Name })
    Check ($left.Count -eq 0) ("modify: no backup and no temporary file are left (the backup is off by default) " + ($left -join ","))

    # Delete with several items selected
    Start-Sleep -Milliseconds 1200
    $lst = Listing "$md\work.zip"
    Check ($lst.Contains("added file.txt") -and $lst.Contains("新資料夾")) "modify: the archive still has the added file and the new folder"
    # the right-click menu of an item: add to the folder, rename, delete
    $row = Find-Row $win "新資料夾"
    Check ($null -ne $row) "modify: the new folder is listed"
    if ($row) {
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        $rr = $row.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($rr.X + $rr.Width / 2), [int]($rr.Y + $rr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
        Check (($null -ne (Find $win "重新命名")) -and ($null -ne (Find $win "刪除")) -and ($null -ne (Find $win "加入檔案到此資料夾…"))) "modify: the item menu has add to this folder, rename and delete"
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 400
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # a format that cannot be changed: a message, nothing happens (drop and menu)
    Set-Content "$md\keep.txt" "keep"
    Push-Location $md; 7z a -ttar "$md\plain.tar" ".\keep.txt" | Out-Null; 7z a -tgzip "$md\plain.tar.gz" "$md\plain.tar" | Out-Null; Pop-Location
    $before = (Get-Item "$md\plain.tar.gz").Length
    $p = Start-App "$md\plain.tar.gz"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    Click (Find $win "加入到壓縮檔"); Start-Sleep -Milliseconds 400
    Click (Wait-Find $win "新增資料夾…" 5)
    Check ($null -ne (Wait-Find $win "無法直接修改此類壓縮檔" 6)) "modify: a tar.gz says it cannot be changed"
    Check ((Get-Item "$md\plain.tar.gz").Length -eq $before) "modify: the tar.gz is untouched"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_ADD_PATHS -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "editfile") {
        Reset-State
    # ===== 42. Edit a file of the archive: extracted, watched, asked after it was saved, put back (with "do not ask again") =====
    $ef = Join-Path $t "editfile"; New-Item -ItemType Directory $ef -Force | Out-Null
    Copy-Item "$t\sample.zip" "$ef\work.zip"
    $cliExe = Join-Path $exeDir "lycanzip-cli.exe"
    function Entry-Text($arc, $name) {
        $l = (& $cliExe l $arc 2>&1 | Out-String) -split "`n"
        foreach ($line in $l) { $c = $line.TrimEnd("`r") -split "`t"; if ($c.Count -ge 4 -and $c[3] -eq $name) { return ((& $cliExe cat $arc $c[0] 2>&1 | Out-String)) } }
        return $null
    }
    function Wait-Entry($arc, $name, $like, $sec = 20) {
        $end = (Get-Date).AddSeconds($sec)
        do { $x = Entry-Text $arc $name; $hit = ($null -ne $x) -and ($x -like $like); if (-not $hit) { Start-Sleep -Milliseconds 500 } } while (-not $hit -and (Get-Date) -lt $end)
        return $hit
    }
    $env:LYCAN_TEST_NO_OPEN = "1"   # the editor is not started: the test saves the file itself
    $p = Start-App "$ef\work.zip"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $row = Find-Row $win "readme.txt"
    Check ($null -ne $row) "edit: the list shows readme.txt"
    if ($row) {
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        $rr = $row.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($rr.X + $rr.Width / 2), [int]($rr.Y + $rr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
        $mi = Find $win "編輯並存回壓縮檔"
        Check ($null -ne $mi) "edit: the item menu offers 'edit and put back into the archive'"
        if ($mi) { Click $mi }
        # the extracted file
        $file = $null; $end = (Get-Date).AddSeconds(15)
        do {
            $root = Join-Path $env:TEMP "LycanZip\open"
            $dir = if (Test-Path $root) { Get-ChildItem $root -Directory -Filter "$($p.Id)-*" | Select-Object -First 1 } else { $null }
            if ($dir -and (Test-Path "$($dir.FullName)\readme.txt")) { $file = "$($dir.FullName)\readme.txt" } else { Start-Sleep -Milliseconds 300 }
        } while (-not $file -and (Get-Date) -lt $end)
        Check ($null -ne $file) "edit: the file was extracted to a temporary folder"
        Check (Has-Text $win "正在編輯*" 8) "edit: the window says the file is being edited"
        if ($file) {
            # saved, but "not now": asked, nothing changes
            Set-Content $file "edit number one"
            Check ($null -ne (Wait-Find $win "存回壓縮檔？" 12)) "edit: after saving the window asks whether to put it back"
            Click (Find $win "暫不存回"); Start-Sleep -Milliseconds 1500
            Check (-not (Wait-Entry "$ef\work.zip" "readme.txt" "edit number one*" 2)) "edit: 'not now' leaves the archive as it was"
            # saved again: put back, and "do not ask again"
            Set-Content $file "edit number two"
            Check ($null -ne (Wait-Find $win "存回壓縮檔？" 12)) "edit: a new save asks again"
            Click (Find $win "之後直接存回，不再詢問"); Start-Sleep -Milliseconds 300
            Click (Find $win "存回")
            Check (Wait-Entry "$ef\work.zip" "readme.txt" "edit number two*") "edit: 'put back' changes the file in the archive"
            Check ((Get-Content $settingsFile -Raw -Encoding UTF8) -match "confirm_write_back=false") "edit: the check box turned the question off"
            Start-Sleep -Milliseconds 2000
            # now it goes back by itself
            Set-Content $file "edit number three"
            Check (Wait-Entry "$ef\work.zip" "readme.txt" "edit number three*" 25) "edit: with the question off a saved file goes back by itself"
            Check ($null -eq (Find $win "存回壓縮檔？")) "edit: no question was asked the third time"
        }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    # a format that cannot be changed: a message
    Set-Content "$ef\keep.txt" "keep"
    Push-Location $ef; 7z a -ttar "$ef\plain.tar" ".\keep.txt" | Out-Null; 7z a -tgzip "$ef\plain.tar.gz" "$ef\plain.tar" | Out-Null; Pop-Location
    $p = Start-App "$ef\plain.tar.gz"; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1200
    $row = Find-Row $win "keep.txt"
    if ($row) {
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Start-Sleep -Milliseconds 400
        $rr = $row.Current.BoundingRectangle; [Win.D]::SetCursorPos([int]($rr.X + $rr.Width / 2), [int]($rr.Y + $rr.Height / 2)) | Out-Null; Start-Sleep -Milliseconds 300
        [Win.D]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [Win.D]::mouse_event(16,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 700
        $mi = Find $win "編輯並存回壓縮檔"
        if ($mi) { Click $mi }
        Check ($null -ne (Wait-Find $win "無法直接修改此類壓縮檔" 6)) "edit: a tar.gz says it cannot be edited"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_NO_OPEN -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "scan") {
        Reset-State
    # ===== 43. Scan for malware (AMSI): a task; clean / found / cannot (the stand-in scanner, then the real one) =====
    $sc = Join-Path $t "scan"; New-Item -ItemType Directory "$sc\src" -Force | Out-Null
    Set-Content "$sc\src\clean.txt" ("nothing to see " * 100)
    Set-Content "$sc\src\bad.txt" "xx LYCAN-TEST-THREAT xx"
    Push-Location "$sc\src"; 7z a -tzip "$sc\clean.zip" ".\clean.txt" | Out-Null; 7z a -tzip "$sc\bad.zip" ".\clean.txt" ".\bad.txt" | Out-Null; Pop-Location
    function Run-Scan($arc) {
        $p = Start-App $arc; $win = Get-Win $p; Wait-Find $win "解壓縮到此" | Out-Null; Start-Sleep -Milliseconds 1000
        Click (Find $win "其他選項"); Start-Sleep -Milliseconds 500
        $mi = Wait-Find $win "掃描惡意程式" 5
        if ($mi) { Click $mi }
        return @($p, $win, $mi)
    }
    $env:LYCAN_TEST_SCAN_FAKE = "1"   # the stand-in scanner: flags content with LYCAN-TEST-THREAT (no antivirus involved)
    $r = Run-Scan "$sc\clean.zip"; $p = $r[0]; $win = $r[1]
    Check ($null -ne $r[2]) "scan: the 'other options' menu offers 'scan for malware'"
    Check (Has-Text $win "*已掃描 1 個檔案*未發現威脅*" 20) "scan: a clean archive: the card says what was scanned and that nothing was found"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $r = Run-Scan "$sc\bad.zip"; $p = $r[0]; $win = $r[1]
    Check ($null -ne (Wait-Find $win "發現可疑的檔案" 20)) "scan: a suspicious file: a message says so"
    Check (Has-Text $win "*bad.txt*" 3) "scan: the message names the file"
    Click (Find $win "確定"); Start-Sleep -Milliseconds 500
    Check (Has-Text $win "*發現 1 個可疑的檔案*" 5) "scan: the card of the task says one suspicious file was found"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:LYCAN_TEST_SCAN_FAKE
    # the real scanner of this machine: either it runs, or it says why it cannot
    $r = Run-Scan "$sc\clean.zip"; $p = $r[0]; $win = $r[1]
    Check ((Has-Text $win "*已掃描 1 個檔案*" 25) -or (Has-Text $win "*無法掃描*" 2)) "scan: with the real AMSI the scan ends (a result, or the reason why not)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

    if (Want "volumes") {
        Reset-State
    # ===== 44. A later volume of a split archive: ask (every time) whether to open the first one =====
    $vl = Join-Path $t "volumes"; New-Item -ItemType Directory "$vl\src", "$vl\alone" -Force | Out-Null
    $rnd = New-Object Random 5; $vb = New-Object byte[] (180KB); $rnd.NextBytes($vb); [IO.File]::WriteAllBytes("$vl\src\data.bin", $vb)
    Push-Location "$vl\src"; 7z a -t7z -v60k "$vl\split.7z" ".\data.bin" | Out-Null; Pop-Location
    Check ((Test-Path "$vl\split.7z.001") -and (Test-Path "$vl\split.7z.002")) "volumes: the test set of volumes exists"
    # 1. open the second volume, choose 'open the first volume'
    $p = Start-Raw @((Q "$vl\split.7z.002")); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "這是分割壓縮檔的後續分卷" 12)) "volumes: opening volume 2 asks about the first volume"
    Check ($null -ne (Find $win "仍開啟這一卷")) "volumes: the question also offers to open this volume anyway"
    Click (Find $win "開啟第一卷")
    Check ($null -ne (Wait-Find $win "解壓縮到此" 15)) "volumes: choosing 'open the first volume' opens an archive"
    Check (Has-Text $win "*split.7z.001*" 5) "volumes: the window shows the first volume (split.7z.001)"
    Check ($null -ne (Find-Row $win "data.bin")) "volumes: the whole content is listed (data.bin)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 2. 'open this volume anyway': it is asked again every time, and this time the choice is the other one
    $p = Start-Raw @((Q "$vl\split.7z.002")); $win = Get-Win $p
    Check ($null -ne (Wait-Find $win "這是分割壓縮檔的後續分卷" 12)) "volumes: asked again the next time (never automatic)"
    Click (Find $win "仍開啟這一卷")
    Start-Sleep -Seconds 3
    Check ((Has-Text $win "*split.7z.002*" 8) -or ($null -ne (Find $win "開啟壓縮檔"))) "volumes: 'anyway' opens volume 2 itself (or fails the way a lone volume does)"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 3. cancel: nothing is opened
    $p = Start-Raw @((Q "$vl\split.7z.003")); $win = Get-Win $p
    if (Test-Path "$vl\split.7z.003") {
        Wait-Find $win "這是分割壓縮檔的後續分卷" 12 | Out-Null
        Click (Find $win "取消"); Start-Sleep -Seconds 2
        Check ($null -eq (Find $win "解壓縮到此")) "volumes: cancelling opens nothing"
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    # 4. the first volume is not there: no question (nothing to offer); the volume opens with a note
    Copy-Item "$vl\split.7z.002" "$vl\alone\split.7z.002"
    $p = Start-Raw @((Q "$vl\alone\split.7z.002")); $win = Get-Win $p
    Start-Sleep -Seconds 4
    Check ($null -eq (Find $win "這是分割壓縮檔的後續分卷")) "volumes: without the first volume next to it there is no question"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Reset-State
    }

}
catch {
    Write-Host ("FAIL: script error: " + $_ + " (line " + $_.InvocationInfo.ScriptLineNumber + ")") -ForegroundColor Red
    $failures++
}
finally {
    Get-Process LycanZip -ErrorAction SilentlyContinue | Stop-Process -Force
    foreach ($f in @($portable, $settingsFile)) { if (Test-Path $f) { Remove-Item -LiteralPath $f -Force } }
    if (-not $KeepShots) { }  # screenshots are kept in app\shots (excluded by .gitignore)
}
Write-Host "FAILURES: $failures"
exit $(if ($failures) { 1 } else { 0 })

