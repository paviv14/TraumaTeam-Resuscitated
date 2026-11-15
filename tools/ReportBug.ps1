# Report a bug: collects what helps find a problem into one zip on the Desktop and opens the bug report page.
# Nothing is sent anywhere by this script: you attach the zip to the report yourself.
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$Bin  = Join-Path $Root 'bin'
$Url  = 'https://github.com/paviv14/TraumaTeam-Resuscitated/issues/new?template=bug_report.yml'

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$tmp = Join-Path $env:TEMP "tt-report-$stamp"
New-Item -ItemType Directory -Force $tmp | Out-Null

# the last two runs' logs and the settings (your Windows user name is replaced by <user>)
foreach ($f in 'ttpc.log', 'ttpc.prev.log', 'settings.ini', 'disc.txt') {
    $p = Join-Path $Bin $f
    if (Test-Path $p) {
        $t = Get-Content -Raw -LiteralPath $p
        if ($env:USERNAME) { $t = $t -replace [regex]::Escape($env:USERNAME), '<user>' }
        Set-Content -LiteralPath (Join-Path $tmp $f) -Value $t -NoNewline
    }
}
# the newest F9 capture (picture + what the game was drawing), if there is one from the last hour
$cap = Get-ChildItem -ErrorAction SilentlyContinue (Join-Path $Bin 'captures\capture_*.png') |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($cap -and $cap.LastWriteTime -gt (Get-Date).AddHours(-1)) {
    Copy-Item $cap.FullName $tmp
    $txt = [IO.Path]::ChangeExtension($cap.FullName, '.txt')
    if (Test-Path $txt) { Copy-Item $txt $tmp }
}
# the PC (no names, no serial numbers)
$os  = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
$cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
$gpu = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue
$mon = Get-CimInstance Win32_DesktopMonitor -ErrorAction SilentlyContinue
$exe = Join-Path $Bin 'Resuscitated.exe'
@(
    "Windows:  $($os.Caption) $($os.Version)"
    "CPU:      $($cpu.Name)"
    "RAM:      $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB"
    ($gpu | ForEach-Object { "GPU:      $($_.Name) (driver $($_.DriverVersion)), $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" })
    "Game exe: $(if (Test-Path $exe) { (Get-Item $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm') } else { 'missing' })"
    "Texture pack: $(if (Test-Path (Join-Path $Root 'textures')) { 'yes' } else { 'no' })"
) | Set-Content (Join-Path $tmp 'pc.txt')

$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "TraumaTeam-bug-report-$stamp.zip"
Compress-Archive -Force -Path (Join-Path $tmp '*') -DestinationPath $zip
Remove-Item -Recurse -Force $tmp

Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.MessageBox]::Show(
    "Saved on your Desktop:`n$([IO.Path]::GetFileName($zip))`n`n" +
    "The bug report page opens next. Please describe what happened and drag this zip into the report.`n`n" +
    "Tip: press F9 in the game right when something looks wrong, then run Report a bug - the picture is included.",
    'Trauma Team Resuscitated - Report a bug') | Out-Null
Start-Process explorer.exe "/select,`"$zip`""
Start-Process $Url
