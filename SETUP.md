# SETUP.md

How to set up your local environment if `ENVIRONMENT.md` is missing.
After completing setup, fill in `ENVIRONMENT.md` with your actual paths.

## Prerequisites

1. **GTA IV Complete Edition** (v1.2.0.43, Rockstar Launcher build).
2. **FusionFix 5.x or newer** installed in the game folder (provides `d3d9.dll`
   and `plugins/GTAIV.EFLC.FusionFix.asi`).
3. **Ultimate ASI Loader** installed as `dinput8.dll` in the game folder
   (any version 9.x).
4. **Windows 10/11 x64**.
5. **PowerShell 5.1 or 7+**.
6. **7-Zip** (for extracting source archives if needed).

## Install 32-bit MinGW GCC

This project targets 32-bit Windows (GTA IV is a 32-bit process). You need
the **i686** MinGW build, not x86_64.

Recommended: WinLibs 13.2.0 i686 posix-dwarf-ucrt, or any equivalent.

```powershell
$url = "https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev1/i686-13.2.0-release-posix-dwarf-ucrt-rt_v11-rev1.7z"
$dest = "$env:TEMP\opencode\mingw32.7z"
Invoke-WebRequest -Uri $url -OutFile $dest
& "C:\Program Files\7-Zip\7z.exe" x $dest -o"$env:TEMP\opencode\mingw32"
```

Verify:
```powershell
& "$env:TEMP\opencode\mingw32\mingw32\bin\g++.exe" --version
# Expect: gcc.exe (i686-posix-dwarf-rev1, Built by MinGW-Builds project) 13.2.0
```

The MinGW distribution ships with `libd3dx9_40.a` and the `d3dx9.h` headers
needed for sprite/texture rendering. No DirectX SDK install required.

## SpeedoIV Skin Assets

The project ports the original SpeedoIV by o!nko!nk (2009) with the
"retarded_chicken" skin (v1.0.1, 2020). Download the skin archive and
extract its `SpeedoIV/` subfolder into the game directory:

```
<game folder>\SpeedoIV\
    Config.ini
    Default\
        Bck.png      (modern dial)
        Bck_orig.png (alternate dial)
        Pin.png      (needle)
        Pin_orig.png (alternate needle)
```

Source: search for `1672676969_SpeedoIV_skin_ by_retarded_chicken_v1.0.1.7z`
on GTA modding sites if you don't already have it.

## Fill in ENVIRONMENT.md

Create `ENVIRONMENT.md` (it's `.gitignore`d) with at minimum:
- Game install path
- Path to 32-bit `g++.exe`
- Path to 7-Zip
- Display resolution and aspect ratio

See the original `ENVIRONMENT.md` (or `AGENTS.md` references to it) for the
expected keys.

## Build and Deploy

Create a `deploy.ps1` script in the repo root (it's `.gitignore`d -- each
developer keeps their own with local paths). Template:

```powershell
# deploy.ps1 - Build, kill game, deploy ASI, relaunch
$ErrorActionPreference = 'Stop'
$gcc        = "<path to i686 g++.exe>"
$src        = "$PSScriptRoot\speedometer.cpp"
$out        = "$PSScriptRoot\build\SpeedoIV-CE.asi"
$gameDir    = "<path to GTA IV install>"
$gameExe    = "$gameDir\GTAIV.exe"
$pluginDir  = "$gameDir\plugins"

# Build
Write-Host "Building..." -NoNewline
New-Item -ItemType Directory -Path "$PSScriptRoot\build" -Force | Out-Null
& $gcc $src `
    -shared -o $out `
    -std=c++17 -O2 -s -static `
    -I $PSScriptRoot `
    -ld3dx9_40 -ld3d9 -lgdi32 -luser32 -lkernel32 `
    "-Wl,--kill-at" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host " FAILED" -ForegroundColor Red; exit 1 }
$size = [math]::Round((Get-Item $out).Length / 1KB, 1)
Write-Host " OK ($size KB)"

# Kill running game
$p = Get-Process "GTAIV" -ErrorAction SilentlyContinue
if ($p) {
    Write-Host "Killing GTAIV (PID $($p.Id))..."
    $p | Stop-Process -Force
    Start-Sleep -Seconds 3
}

# Deploy and launch
Copy-Item $out (Join-Path $pluginDir "SpeedoIV-CE.asi") -Force
Write-Host "Deployed to $pluginDir"
Start-Process -FilePath $gameExe -WorkingDirectory $gameDir
Write-Host "Game launched"
```

Then run:
```powershell
cd <repo>
.\deploy.ps1
```

## Verifying the Hook

After launch, check the runtime log (path in ENVIRONMENT.md). You should see:
- `d3d9hook: FOUND RAGE wrapper at +0x250E9` (or similar)
- `d3d9hook: Hooked Reset` and `Hook installed`
- `Game init: offsets OK`
- `D3D resources loaded OK`
- `Frame N: visible=1 speed=X.X resOk=1`

If `speed` stays at 0 while you're driving, re-launch and immediately drive
fast (>30 km/h) -- the pattern scanner picks the candidate with the highest
reported speed, so you need to be moving when it runs.

## Config Live-Reload

`Config.ini` is re-read every ~1 second. You can edit position/size/colors
without restarting the game.
