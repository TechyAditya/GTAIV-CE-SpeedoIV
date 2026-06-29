# SETUP.md

One-time bootstrap for new clones. Most of it is automated by
`setup.ps1`; this file documents what that script does and what to
do if you'd rather set things up by hand.

## Prerequisites (manual install)

1. **GTA IV Complete Edition** (v1.2.0.43, Rockstar Launcher or Steam).
2. **FusionFix 5.x+** dropped into the game folder (provides `d3d9.dll`
   that proxies DXVK / Vulkan, plus `plugins/GTAIV.EFLC.FusionFix.asi`).
   Get it from https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix.
3. **Ultimate ASI Loader** installed as `dinput8.dll` in the game folder
   (any 9.x release). Get it from
   https://github.com/ThirteenAG/Ultimate-ASI-Loader.
4. **Windows 10/11 x64**, **PowerShell 5.1+**, **7-Zip**.

## One-time script setup

From the repo root:

```powershell
.\setup.ps1
```

That does, idempotently:

1. Downloads + extracts 32-bit MinGW i686 GCC 13.2.0 to
   `%TEMP%\opencode\mingw32\` (only the first time).
2. Auto-detects (or prompts for once) the GTA IV install folder.
3. Generates `ENVIRONMENT.md` with the discovered paths + your display
   resolution + the verified memory layout of CE 1.2.0.43. Gitignored.
4. Generates `deploy.ps1` -- a single command that builds, kills the
   running game, copies the ASI into `plugins\`, and relaunches.
   Gitignored.
5. Verifies that FusionFix + ASI Loader are in place (warns if not).

Optional flags:

```powershell
.\setup.ps1 -GameDir "D:\Games\GTAIV"   # specify install path explicitly
.\setup.ps1 -Force                       # redownload MinGW + overwrite deploy.ps1
                                         # (ENVIRONMENT.md is never overwritten;
                                         #  delete it manually to regenerate)
```

After setup completes:

```powershell
.\build.ps1                 # build SpeedoIV-CE.asi -> build\ and (if ENVIRONMENT.md
                            # provides GAME_DIR) auto-install to <game>\plugins\
.\deploy.ps1                # full build + kill + redeploy + relaunch cycle
.\build.ps1 -Tools          # also build everything in tools\ to build\*.exe
.\build.ps1 -ToolsOnly      # skip ASI, just rebuild tools
```

## Manual fallback (no setup.ps1)

### 1. Install 32-bit MinGW

WinLibs 13.2.0 i686 posix-dwarf-ucrt or equivalent. Anything that gives
you `i686-w64-mingw32-g++.exe` with `libd3dx9_40.a` available is fine.

```powershell
$url  = "https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev1/i686-13.2.0-release-posix-dwarf-ucrt-rt_v11-rev1.7z"
$dest = "$env:TEMP\opencode\mingw32.7z"
Invoke-WebRequest -Uri $url -OutFile $dest
& "C:\Program Files\7-Zip\7z.exe" x $dest -o"$env:TEMP\opencode\mingw32"
```

Verify:

```powershell
& "$env:TEMP\opencode\mingw32\mingw32\bin\g++.exe" --version
# gcc.exe ... 13.2.0
```

### 2. Game skin assets

The bundled artwork lives in `<game>\SpeedoIV\Default\Bck.png` /
`Pin.png`. Originally extracted from
`1672676969_SpeedoIV_skin_ by_retarded_chicken_v1.0.1.7z`. Layout:

```
<game>\SpeedoIV\
    Config.ini
    Default\
        Bck.png      (dial face)
        Pin.png      (needle)
        NOTICE.txt   (credits)
```

To ship a different skin, drop a folder beside `Default\` containing
its own `Bck.png` / `Pin.png` and set `SkinFolder = MyFolder` in
`Config.ini`. The plugin re-reads the file every ~1 second.

### 3. ENVIRONMENT.md and deploy.ps1 by hand

If you didn't run `setup.ps1`, write the minimal `ENVIRONMENT.md`:

```
GAME_DIR = "C:\path\to\Grand Theft Auto IV - The Complete Edition"
```

`build.ps1` reads that line to auto-install after a successful build.
For a custom deploy loop, see the generated `deploy.ps1` (or copy the
template from `setup.ps1`).

## Verifying the install

After running the game once with the ASI loaded, check:

- The runtime log only appears if `Debug = true` in `Config.ini` OR
  there was a warning/error -- by design, Debug=false stays silent.
- With `Debug = true` the log should contain (in this order):
  - `SpeedoIV-CE starting...`
  - `Game module: base=0x00FE0000 size=0x01BE6400`
  - `d3d9hook: FOUND RAGE wrapper at +0x250E9 ...`
  - `d3d9hook: Hook installed`
  - `Game init: player=OK ui=OK (FindPlayerPed=0x014B1C10 ...)`
  - `D3D resources loaded OK`
  - `Frame N: visible=1 speed=X.X velOff=0x1270 resOk=1` every 300 frames.

If `Game init: player=FAILED`, your build's `FindPlayerPed` pattern
doesn't match -- you're not on CE 1.2.0.43, or the executable was
modified. Run `tools\probe_vehicle.exe` while the game is running to
re-extract the function address.

## Live tunable config

Edit `<game>\SpeedoIV\Config.ini` while the game is running -- the
plugin re-reads it about once per second. Useful knobs:

- `Debug` -- true for verbose log file, false for silent + lazy on error.
- `PositionX` / `PositionY`, `SizeX` / `SizeY`, `ScreenAlign` -- on-screen layout.
- `Angle` -- static rotation of the whole speedometer.
- `SweepDeg` -- needle arc from 0 to MaxSpeed (default 280 for the bundled dial).
- `MaxSpeed` -- top of the dial in km/h.
- `Alpha` -- 0-255 opacity.
- `ToggleKey` -- numeric VK code (117 = F6 by default; avoid F5 = GTA quicksave).
- `SkinFolder` -- name of the subfolder under `SpeedoIV\` to load `Bck.png`/`Pin.png` from.
