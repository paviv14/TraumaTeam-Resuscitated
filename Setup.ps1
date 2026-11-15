# Trauma Team Resuscitated - Setup
# Builds the game for this PC from YOUR OWN copy of Trauma Team (USA, extracted with Dolphin), then makes the
# button-prompt pictures from it. Nothing of the game is downloaded: only free tools (Python via uv, the
# w64devkit C compiler, SDL2, the OpenXR loader) and, if you choose, the LaMa picture-repair model.
# Run Setup.bat (it starts this script). Everything it downloads stays in the "tools-cache" folder here.

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
$Root  = $PSScriptRoot
$Cache = Join-Path $Root 'tools-cache'
$Bin   = Join-Path $Root 'bin'
$DolSha1 = '420143D75336A5F2BC14F9DFBE8A9EC56EFBE8C8'   # Trauma Team (USA) SUOE01, sys/main.dol
New-Item -ItemType Directory -Force $Cache, $Bin | Out-Null
try { Start-Transcript -Path (Join-Path $Cache 'setup.log') -Force | Out-Null } catch {}

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host ""; Write-Host $t -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }
function Get-File($url, $out) {
    Write-Host "   downloading $(Split-Path $url -Leaf)"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out
}
function Latest-Asset($repo, $pattern) {
    $r = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'TraumaTeamResuscitated-Setup' }
    $a = $r.assets | Where-Object { $_.name -match $pattern } | Select-Object -First 1
    if (-not $a) { Fail "Could not find a download for $repo ($pattern)." }
    return $a.browser_download_url
}

Write-Host "Trauma Team Resuscitated - Setup" -ForegroundColor White
Write-Host "An unofficial fan project, free and not affiliated with Atlus, Sega or Nintendo."
Write-Host "It builds the game from your own copy; nothing here contains the game."

# ---------------------------------------------------------------- 1. your disc
Step "Finding your extracted disc"
function Disc-Data($p) {
    if (-not $p) { return $null }
    if (Test-Path (Join-Path $p 'sys\main.dol')) { return (Resolve-Path $p).Path }
    if (Test-Path (Join-Path $p 'DATA\sys\main.dol')) { return (Resolve-Path (Join-Path $p 'DATA')).Path }
    return $null
}
$Data = $null
$saved = Join-Path $Bin 'disc.txt'
if (Test-Path $saved) { $Data = Disc-Data ((Get-Content $saved -TotalCount 1).Trim()) }
foreach ($c in @("$Root\disc", "$Root\..\disc", "$Root\..\..\disc")) { if (-not $Data) { $Data = Disc-Data $c } }
if (-not $Data) {
    Write-Host "   Pick the folder you extracted Trauma Team (USA) to."
    Write-Host "   (Dolphin: right-click the game > Properties > Filesystem > right-click the disc > Extract Entire Disc)"
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'The folder you extracted Trauma Team (USA) to (it contains DATA, or sys and files)'
    if ($dlg.ShowDialog() -eq 'OK') { $Data = Disc-Data $dlg.SelectedPath }
}
if (-not $Data) { Fail "No extracted disc found (a folder with sys\main.dol or DATA\sys\main.dol)." }
$sha = (Get-FileHash -Algorithm SHA1 (Join-Path $Data 'sys\main.dol')).Hash
if ($sha -ne $DolSha1) { Fail "That disc's main.dol is not Trauma Team (USA) (SUOE01). Only the USA version is supported." }
Write-Host "   $Data  (Trauma Team USA - OK)"

