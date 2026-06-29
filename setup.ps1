# setup.ps1 - One-time bootstrap for SpeedoIV-CE development
#
# Does (idempotently):
#   1. Downloads + extracts 32-bit MinGW i686 GCC if not present.
#   2. Locates GTA IV Complete Edition install (Rockstar / Steam / Epic) OR
#      asks the user once and remembers.
#   3. Writes ENVIRONMENT.md with the discovered paths + display info.
#   4. Writes a per-developer deploy.ps1 from a template.
#   5. Verifies that FusionFix + Ultimate ASI Loader are installed in the
#      game folder. Warns (does NOT install) if missing.
#
# After running this, you can immediately `.\build.ps1` and `.\deploy.ps1`.
#
# Re-run any time -- it skips work that's already done.

[CmdletBinding()]
param(
    [string]$GameDir = "",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot

function Find-GameDir {
    param([string]$Hint)
    if ($Hint -and (Test-Path (Join-Path $Hint "GTAIV.exe"))) { return $Hint }

    $candidates = @(
        "$env:ProgramFiles\Rockstar Games\Grand Theft Auto IV - The Complete Edition",
        "$env:ProgramFiles(x86)\Rockstar Games\Grand Theft Auto IV - The Complete Edition",
        "$env:ProgramFiles\Steam\steamapps\common\Grand Theft Auto IV",
        "$env:ProgramFiles(x86)\Steam\steamapps\common\Grand Theft Auto IV"
    )
    # Also scan every fixed drive
    Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Used } | ForEach-Object {
        $candidates += "$($_.Root)Games\Grand Theft Auto IV - The Complete Edition"
        $candidates += "$($_.Root)SteamLibrary\steamapps\common\Grand Theft Auto IV"
    }
    foreach ($c in $candidates | Select-Object -Unique) {
        if (Test-Path (Join-Path $c "GTAIV.exe")) { return $c }
    }
    return $null
}

# --- 1. MinGW ---
$mingwBin = "$env:TEMP\opencode\mingw32\mingw32\bin\g++.exe"
if (-not (Test-Path $mingwBin) -or $Force) {
    Write-Host "Installing 32-bit MinGW GCC..." -ForegroundColor Cyan
    $url  = "https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev1/i686-13.2.0-release-posix-dwarf-ucrt-rt_v11-rev1.7z"
    $tmp  = "$env:TEMP\opencode"
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    $arc  = "$tmp\mingw32.7z"
    if (-not (Test-Path $arc) -or $Force) {
        Write-Host "  Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $arc -UseBasicParsing
    }
    $sevenZip = (Get-Command "7z.exe" -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) {
        foreach ($p in @("$env:ProgramFiles\7-Zip\7z.exe", "$env:ProgramFiles(x86)\7-Zip\7z.exe")) {
            if (Test-Path $p) { $sevenZip = $p; break }
        }
    }
    if (-not $sevenZip) {
        Write-Error "7-Zip not found. Install 7-Zip and re-run setup.ps1."
        exit 1
    }
    Write-Host "  Extracting via $sevenZip"
    & $sevenZip x $arc "-o$tmp\mingw32" -y | Out-Null
    if (-not (Test-Path $mingwBin)) {
        Write-Error "MinGW install failed (g++.exe not present after extract)"
        exit 1
    }
}
$ver = & $mingwBin --version | Select-Object -First 1
Write-Host "MinGW: $ver" -ForegroundColor Green

# --- 2. Game directory ---
$resolvedGame = Find-GameDir -Hint $GameDir
if (-not $resolvedGame) {
    $resolvedGame = Read-Host "Enter the full path to your GTA IV Complete Edition install (the folder containing GTAIV.exe)"
    if (-not (Test-Path (Join-Path $resolvedGame "GTAIV.exe"))) {
        Write-Error "GTAIV.exe not found in '$resolvedGame'. Aborting."
        exit 1
    }
}
Write-Host "Game: $resolvedGame" -ForegroundColor Green

# --- 3. ENVIRONMENT.md ---
$envPath = Join-Path $repo "ENVIRONMENT.md"
$disp = Get-WmiObject -Class Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution } |
        Select-Object -First 1 -Property CurrentHorizontalResolution, CurrentVerticalResolution
$resW = if ($disp) { $disp.CurrentHorizontalResolution } else { 1920 }
$resH = if ($disp) { $disp.CurrentVerticalResolution }   else { 1080 }

$envContent = @"
# ENVIRONMENT.md

Initial scaffold written by setup.ps1 on first run. After that, this
file is maintained by hand (or by agents) and setup.ps1 will refuse
to overwrite it. Delete the file and re-run setup.ps1 to regenerate
from scratch.

## build.ps1 install target

GAME_DIR = "$resolvedGame"

## Game

