# publish.ps1 - Build SpeedoIV-CE and package it into a release zip.
#
# Reads VERSION (one-line semver, e.g. 1.0.0) from the repo root.
# Builds the ASI fresh, gathers it together with dist\SpeedoIV\ assets and
# a generated end-user README, and produces:
#
#   dist\SpeedoIV-CE-v<VERSION>.zip
#
# Zip layout (extract into the GTA IV install folder):
#
#   plugins\
#       SpeedoIV-CE.asi
#   SpeedoIV\
#       Config.ini
#       Default\
#           Bck.png
#           Pin.png
#           NOTICE.txt
#   README.txt           (end-user instructions, generated)
#
# Usage:
#   .\publish.ps1                 # full build + zip
#   .\publish.ps1 -SkipBuild      # reuse build\SpeedoIV-CE.asi as-is

[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo  = $PSScriptRoot
$out   = Join-Path $repo "dist"
$asi   = Join-Path $repo "build\SpeedoIV-CE.asi"

# --- 1. Version ---
$versionFile = Join-Path $repo "VERSION"
if (-not (Test-Path $versionFile)) {
    Write-Error "VERSION file is missing at repo root. Create it with a single line like '1.0.0'."
    exit 1
}
$version = (Get-Content $versionFile -Raw).Trim()
if (-not ($version -match '^\d+\.\d+\.\d+([-.+].+)?$')) {
    Write-Error "VERSION file content '$version' is not semver (expected MAJOR.MINOR.PATCH[-suffix])."
    exit 1
}
Write-Host "Publishing SpeedoIV-CE v$version" -ForegroundColor Cyan

# --- 2. Build (unless skipped) ---
if (-not $SkipBuild) {
    Write-Host "Building..."
    & (Join-Path $repo "build.ps1") -NoInstall
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }
}
if (-not (Test-Path $asi)) {
    Write-Error "Build artifact missing: $asi (run without -SkipBuild)"
    exit 1
}
$asiKb = [math]::Round((Get-Item $asi).Length / 1KB, 1)
Write-Host "ASI: $asi ($asiKb KB)"

# --- 3. Verify dist staging area ---
$stagedAssets = Join-Path $repo "dist\SpeedoIV"
$requiredFiles = @(
    "dist\SpeedoIV\Config.ini",
    "dist\SpeedoIV\Default\Bck.png",
    "dist\SpeedoIV\Default\Pin.png",
    "dist\SpeedoIV\Default\NOTICE.txt"
)
foreach ($rel in $requiredFiles) {
    $abs = Join-Path $repo $rel
    if (-not (Test-Path $abs)) {
        Write-Error "Required asset missing: $rel -- regenerate dist\ contents before publishing."
        exit 1
    }
}

# --- 4. Build the zip in a temporary staging folder ---
$stage = Join-Path $env:TEMP "speedoiv-ce-publish-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    # plugins/
    $stagePlugins = Join-Path $stage "plugins"
    New-Item -ItemType Directory -Path $stagePlugins -Force | Out-Null
    Copy-Item $asi (Join-Path $stagePlugins "SpeedoIV-CE.asi") -Force

    # SpeedoIV/
    Copy-Item $stagedAssets (Join-Path $stage "SpeedoIV") -Recurse -Force

    # End-user README -- generated so the version stays in sync
    $shaShort = ""
    try { $shaShort = (git -C $repo rev-parse --short HEAD 2>$null).Trim() } catch {}
    $buildDate = Get-Date -Format "yyyy-MM-dd"
    $readme = @"
SpeedoIV-CE v$version
=====================
A speedometer overlay for Grand Theft Auto IV: The Complete Edition.

Build:    $buildDate$(if ($shaShort) { "  (commit $shaShort)" })
ASI size: $asiKb KB

----------------------------------------------------------------------
PREREQUISITES
----------------------------------------------------------------------
1. Grand Theft Auto IV: The Complete Edition.
   Tested on v1.2.0.43 (Rockstar Launcher / Steam build, late 2025).
   Older 1.0.x versions are NOT supported.

2. Ultimate ASI Loader v9.x+ installed as dinput8.dll in the game
   folder. Download from:
       https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
   The latest release is recommended.

3. FusionFix v5.x+ installed (provides d3d9.dll proxy + DXVK bridge).
   Strongly recommended for stability. Download from:
       https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/releases
   Tested with FusionFix 5.0.1 in DXVK mode (d3d9.cfg has API = 1).

