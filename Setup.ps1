# Trauma Team Resuscitated - Setup
# Builds the game for this PC from YOUR OWN copy of Trauma Team (USA, extracted with Dolphin), then makes the
# button-prompt pictures from it. Nothing of the game is downloaded: only free tools (Python via uv, the
# w64devkit C compiler, SDL2) and, if you choose, the LaMa picture-repair model.
# Run Setup.bat (it starts this script). Everything it downloads stays in the "tools-cache" folder here.

param([string]$Disc = '', [string]$Tutorials = '', [string]$DiscJp = '', [switch]$NoPause)   # the setup window passes these
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
$Root  = $PSScriptRoot
$Cache = Join-Path $Root 'tools-cache'
$Bin   = Join-Path $Root 'bin'
$DolSha1 = '420143D75336A5F2BC14F9DFBE8A9EC56EFBE8C8'   # Trauma Team (USA) SUOE01, sys/main.dol
$JpSha1  = 'C33B2F40F504F02FF8C3C7CD4D95E4AB0C6FD851'   # HOSPITAL. 6-nin no Ishi (Japan), sys/main.dol
New-Item -ItemType Directory -Force $Cache, $Bin | Out-Null
try { Start-Transcript -Path (Join-Path $Cache 'setup.log') -Force | Out-Null } catch {}

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host ""; Write-Host "ERROR: $t" -ForegroundColor Red; if (-not $NoPause) { Read-Host "Press Enter to close" }; exit 1 }
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
Step "1/6 Finding your extracted disc"
function Disc-Data($p) {
    if (-not $p) { return $null }
    if (Test-Path (Join-Path $p 'sys\main.dol')) { return (Resolve-Path $p).Path }
    if (Test-Path (Join-Path $p 'DATA\sys\main.dol')) { return (Resolve-Path (Join-Path $p 'DATA')).Path }
    return $null
}
# a disc image (.iso .rvz .wbfs .wia .gcz): unpacked with DolphinTool (Dolphin's own command-line tool; yours if
# Dolphin is next to this folder or installed, otherwise a copy is downloaded) into the "disc" folder here
function Find-DolphinTool {
    $c = @(Get-ChildItem -ErrorAction SilentlyContinue -Path "$Root\..\dolphin*\DolphinTool.exe", "$Root\..\..\dolphin*\DolphinTool.exe",
           "$env:ProgramFiles\Dolphin*\DolphinTool.exe", "$Cache\dolphintool\*\DolphinTool.exe", "$Cache\dolphintool\*\*\DolphinTool.exe")
    if ($c.Count) { return $c[0].FullName }
    Write-Host "   getting DolphinTool (to unpack the disc image)"
    $meta = Invoke-RestMethod -UseBasicParsing -Uri 'https://registry.npmjs.org/@emmercm/dolphin-tool-win32-x64/latest'
    $t = Join-Path $Cache 'dolphintool.tgz'; Get-File $meta.dist.tarball $t
    New-Item -ItemType Directory -Force (Join-Path $Cache 'dolphintool') | Out-Null
    & tar -xzf $t -C (Join-Path $Cache 'dolphintool'); Remove-Item $t
    $c = @(Get-ChildItem -Recurse (Join-Path $Cache 'dolphintool') -Filter 'DolphinTool.exe' -ErrorAction SilentlyContinue)
    if (-not $c.Count) { $c = @(Get-ChildItem -Recurse (Join-Path $Cache 'dolphintool') -Filter 'dolphin-tool*.exe') }
    if (-not $c.Count) { Fail "Could not get DolphinTool. Extract the disc with Dolphin instead (Properties > Filesystem > Extract Entire Disc)." }
    return $c[0].FullName
}
$Data = $null
if ($Disc -and (Test-Path -PathType Leaf $Disc)) {
    $dt = Find-DolphinTool
    $out = Join-Path $Root 'disc'
    if (-not (Disc-Data $out)) {
        Write-Host "   unpacking the disc image (a few minutes)..."
        $ErrorActionPreference = 'Continue'   # DolphinTool writes notes to stderr
        & $dt extract -i $Disc -o $out --gameonly --quiet 2>&1 | ForEach-Object { Write-Host "   $_" }
        $ErrorActionPreference = 'Stop'
    }
    $dol = Get-ChildItem -Recurse $out -Filter 'main.dol' -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -eq 'sys' } | Select-Object -First 1
    if (-not $dol) { Fail "DolphinTool could not unpack that disc image. Is it a Wii disc image (.iso, .rvz, .wbfs, .wia, .gcz)?" }
    if ((Get-FileHash -Algorithm SHA1 $dol.FullName).Hash -ne $DolSha1) {
        Remove-Item -Recurse -Force $out
        Fail "That disc image is not Trauma Team (USA) (SUOE01). Only the USA version is supported."
    }
    $Data = $dol.Directory.Parent.FullName
} else { $Data = Disc-Data $Disc }
$saved = Join-Path $Bin 'disc.txt'
if (-not $Data -and (Test-Path $saved)) { $Data = Disc-Data ((Get-Content $saved -TotalCount 1).Trim()) }
foreach ($c in @("$Root\disc", "$Root\..\disc", "$Root\..\..\disc")) { if (-not $Data) { $Data = Disc-Data $c } }
if (-not $Data -and -not $NoPause) {
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
Step "2/6 Getting the tools (first time only)"
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
$SdlDll = Join-Path $Sdl 'bin\SDL2.dll'
# 2.32 or newer: older SDL does not know some controllers (an 8BitDo Ultimate 2 did nothing with 2.30)
if (-not (Test-Path $SdlDll) -or (Get-Item $SdlDll).VersionInfo.FileMinorPart -lt 32) {
    if (Test-Path $Sdl) { Remove-Item -Recurse -Force $Sdl }
    $z = Join-Path $Cache 'sdl.zip'; $t = Join-Path $Cache 'sdl'
    Get-File 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip' $z
    Expand-Archive -Force $z $t
    $src = Get-ChildItem -Recurse -Directory $t | Where-Object { $_.Name -eq 'x86_64-w64-mingw32' } | Select-Object -First 1
    New-Item -ItemType Directory -Force $Sdl | Out-Null
    foreach ($d in 'include', 'lib', 'bin') { Copy-Item -Recurse -Force (Join-Path $src.FullName $d) $Sdl }
    Remove-Item -Recurse -Force $z, $t
}
$env:PATH = (Join-Path $Dev 'bin') + ';' + $env:PATH

# ---------------------------------------------------------------- 3. recompile
Step "3/6 Recompiling the game from your disc (a few minutes)"
Push-Location $Root
& $Uv run --no-project --python 3.12 python -m recomp.lift (Join-Path $Data 'sys\main.dol') data\symbols.txt generated
if ($LASTEXITCODE) { Pop-Location; Fail "Translating main.dol failed." }
Pop-Location
Push-Location (Join-Path $Root 'recomp')
& make -f build.mk TARGET=windows VR=0 CC=gcc WINDRES=windres "-j$env:NUMBER_OF_PROCESSORS"   # VR mode: not in releases yet
$ok = $LASTEXITCODE -eq 0
Pop-Location
$Exe = Join-Path $Root 'build-win\Resuscitated.exe'
if (-not $ok -or -not (Test-Path $Exe)) { Fail "Building failed - see the messages above." }

Step "4/6 Putting the game together in the bin folder"
Copy-Item -Force $Exe $Bin
Copy-Item -Force (Join-Path $Sdl 'bin\SDL2.dll') $Bin
foreach ($line in (& objdump -p $Exe | Select-String 'DLL Name: (\S+)')) {   # compiler run-time DLLs the program needs
    $n = $line.Matches[0].Groups[1].Value; $p = Join-Path $Dev "bin\$n"
    if (Test-Path $p) { Copy-Item -Force $p $Bin }
}
if (-not (Test-Path (Join-Path $Bin 'nand'))) { Copy-Item -Recurse (Join-Path $Root 'nand') $Bin }
Set-Content -Encoding ASCII $saved $Data

# optional: Japanese voices from the player's Japanese disc (only its sound\jpn folder is kept, in disc_jp)
if ($DiscJp) {
    Write-Host "   Japanese voices: $DiscJp"
    $jpRoot = Disc-Data $DiscJp
    if ($jpRoot -and (Get-FileHash -Algorithm SHA1 (Join-Path $jpRoot 'sys\main.dol')).Hash -ne $JpSha1) { $jpRoot = $null }
    if (-not $jpRoot -and (Test-Path (Join-Path $DiscJp 'files\sound\jpn\sys\00000000\static.brsar'))) { $jpRoot = $DiscJp }   # kept from an earlier install
    if (-not $jpRoot -and (Test-Path -PathType Leaf $DiscJp)) {
        $dt = Find-DolphinTool; $tmp = Join-Path $Cache 'disc_jp_unpack'
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        Write-Host "   unpacking the Japanese disc image (a few minutes)..."
        $ErrorActionPreference = 'Continue'
        & $dt extract -i $DiscJp -o $tmp --gameonly --quiet 2>&1 | ForEach-Object { Write-Host "   $_" }
        $ErrorActionPreference = 'Stop'
        $dol = Get-ChildItem -Recurse $tmp -Filter 'main.dol' -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -eq 'sys' } | Select-Object -First 1
        if ($dol -and (Get-FileHash -Algorithm SHA1 $dol.FullName).Hash -eq $JpSha1) {
            $dst = Join-Path $Root 'disc_jp\DATA\files\sound'
            New-Item -ItemType Directory -Force $dst | Out-Null
            if (Test-Path (Join-Path $dst 'jpn')) { Remove-Item -Recurse -Force (Join-Path $dst 'jpn') }
            Move-Item (Join-Path $dol.Directory.Parent.FullName 'files\sound\jpn') $dst
            $jpRoot = Join-Path $Root 'disc_jp\DATA'
        }
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    }
    if ($jpRoot -and (Test-Path (Join-Path $jpRoot 'files\sound\jpn\sys\00000000\static.brsar'))) {
        Set-Content -Encoding ASCII (Join-Path $Bin 'disc_jp.txt') $jpRoot
        Write-Host "   Japanese voices ready - pick them in the game: F1 > Language > Voices."
    } else { Write-Host "   That is not HOSPITAL. 6-nin no Ishi (Japan) - skipping the Japanese voices." -ForegroundColor Yellow }
}

# ---------------------------------------------------------------- 4. button prompts
Step "5/6 Making the button prompts from your disc"
Write-Host "   The tutorial pictures show a Wii Remote. Setup can change them to show your keyboard or controller"
Write-Host "   buttons instead (downloads about 400 MB once - a picture-repair AI model - and takes about 5 minutes)."
$tut = if ($Tutorials) { $Tutorials -match '^[yY1t]' } else { (Read-Host "   Show keyboard / controller button prompts in the tutorial pictures (experimental)? [Y/n]") -notmatch '^[nN]' }
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

Step "6/6 Done"
Write-Host "   Start the game with bin\Resuscitated.exe (F1 or Options + Share: settings and controls)."
$s = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $Root 'Trauma Team Resuscitated.lnk'))
$s.TargetPath = Join-Path $Bin 'Resuscitated.exe'; $s.WorkingDirectory = $Bin; $s.Save()
if (-not $NoPause) { Read-Host "Press Enter to close" }
