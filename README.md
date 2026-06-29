# SpeedoIV-CE

A working speedometer overlay for **Grand Theft Auto IV: The Complete
Edition (v1.2.0.43)**. Designed to coexist cleanly with FusionFix + DXVK.

The dial appears when you're driving, hides on foot, and disappears
during pause, map, brief and cutscene transitions. Toggle on/off with
**F6**. Live-tunable position, size, alpha and needle sweep via
`Config.ini` (re-read once per second -- no restart).

![needle moves](https://placehold.co/600x40/333/fff?text=A+real+speedometer+for+CE)

## Install

1. Install **GTA IV: The Complete Edition** (Rockstar Launcher or Steam,
   version 1.2.0.43).
2. Install [**FusionFix**](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
   into the game folder. Strongly recommended -- it provides the DXVK
   bridge SpeedoIV-CE expects and fixes countless other engine bugs.
3. Install [**Ultimate ASI Loader**](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   as `dinput8.dll` in the game folder.
4. Drop `SpeedoIV-CE.asi` into `<game>\plugins\` (built from this repo,
   see Build below).
5. Drop the `SpeedoIV\` folder from this repo into `<game>\` so that
   `<game>\SpeedoIV\Config.ini`, `<game>\SpeedoIV\Default\Bck.png`,
   `<game>\SpeedoIV\Default\Pin.png` exist.
6. Launch the game. Press **F6** to hide/show.

## Configure

Edit `<game>\SpeedoIV\Config.ini` while the game is running. Changes
take effect within ~1 second.

| Setting       | Default | Notes                                                          |
|---------------|---------|----------------------------------------------------------------|
| `Debug`       | `false` | `true` = full verbose log; `false` = silent (lazy on warn/err) |
| `Autostart`   | `true`  | Speedometer visible on game start                              |
| `ToggleKey`   | `117`   | VK code. 117 = F6. Avoid 116 (F5 = GTA quicksave)              |
| `ScreenAlign` | `BL`    | Two chars: T/B + L/R                                           |
| `PositionX/Y` |         | Offset from chosen corner, in pixels                           |
| `SizeX/Y`     |         | Displayed sprite size, in pixels                               |
| `Angle`       | `0`     | Static rotation applied to whole speedometer                   |
| `SweepDeg`    | `280`   | Total needle arc from 0 to MaxSpeed. Tune to your dial         |
| `MaxSpeed`    | `280`   | Top-of-dial reading in km/h                                    |
| `Alpha`       | `220`   | 0-255 opacity                                                  |
| `SkinFolder`  | `Default` | Subfolder of `SpeedoIV\` to load `Bck.png` and `Pin.png` from |

To ship a different look, drop a folder beside `Default\` containing
your own `Bck.png` (dial) and `Pin.png` (needle, pointing at the
"0" mark of the dial) and set `SkinFolder = MyFolder` in `Config.ini`.

## Build

From a clone of this repo:

```powershell
.\setup.ps1     # one-time: downloads 32-bit MinGW, generates ENVIRONMENT.md + deploy.ps1
.\build.ps1     # builds SpeedoIV-CE.asi into build\ and copies it to <game>\plugins\
.\deploy.ps1    # full cycle: kill running game, build, copy, relaunch
.\publish.ps1   # build + package dist\SpeedoIV-CE-v<VERSION>.zip for release
```

See [SETUP.md](SETUP.md) for the full bootstrap details, including the
manual fallback.

The plugin is built with 32-bit MinGW i686 GCC against the bundled
`d3dx9_40.lib`. GTA IV itself is a 32-bit process -- this is not
optional.

## Releases

Released via [GitHub Actions](.github/workflows/release.yml) on every
pushed semver tag. Latest version is in
[**VERSION**](VERSION); release zips appear at
[**Releases**](../../releases).

To cut a release:

```powershell
# 1. Edit VERSION (e.g. 1.0.0 -> 1.0.1)
# 2. Rewrite RELEASE_NOTES.md for end users (template in RELEASE_NOTES.md.template)
# 3. Append a section to ISSUES.md if there's anything worth recording
# 4. Update dist/SpeedoIV/Config.ini if you added new config keys
git commit -am "Release v1.0.1: ..."
git push
git tag v1.0.1
git push origin --tags
# The Release workflow takes it from here -- watch the Actions tab.
```

The workflow validates that the tag matches `VERSION`, requires a
non-trivial `RELEASE_NOTES.md`, builds the ASI in CI using the same
MinGW toolchain we use locally, runs `publish.ps1`, and creates the
release with the zip attached and your notes as the body.

To build the zip locally (without pushing):

```powershell
.\publish.ps1
# -> dist\SpeedoIV-CE-v<VERSION>.zip
```

See [AGENTS.md](AGENTS.md) "Versioning + Releases" for the bump
policy (patch / minor / major) and the full required-steps checklist.

## Code layout

```
sdk/                 reusable headers, drop into any GTA IV CE ASI mod
    all.h              umbrella include
    logging.h          timestamped file logger with Debug-flag gating
    memory.h           safe memory read helpers + pointer predicates
    scanner.h          pattern scanner over a memory range
    game.h             main executable discovery
    ui.h               game pause / menu detection
    player.h           player ped + vehicle + speed
    d3d9_hook.h        RAGE D3D9 device hook (EndScene + Reset)
    render.h           D3DX9 sprite helpers (texture, state, transforms)

tools/               dev-only utilities (compile to standalone .exe via build.ps1 -Tools)
    probe_vehicle.cpp    dump player chain + vehicle struct
    probe_dynamic.cpp    time-series sampler -- finds which offset is REALLY velocity
    probe_pause.cpp      live read of m_UserPause / m_CodePause / m_MenuActive
    find_real.cpp        verify a known CPedFactory address resolves the chain
    debug_scan.cpp       scan game memory for CPedFactory candidates
    inject_hook.cpp      external EndScene-hook injector (early debugging)
    live_probe.cpp       probe live game state from outside
    live_patcher.cpp     hot-patch bytes into the running game
    device_scan.cpp      enumerate D3D9 device candidates
    restore_hook.cpp     un-patch after a botched hook attempt

SpeedoIV/            (not in repo) deployed by users into their game folder
                     Mirror of dist/SpeedoIV/ -- see dist/ below.

dist/                Release staging area
    SpeedoIV/          canonical end-user config + skin shipped in releases
        Config.ini       default settings
        Default/
            Bck.png        dial face artwork
            Pin.png        needle artwork (points at 0)
            NOTICE.txt     credits for the bundled artwork
    *.zip              built by publish.ps1, gitignored

speedometer.cpp      the actual ASI -- thin glue over the sdk/ modules
build.ps1            build the ASI and (optionally) tools/
setup.ps1            one-time MinGW + ENVIRONMENT.md + deploy.ps1 bootstrap
publish.ps1          package a release zip (reads VERSION)
VERSION              release version (semver, single line)
RELEASE_NOTES.md     body of the next GitHub release (required by CI)
LICENSE              GPL-3.0
.github/workflows/
    release.yml      tag-triggered CI: builds + publishes a GitHub release

README.md            this file
AGENTS.md            conventions + working-with-codebase guide
SETUP.md             bootstrap details (humans + agents)
ISSUES.md            chronological log of every bug + how it was diagnosed
FusionFix.md         notes on how FusionFix's source informed our patterns
ENVIRONMENT.md       per-clone paths (gitignored, generated by setup.ps1)
deploy.ps1           per-developer wrapper (gitignored, generated by setup.ps1)
```

## How it works (short version)

- **Hooking**: pattern-scans `GTAIV.exe` for the RAGE D3D9 wrapper's
  device pointer, then patches `vt[42]` (EndScene) and `vt[16]` (Reset)
  on its vtable. The wrapper lives inside the game image, not in
  `d3d9.dll`, so legacy approaches that hook a dummy device fail under
  DXVK.
- **Player vehicle**: pattern-scans for the game's internal
  `FindPlayerPed` / `FindPlayerVehicle` helpers and calls them as
  `__cdecl(int=0)`. Same code path `GET_CAR_CHAR_IS_USING` natives
  use. Way more reliable than guessing factory globals.
- **Velocity**: read at `CVehicle + 0x1270` -- a vec3 (m/s). Verified
  by `tools\probe_dynamic.exe`, which samples the struct three times
  one second apart and finds the one offset whose magnitude actually
  tracks driving.
- **Pause detection**: reads `CTimer::m_UserPause`,
  `CTimer::m_CodePause`, `CMenuManager::m_MenuActive` -- same byte
  globals FusionFix uses.

For the full story, including every dead end, read
[ISSUES.md](ISSUES.md). For the conventions an AI agent should follow
when extending this, read [AGENTS.md](AGENTS.md).

## Credits

- **Bundled artwork** (`Bck.png`, `Pin.png` in `SpeedoIV/Default/`) is
  (c) **retarded_chicken** from the original
  "SpeedoIV" skin pack (2009, gtaiv-mods.com). Only the textures are
  reused; none of the original code is included.
- **Game-internal patterns** (`FindPlayerPed`, `FindPlayerVehicle`,
  `m_UserPause`, `m_CodePause`, `m_MenuActive`) come from
  [**GTAIV.EFLC.FusionFix**](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
  by **ThirteenAG**, GPL-3.0. The actual offsets and `CVehicle+0x1270`
  velocity location were verified independently with the probe tools
  in `tools/`.
- **FusionFix** itself, by **ThirteenAG** and contributors -- without
  the DXVK bridge and the published source, this plugin would not
  exist.
- **DXVK**, by **Philip Rebohle** -- the reason GTA IV is playable on
  modern hardware at all.

## License

The SpeedoIV-CE code (everything under `sdk/`, `tools/` and
`speedometer.cpp`) is released under the same terms as FusionFix:
**GPL-3.0**. The bundled artwork is governed by its original author's
terms; see `SpeedoIV/Default/NOTICE.txt`.