----------------------------------------------------------------------
INSTALL
----------------------------------------------------------------------
Extract THIS zip directly into your GTA IV install folder (the folder
containing GTAIV.exe). It will place files like this:

    <game>\plugins\SpeedoIV-CE.asi
    <game>\SpeedoIV\Config.ini
    <game>\SpeedoIV\Default\Bck.png
    <game>\SpeedoIV\Default\Pin.png
    <game>\SpeedoIV\Default\NOTICE.txt

Existing SpeedoIV\ contents are NOT overwritten if the zip is
extracted with "skip existing" -- delete the SpeedoIV\ folder first
if you want a clean reset.

----------------------------------------------------------------------
CONTROLS
----------------------------------------------------------------------
F6          Toggle speedometer on/off
(rebind via the ToggleKey entry in Config.ini)

The dial automatically hides:
    - when on foot
    - while the pause / map / brief / cutscene menus are showing
    - during loading transitions

----------------------------------------------------------------------
CONFIGURATION
----------------------------------------------------------------------
Edit <game>\SpeedoIV\Config.ini at any time, even while the game is
running. Changes take effect within ~1 second; no restart needed.

Common knobs:
    Debug          true / false  (default false -- silent log)
    ToggleKey      117 = F6, 118 = F7, 119 = F8, 96 = Numpad 0
    ScreenAlign    BL / BR / TL / TR
    PositionX/Y    pixel offset from the chosen corner
    SizeX/Y        displayed sprite size in pixels
    Alpha          0..255 opacity
    MaxSpeed       top of the dial reading in km/h
    SweepDeg       needle arc from 0 to MaxSpeed
                   (280 = default dial; tune if you ship a custom skin)
    SkinFolder     subfolder of SpeedoIV\ to load Bck.png + Pin.png from

----------------------------------------------------------------------
CUSTOM SKINS
----------------------------------------------------------------------
Drop a folder beside Default\ -- e.g. SpeedoIV\MySkin\ -- containing
your own Bck.png (dial face) and Pin.png (needle, pointed at the 0
mark). Set SkinFolder = MySkin in Config.ini.

----------------------------------------------------------------------
TROUBLESHOOTING
----------------------------------------------------------------------
- Speedometer never appears:
    Set Debug = true in Config.ini, relaunch, check the file
    <game>\SpeedoIV-CE.log for entries like
        d3d9hook: Hook installed
        Game init: player=OK ui=OK ...
    If 'player=FAILED', the ASI didn't recognise your game build
    -- only CE v1.2.0.43 is supported.

- Needle stays at zero while driving:
    Same as above. With Debug = true the log will report 'velOff=0x1270'
    on a working install. Anything else means the pattern scan picked
    a wrong offset; please file an issue with the log attached.

- Needle position wrong at top speed:
    Tune SweepDeg in Config.ini. 280 matches the bundled dial.

- Game crashes on launch:
    Make sure Ultimate ASI Loader (dinput8.dll) is installed and that
    no other speedometer ASI is in plugins\ at the same time.

- Speedometer hides during pause but stays visible while driving (good)
  even though the rest of the HUD disappears (e.g. during certain
  cutscenes):
    File an issue with steps + a Debug = true log.

----------------------------------------------------------------------
CREDITS
----------------------------------------------------------------------
- Bundled dial and needle artwork (Bck.png / Pin.png in
  SpeedoIV\Default\) is (c) retarded_chicken from the original
  SpeedoIV skin pack (2009). Only the textures are reused; none of
  the original SpeedoIV code is included.
- Game-internal patterns (FindPlayerPed, FindPlayerVehicle, pause /
  menu state) derived from FusionFix by ThirteenAG, GPL-3.0.
  https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix
- DXVK by Philip Rebohle -- the reason modern GTA IV is playable at all.

SpeedoIV-CE source: see the project README in the source repository.
Released under GPL-3.0, same as FusionFix.
"@
    Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding utf8

    # --- 5. Zip it ---
    New-Item -ItemType Directory -Path $out -Force | Out-Null
    $zipName = "SpeedoIV-CE-v$version.zip"
    $zipPath = Join-Path $out $zipName
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $zipPath -CompressionLevel Optimal

    $zipKb = [math]::Round((Get-Item $zipPath).Length / 1KB, 1)
    Write-Host ""
    Write-Host "Wrote $zipPath ($zipKb KB)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Contents:"
    & 'C:\Windows\System32\tar.exe' -tf $zipPath 2>$null | ForEach-Object { Write-Host "  $_" }
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