- Game install dir: ``$resolvedGame``
- Game executable: ``$resolvedGame\GTAIV.exe``
- Plugins folder: ``$resolvedGame\plugins``
- Speedometer assets folder: ``$resolvedGame\SpeedoIV``
- Runtime log: ``$resolvedGame\SpeedoIV-CE.log``

## Tools

- 32-bit MinGW GCC: ``$mingwBin``
- 7-Zip: ``$sevenZip``

## Display

- $resW x $resH

## Game Version

- GTA IV Complete Edition v1.2.0.43 (Rockstar Launcher / Steam).
- FusionFix 5.x active (``d3d9.dll`` in the game folder).
- DXVK API mode enabled when FusionFix's ``d3d9.cfg`` has ``API = 1``.

## Memory Layout (CE 1.2.0.43, observed)

Stable across launches (no ASLR):

- Game module base: 0x00FE0000  size: 0x01BE6400
- RAGE D3D9 wrapper dev: 0x024BC2B8, vtable: 0x01BAA294
  - vt[16] Reset = 0x0100C3A0
  - vt[17] Present = 0x010012F0
  - vt[42] EndScene = 0x01001770
- FindPlayerPed     = 0x014B1C10
- FindPlayerVehicle = 0x014B1C40
- g_currentPlayerSlot = 0x01C16F14
- g_PlayerInfoArray = 0x01D88808
- CPlayerInfo+0x598 -> CPed*
- CPed+0xB30 -> CVehicle*
- CPed+0x26C bit 2 = inVehicle flag
- CVehicle+0x1270 -> vec3 m/s (velocity)
- CTimer::m_UserPause  = 0x01D53590
- CTimer::m_CodePause  = 0x01D53591
- CMenuManager::m_MenuActive = 0x01D409F6

See ISSUES.md for the history of how these were found.
"@

if (Test-Path $envPath) {
    Write-Host "ENVIRONMENT.md already exists -- preserving it (agent-maintained, not regenerated by setup.ps1)." -ForegroundColor Yellow
} else {
    Set-Content -Path $envPath -Value $envContent -Encoding utf8
    Write-Host "Wrote $envPath (first-time scaffold; further edits are manual / agent-managed)" -ForegroundColor Green
}

# --- 4. deploy.ps1 ---
$deployPath = Join-Path $repo "deploy.ps1"
$deployBody = @"
# deploy.ps1 - Build + kill running game + copy ASI + relaunch.
# Generated by setup.ps1. Edit freely; .gitignored.
`$ErrorActionPreference = 'Stop'
`$ScriptDir = `$PSScriptRoot
`$gameDir   = "$resolvedGame"
`$gameExe   = Join-Path `$gameDir "GTAIV.exe"
`$pluginDir = Join-Path `$gameDir "plugins"

# Kill any running game instance FIRST so build.ps1 can overwrite the deployed ASI
`$p = Get-Process "GTAIV" -ErrorAction SilentlyContinue
if (`$p) {
    Write-Host "Killing GTAIV (PID `$(`$p.Id))..."
    `$p | Stop-Process -Force
    Start-Sleep -Seconds 3
}

# Build (build.ps1 will auto-copy to plugins\ when ENVIRONMENT.md is present)
& (Join-Path `$ScriptDir "build.ps1")
if (`$LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

Start-Process -FilePath `$gameExe -WorkingDirectory `$gameDir
Write-Host "Game launched"
"@

if ((Test-Path $deployPath) -and -not $Force) {
    Write-Host "deploy.ps1 already exists -- keeping it (use -Force to overwrite)." -ForegroundColor Yellow
} else {
    Set-Content -Path $deployPath -Value $deployBody -Encoding utf8
    Write-Host "Wrote $deployPath" -ForegroundColor Green
}

# --- 5. Verify prerequisites in the game folder ---
$asiLoader = Join-Path $resolvedGame "dinput8.dll"
$fusion    = Join-Path $resolvedGame "d3d9.dll"
$pluginDir = Join-Path $resolvedGame "plugins"
if (-not (Test-Path $asiLoader)) {
    Write-Warning "Ultimate ASI Loader (dinput8.dll) not found in $resolvedGame -- ASI plugins will not load. See README.md."
}
if (-not (Test-Path $fusion)) {
    Write-Warning "FusionFix (d3d9.dll) not found in $resolvedGame -- recommended for stable DXVK rendering. See README.md."
}
if (-not (Test-Path $pluginDir)) {
    New-Item -ItemType Directory -Path $pluginDir | Out-Null
    Write-Host "Created plugins folder: $pluginDir" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Next steps:"
Write-Host "  .\build.ps1         # build SpeedoIV-CE.asi -> build\ and copy to plugins\"
Write-Host "  .\deploy.ps1        # build + kill + redeploy + relaunch the game"
Write-Host "  .\build.ps1 -Tools  # also build everything in tools\ to build\*.exe"
