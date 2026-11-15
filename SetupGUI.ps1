# Trauma Team Resuscitated - the setup window. It runs Setup.ps1 (which does the work) and shows its progress.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$Root = $PSScriptRoot
$Log  = Join-Path $Root 'tools-cache\setup-window.log'; $ErrLog = Join-Path $Root 'tools-cache\setup-errors.log'
function Read-Shared($p) {   # the log while Setup is still writing it
    try { $f = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite'); $r = New-Object IO.StreamReader($f); $t = $r.ReadToEnd(); $r.Close(); return @($t -split "`r?`n") } catch { return @() }
}
New-Item -ItemType Directory -Force (Join-Path $Root 'tools-cache') | Out-Null

# ---------------------------------------------------------------- look
$bg = [Drawing.Color]::FromArgb(24, 24, 28); $panel = [Drawing.Color]::FromArgb(36, 36, 42)
$fg = [Drawing.Color]::FromArgb(235, 235, 240); $dim = [Drawing.Color]::FromArgb(160, 160, 170)
$red = [Drawing.Color]::FromArgb(205, 32, 40); $green = [Drawing.Color]::FromArgb(70, 190, 110)
$ui = New-Object Drawing.Font('Segoe UI', 10); $uiB = New-Object Drawing.Font('Segoe UI Semibold', 10)
function Label($text, $x, $y, $w, $h, $font = $ui, $color = $fg) {
    $l = New-Object Windows.Forms.Label; $l.Text = $text; $l.Location = "$x,$y"; $l.Size = "$w,$h"
    $l.Font = $font; $l.ForeColor = $color; $l.BackColor = [Drawing.Color]::Transparent; $form.Controls.Add($l); $l
}
function Button($text, $x, $y, $w, $primary = $false) {
    $b = New-Object Windows.Forms.Button; $b.Text = $text; $b.Location = "$x,$y"; $b.Size = "$w,36"; $b.Font = $uiB
    $b.FlatStyle = 'Flat'; $b.FlatAppearance.BorderSize = 0; $b.ForeColor = 'White'; $b.Cursor = 'Hand'
    $b.BackColor = $(if ($primary) { $red } else { $panel }); $form.Controls.Add($b); $b
}

$form = New-Object Windows.Forms.Form
$form.Text = 'Trauma Team Resuscitated - Setup'; $form.ClientSize = '640,520'; $form.BackColor = $bg
$form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false; $form.StartPosition = 'CenterScreen'
$ico = Join-Path $Root 'recomp\res\resuscitated.ico'; if (Test-Path $ico) { $form.Icon = New-Object Drawing.Icon($ico) }

# header: red cross + title
$hdr = New-Object Windows.Forms.Panel; $hdr.Location = '0,0'; $hdr.Size = '640,92'; $hdr.BackColor = $panel; $form.Controls.Add($hdr)
$hdr.Add_Paint({ param($s, $e)
    $g = $e.Graphics; $g.SmoothingMode = 'AntiAlias'; $br = New-Object Drawing.SolidBrush($red)
    $g.FillRectangle($br, 40, 22, 16, 48); $g.FillRectangle($br, 24, 38, 48, 16)
    $g.DrawString('TRAUMA TEAM', (New-Object Drawing.Font('Segoe UI Black', 20)), [Drawing.Brushes]::White, 88, 14)
    $g.DrawString('RESUSCITATED  -  PC setup', (New-Object Drawing.Font('Segoe UI Semibold', 10)), (New-Object Drawing.SolidBrush($dim)), 91, 54) })
Label ("An unofficial fan project, free and not affiliated with Atlus, Sega or Nintendo. " +
       "Setup builds the game on this PC from your own copy of Trauma Team (USA); nothing of the game is downloaded.") 24 104 592 40 $ui $dim | Out-Null

# 1. disc
Label '1.  Your copy of Trauma Team (USA)' 24 156 400 22 $uiB | Out-Null
Label 'Your disc image (.iso, .rvz, .wbfs...) - or a folder you extracted it to with Dolphin' 44 178 580 20 (New-Object Drawing.Font('Segoe UI', 8.5)) $dim | Out-Null
$disc = New-Object Windows.Forms.TextBox; $disc.Location = '44,202'; $disc.Size = '330,26'; $disc.Font = $ui
$disc.BackColor = $panel; $disc.ForeColor = $fg; $disc.BorderStyle = 'FixedSingle'; $form.Controls.Add($disc)
$browse = Button 'Disc image...' 384 198 120
$folder = Button 'Folder...' 512 198 104
$discOk = Label '' 44 232 572 20 (New-Object Drawing.Font('Segoe UI', 9)) $dim

# 2. options
Label '2.  Options' 24 262 400 22 $uiB | Out-Null
$tut = New-Object Windows.Forms.CheckBox; $tut.Location = '44,288'; $tut.Size = '572,40'; $tut.Checked = $true
$tut.Font = $ui; $tut.ForeColor = $fg; $tut.Text = "Repaint the tutorial pictures for keyboard and controllers`n(downloads about 1 GB once; adds 10-30 minutes)"
$form.Controls.Add($tut)

# progress
$status = Label 'Ready.' 24 344 592 22 $uiB
$bar = New-Object Windows.Forms.ProgressBar; $bar.Location = '24,370'; $bar.Size = '592,10'; $bar.Maximum = 60; $form.Controls.Add($bar)
$box = New-Object Windows.Forms.TextBox; $box.Location = '24,388'; $box.Size = '592,70'; $box.Multiline = $true; $box.ReadOnly = $true
$box.ScrollBars = 'Vertical'; $box.BackColor = $panel; $box.ForeColor = $dim; $box.BorderStyle = 'None'
$box.Font = New-Object Drawing.Font('Consolas', 8.5); $form.Controls.Add($box)

$install = Button 'Install' 476 470 140 $true
$play = Button 'Play now' 476 470 140 $true; $play.Visible = $false
$close = Button 'Close' 326 470 140

# ---------------------------------------------------------------- disc folder
function Disc-Data($p) {
    if (-not $p) { return $null }
    if (Test-Path (Join-Path $p 'sys\main.dol')) { return $p }
    if (Test-Path (Join-Path $p 'DATA\sys\main.dol')) { return (Join-Path $p 'DATA') }
    return $null
}
function Check-Disc {
    $p = $disc.Text.Trim()
    if ($p -and (Test-Path -PathType Leaf $p)) {   # a disc image: its game ID is in the first bytes of a plain .iso; others are checked during the install
        $id = ''; try { $f = [IO.File]::OpenRead($p); $b = New-Object byte[] 6; [void]$f.Read($b, 0, 6); $f.Close(); $id = [Text.Encoding]::ASCII.GetString($b) } catch {}
        if ($p -match '\.iso$' -and $id -ne 'SUOE01') { $discOk.Text = 'This is not Trauma Team (USA) (game ID SUOE01).'; $discOk.ForeColor = $red; return $false }
        $discOk.Text = 'Disc image - it will be unpacked during the install.'; $discOk.ForeColor = $green; return $true
    }
    $d = Disc-Data $p
    if (-not $d) { $discOk.Text = $(if ($disc.Text) { 'No sys\main.dol in that folder.' } else { 'Pick the folder you extracted the game to.' }); $discOk.ForeColor = $dim; return $false }
    $discOk.Text = 'Checking...'; $form.Refresh()
    if ((Get-FileHash -Algorithm SHA1 (Join-Path $d 'sys\main.dol')).Hash -ne '420143D75336A5F2BC14F9DFBE8A9EC56EFBE8C8') {
        $discOk.Text = 'This is not Trauma Team (USA). Only the USA version (SUOE01) is supported.'; $discOk.ForeColor = $red; return $false }
    $discOk.Text = 'Trauma Team (USA) found.'; $discOk.ForeColor = $green; return $true
}
$saved = Join-Path $Root 'bin\disc.txt'
if (Test-Path $saved) { $disc.Text = (Get-Content $saved -TotalCount 1).Trim() }
if (-not $disc.Text) { foreach ($c in @("$Root\disc", "$Root\..\disc", "$Root\..\..\disc")) { if (-not $disc.Text -and (Disc-Data $c)) { $disc.Text = (Resolve-Path $c).Path } } }
if (-not $disc.Text) {   # a disc image next to this folder, e.g. "Trauma Team (USA).rvz"
    $img = Get-ChildItem -ErrorAction SilentlyContinue -Path "$Root\*", "$Root\..\*" -Include *.iso, *.rvz, *.wbfs, *.wia, *.gcz |
           Where-Object { $_.Name -match 'trauma\s*team' } | Select-Object -First 1
    if ($img) { $disc.Text = $img.FullName }
}
$browse.Add_Click({
    $dlg = New-Object Windows.Forms.OpenFileDialog; $dlg.Title = 'Your Trauma Team (USA) disc image'
    $dlg.Filter = 'Wii disc images (*.iso;*.rvz;*.wbfs;*.wia;*.gcz)|*.iso;*.rvz;*.wbfs;*.wia;*.gcz|All files|*.*'
    if ($dlg.ShowDialog() -eq 'OK') { $disc.Text = $dlg.FileName; Check-Disc | Out-Null } })
$folder.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog; $dlg.Description = 'The folder you extracted Trauma Team (USA) to'
    if ($dlg.ShowDialog() -eq 'OK') { $disc.Text = $dlg.SelectedPath; Check-Disc | Out-Null } })