# ---------------------------------------------------------------- 2. tools
Step "Getting the tools (first time only)"
$Uv = Join-Path $Cache 'uv\uv.exe'
if (-not (Test-Path $Uv)) {
    $z = Join-Path $Cache 'uv.zip'
    Get-File 'https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-pc-windows-msvc.zip' $z
    Expand-Archive -Force $z (Join-Path $Cache 'uv'); Remove-Item $z
}
$env:UV_CACHE_DIR = Join-Path $Cache 'uv-cache'; $env:UV_PYTHON_INSTALL_DIR = Join-Path $Cache 'python'
$Dev = Join-Path $Cache 'w64devkit'
if (-not (Test-Path (Join-Path $Dev 'bin\gcc.exe'))) {
    $x = Join-Path $Cache 'w64devkit.exe'
    Get-File (Latest-Asset 'skeeto/w64devkit' '^w64devkit-x64-.*\.exe$') $x
    & $x "-o$Cache" -y | Out-Null; Remove-Item $x
    if (-not (Test-Path (Join-Path $Dev 'bin\gcc.exe'))) { Fail "The compiler (w64devkit) did not unpack." }
}
$Tp = Join-Path $Root 'third_party'
$Sdl = Join-Path $Tp 'SDL2-win'
if (-not (Test-Path (Join-Path $Sdl 'bin\SDL2.dll'))) {
    $z = Join-Path $Cache 'sdl.zip'; $t = Join-Path $Cache 'sdl'
    Get-File 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.zip' $z
    Expand-Archive -Force $z $t
    $src = Get-ChildItem -Recurse -Directory $t | Where-Object { $_.Name -eq 'x86_64-w64-mingw32' } | Select-Object -First 1
    New-Item -ItemType Directory -Force $Sdl | Out-Null
    foreach ($d in 'include', 'lib', 'bin') { Copy-Item -Recurse -Force (Join-Path $src.FullName $d) $Sdl }
    Remove-Item -Recurse -Force $z, $t
}
$Xr = Join-Path $Tp 'openxr'
if (-not (Test-Path (Join-Path $Xr 'libopenxr_loader.dll'))) {
    New-Item -ItemType Directory -Force $Xr | Out-Null
    Copy-Item -Recurse -Force (Join-Path $Root 'third_party_src\openxr\*') $Xr
    $z = Join-Path $Cache 'openxr.zip'; $t = Join-Path $Cache 'openxr'
    Get-File (Latest-Asset 'KhronosGroup/OpenXR-SDK-Source' '^openxr_loader_windows.*\.zip$') $z
    Expand-Archive -Force $z $t
    $dll = Get-ChildItem -Recurse $t -Filter 'openxr_loader.dll' | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    if (-not $dll) { Fail "The OpenXR loader download did not contain openxr_loader.dll." }
    Copy-Item $dll.FullName (Join-Path $Xr 'libopenxr_loader.dll')
    Remove-Item -Recurse -Force $z, $t
}
$env:PATH = (Join-Path $Dev 'bin') + ';' + $env:PATH
if (-not (Test-Path (Join-Path $Xr 'libopenxr_loader.dll.a'))) {
    Push-Location $Xr; & dlltool -d openxr_loader.def -l libopenxr_loader.dll.a; Pop-Location
}

# ---------------------------------------------------------------- 3. recompile
Step "Recompiling the game from your disc (a few minutes)"
Push-Location $Root
& $Uv run --no-project --python 3.12 python -m recomp.lift (Join-Path $Data 'sys\main.dol') data\symbols.txt generated
if ($LASTEXITCODE) { Pop-Location; Fail "Translating main.dol failed." }
Pop-Location
Push-Location (Join-Path $Root 'recomp')
& make -f build.mk TARGET=windows CC=gcc WINDRES=windres "-j$env:NUMBER_OF_PROCESSORS"
$ok = $LASTEXITCODE -eq 0
Pop-Location
$Exe = Join-Path $Root 'build-win\Resuscitated.exe'
if (-not $ok -or -not (Test-Path $Exe)) { Fail "Building failed - see the messages above." }

Step "Putting the game together in the bin folder"
Copy-Item -Force $Exe $Bin
Copy-Item -Force (Join-Path $Sdl 'bin\SDL2.dll') $Bin
Copy-Item -Force (Join-Path $Xr 'libopenxr_loader.dll') $Bin
foreach ($line in (& objdump -p $Exe | Select-String 'DLL Name: (\S+)')) {   # compiler run-time DLLs the program needs
    $n = $line.Matches[0].Groups[1].Value; $p = Join-Path $Dev "bin\$n"
    if (Test-Path $p) { Copy-Item -Force $p $Bin }
}
if (-not (Test-Path (Join-Path $Bin 'nand'))) { Copy-Item -Recurse (Join-Path $Root 'nand') $Bin }
Set-Content -Encoding ASCII $saved $Data

# ---------------------------------------------------------------- 4. button prompts
Step "Making the button prompts from your disc"
Write-Host "   The tutorial pictures show a Wii Remote. Setup can repaint them for keyboard and controllers"
Write-Host "   (downloads about 1 GB once - a picture-repair AI model - and takes 10-30 minutes)."
$tut = (Read-Host "   Repaint the tutorial pictures? [Y/n]") -notmatch '^[nN]'
$with = @('--with', 'pillow', '--with', 'numpy', '--with', 'scipy', '--with', 'opencv-python-headless')
$args2 = @()
if ($tut) {
    $with += @('--with', 'torch')
    $m = Join-Path $Root 'tools\prompts\big-lama.pt'
    if (-not (Test-Path $m)) { Get-File 'https://github.com/Sanster/models/releases/download/add_big_lama/big-lama.pt' $m }
    $env:LAMA_MODEL = $m; $args2 = @('--tutorials')
}
Push-Location $Root
& $Uv run --no-project --python 3.12 @with python tools\prompts\setup_prompts.py $Data (Join-Path $Bin 'prompts') (Join-Path $Cache 'prompts-work') @args2
$ok = $LASTEXITCODE -eq 0
Pop-Location
if (-not $ok) { Write-Host "   The button prompts could not be made; the game still works with the original Wii pictures." -ForegroundColor Yellow }

Step "Done"
Write-Host "   Start the game with bin\Resuscitated.exe (F1 or Options + Share: settings and controls)."
$s = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $Root 'Trauma Team Resuscitated.lnk'))
$s.TargetPath = Join-Path $Bin 'Resuscitated.exe'; $s.WorkingDirectory = $Bin; $s.Save()
Read-Host "Press Enter to close"