$disc.Add_Leave({ Check-Disc | Out-Null })

# ---------------------------------------------------------------- run Setup.ps1, follow its output
$proc = $null; $timer = New-Object Windows.Forms.Timer; $timer.Interval = 400; $seen = 0
$install.Add_Click({
    if (-not (Check-Disc)) { return }
    $install.Enabled = $false; $browse.Enabled = $false; $folder.Enabled = $false; $tut.Enabled = $false; $disc.ReadOnly = $true
    $status.Text = 'Starting...'; $box.Text = ''; Set-Content $Log ''
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $Root 'Setup.ps1')`"", '-NoPause',
           '-Disc', "`"$($disc.Text.Trim())`"", '-Tutorials', $(if ($tut.Checked) { 'yes' } else { 'no' }))
    $script:proc = Start-Process powershell -ArgumentList $a -WindowStyle Hidden -PassThru -RedirectStandardOutput $Log -RedirectStandardError $ErrLog
    $timer.Start() })
$timer.Add_Tick({
    $lines = Read-Shared $Log
    if ($lines.Count -gt $script:seen) {
        foreach ($l in $lines[$script:seen..($lines.Count - 1)]) {
            if ($l -match '^== (\d)/6 (.*)') { $status.Text = "Step $($matches[1]) of 6:  $($matches[2])"; $bar.Value = [Math]::Min(60, ([int]$matches[1] - 1) * 10 + 2) }
            elseif ($l.Trim()) { $box.AppendText($l.TrimEnd() + "`r`n") }
        }
        $script:seen = $lines.Count
    } elseif ($bar.Value -lt 58 -and $bar.Value % 10 -lt 8) { $bar.Value++ }   # creeping within a step
    if ($script:proc -and $script:proc.HasExited) {
        $timer.Stop()
        if ($script:proc.ExitCode -eq 0) {
            $bar.Value = 60; $status.Text = 'All done - enjoy!'; $status.ForeColor = $green; $install.Visible = $false; $play.Visible = $true
        } else {
            $status.Text = 'Setup stopped - see the messages below (also in tools-cache\setup.log).'; $status.ForeColor = $red
            foreach ($l in (Read-Shared $ErrLog | Select-Object -Last 15)) { if ($l.Trim()) { $box.AppendText($l.TrimEnd() + "`r`n") } }
            $install.Text = 'Try again'; $install.Enabled = $true; $browse.Enabled = $true; $folder.Enabled = $true; $tut.Enabled = $true; $disc.ReadOnly = $false
        }
    } })
$play.Add_Click({ Start-Process (Join-Path $Root 'bin\Resuscitated.exe') -WorkingDirectory (Join-Path $Root 'bin'); $form.Close() })
$close.Add_Click({ $form.Close() })
$form.Add_FormClosing({ param($s, $e)
    if ($script:proc -and -not $script:proc.HasExited) {
        if ([Windows.Forms.MessageBox]::Show('Setup is still running. Stop it?', 'Trauma Team Resuscitated', 'YesNo') -ne 'Yes') { $e.Cancel = $true; return }
        & taskkill /T /F /PID $script:proc.Id | Out-Null
    } })
$form.Add_Shown({ Check-Disc | Out-Null })
[void]$form.ShowDialog()
