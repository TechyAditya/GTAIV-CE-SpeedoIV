# AGENTS.md

Instructions for AI agents working on this codebase.

## Project

**SpeedoIV-CE**: a 32-bit ASI plugin for GTA IV Complete Edition
(v1.2.0.43) that renders an analog speedometer overlay using D3DX9
sprites. Coexists with FusionFix + DXVK.

The plugin is a clean reimplementation that does **not** depend on the
deprecated ScriptHook v0.5.1 (which only supports 1.0.4.0 - 1.0.7.0).
None of the original SpeedoIV (2009) code is reused; only the dial /
needle textures are.

## Working with This Codebase

**To find current status**:

1. Tail the last ~50 lines of `ISSUES.md` -- the most recent findings
   and what's been tried.
2. `git log --oneline -10` for recent commits.
3. `git status` + `git diff` for uncommitted in-progress work.
4. If you need runtime context, read the log file (path in
   `ENVIRONMENT.md`). Note: with `Debug = false` in `Config.ini`, the
   file only exists if a warning/error happened. Set `Debug = true` in
   the live `Config.ini` to start producing a fresh verbose log on next
   game launch.

**Before committing**:

1. Update `ISSUES.md` with any new findings, dead ends, or fixes from
   this session. **Append a new numbered section** at the bottom -- do
   not rewrite history.
2. Stage and commit code + the `ISSUES.md` update **together** so future
   readers can correlate the diff with the explanation.

`ISSUES.md` is the project's institutional memory. Treat it as
append-only documentation of what was tried, what worked, and why.

## Documentation Map

| File | Purpose |
|---|---|
| `README.md`               | For humans: install, configure, credits, layout |
| `AGENTS.md`               | (this file) Conventions, deployment, hook architecture, release flow |
| `SETUP.md`                | Bootstrap (`setup.ps1`) details + manual fallback |
| `ENVIRONMENT.md`          | Local paths + observed memory layout (gitignored; agent-maintained) |
| `FusionFix.md`            | How FusionFix works and how our patterns relate |
| `ISSUES.md`               | Chronological history of bugs + how they were diagnosed |
| `RELEASE_NOTES.md`        | Body of the **next** GitHub release. Rewritten each version. |
| `RELEASE_NOTES.md.template` | Skeleton for writing `RELEASE_NOTES.md` for a new version |
| `VERSION`                 | Single line semver string driving releases |
| `LICENSE`                 | GPL-3.0 verbatim |

If `ENVIRONMENT.md` is missing, run `.\setup.ps1` (idempotent; safe to
re-run). After the first run it becomes agent/human-maintained and
`setup.ps1` will refuse to overwrite it.

## Repository Layout

```
sdk/                Reusable headers. Drop into any GTA IV CE ASI mod.
    all.h             Umbrella include
    logging.h         Timestamped file logger with Debug-flag gating
    memory.h          Safe read helpers + pointer validity predicates
    scanner.h         Pattern scanner over a memory range
    game.h            Main executable discovery (base + size)
    ui.h              Game pause / menu state detection
    player.h          Player ped + vehicle + speed
    d3d9_hook.h       GTA IV RAGE D3D9 device hook (EndScene + Reset)
    render.h          D3DX9 sprite helpers (texture, state reset, transforms)

tools/              Dev-only utilities. `.\build.ps1 -Tools` builds them.
    probe_vehicle.cpp   Dump player chain + vehicle struct (one snapshot)
    probe_dynamic.cpp   Time-series sampler -- finds the real velocity offset
    probe_pause.cpp     Live read of pause/menu globals
    find_real.cpp       Verify a known CPedFactory address resolves
    debug_scan.cpp      Scan game memory for CPedFactory candidates
    inject_hook.cpp     External EndScene-hook injector
    live_probe.cpp      Dump loaded modules + device vtables
    live_patcher.cpp    Hot-patch bytes in a running game
    device_scan.cpp     Enumerate D3D9 device candidates
    restore_hook.cpp    Restore a manually-patched vtable entry

dist/               Release staging area
    SpeedoIV/         Canonical end-user config + skin shipped in releases
        Config.ini      Default settings
        Default/
            Bck.png       Dial face (retarded_chicken artwork, 2009)
            Pin.png       Needle (points at the 0 mark)
            NOTICE.txt    Credits for the bundled artwork
    *.zip             Built by publish.ps1, gitignored

.github/workflows/
    release.yml       Tag-triggered CI: validate, build, publish GitHub Release

speedometer.cpp       The ASI itself -- thin glue over the sdk/ modules
build.ps1             Build SpeedoIV-CE.asi (and optionally tools/)
setup.ps1             One-time MinGW + ENVIRONMENT.md + deploy.ps1 bootstrap
publish.ps1           Package a release zip locally (also used by CI)

VERSION               Single-line semver release version
RELEASE_NOTES.md      Notes body for the next release (required by CI)
LICENSE               GPL-3.0

build/                Compiled artifacts (gitignored)
ENVIRONMENT.md        Per-clone paths + memory layout (gitignored)
deploy.ps1            Per-developer deploy wrapper (gitignored)
```

## Deployment Flow

```
.\deploy.ps1
```

That script (generated by `setup.ps1`):

1. Kills the running `GTAIV.exe`, waits 3 seconds.
2. Calls `.\build.ps1`, which builds `build\SpeedoIV-CE.asi` and
   copies it into `<game>\plugins\` (driven by the `GAME_DIR = "..."`
   line in `ENVIRONMENT.md`).
3. Relaunches the game.

For visual tweaks only (position, size, alpha, sweep, max speed), do
NOT restart: edit `<game>\SpeedoIV\Config.ini` and the running plugin
re-reads it within ~1 second.

## Logging Model

The logger has three calls and one flag:

- `sdk::Log(fmt, ...)`     -- info (heartbeats, config snapshots)
- `sdk::LogWarn(fmt, ...)` -- soft failures
- `sdk::LogError(fmt, ...)`-- hard failures

Behavior depends on `Debug` in `Config.ini`:

- **`Debug = true`**: log file is opened (truncated) on first call,
  every call writes immediately.
- **`Debug = false`** (default): log file is **not** touched. Info
  calls go into a 64-entry ring buffer. On the first warn/error, the
  file is lazy-opened in **append** mode, the buffered context is
  flushed under a `--- buffered context ---` header, then the warn/error
  line is written. This way:
  - Quiet by default (clean game folder, no perpetual log growth).
  - When something breaks, you get the surrounding context for free.
  - History from previous sessions is never wiped unless `Debug=true`.

When extending the SDK, prefer `LogWarn`/`LogError` for things that
matter and `Log` for verbose diagnostics. Don't bypass the logger
with raw `fopen` calls.

## Hook Architecture

The naive "create a dummy `IDirect3DDevice9` via `Direct3DCreate9` and
patch its vtable[42]" approach **does not work** with FusionFix/DXVK
because DXVK builds per-instance vtables. See `ISSUES.md` #5/#6.

What works: GTA IV (RAGE) wraps `IDirect3DDevice9` in its own class
whose vtable lives inside `GTAIV.exe`. We pattern-scan for the global
pointer to that wrapper:

```
A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??
```

Among multiple matches, pick the one whose vtable lives in the game
module range. Then patch `vt[42]` (EndScene -- per-frame) and `vt[16]`
(Reset -- alt+tab / resolution change).

```cpp
#include "sdk/all.h"

d3d9hook::SetCallback(OnFrame);
d3d9hook::SetLostCallback(OnLost);
auto mod = sdk::GetGameModule();
d3d9hook::Install(mod.base, mod.size);
```

## Game State Access

### Player vehicle + speed

```cpp
sdk::player::Init(mod.base, mod.size);   // pattern-scan + cache helpers
float kmh = sdk::player::GetSpeed();     // -1 if on foot
uintptr_t veh = sdk::player::GetVehicle(); // 0 if on foot
```

Internally, `Init` finds the game's `FindPlayerPed` and
`FindPlayerVehicle` helper functions and stores them as
`__cdecl(int)` pointers. `GetSpeed` calls `FindPlayerVehicle(0)` each
frame and reads the velocity vector at `CVehicle + 0x1270`.

The factory-scan / scored-candidate approach from earlier iterations
is gone -- it was unreliable. Don't reintroduce it. See `ISSUES.md`
#17/#20.

### Pause / menu state

```cpp
sdk::ui::Init(mod.base, mod.size);
if (sdk::ui::IsGamePaused()) { /* user menu, code-pause, or any menu */ }
if (sdk::ui::IsUserPaused()) { /* Esc menu only */ }
if (sdk::ui::IsMenuOpen())   { /* map, brief, stats, etc. */ }
```

Patterns are FusionFix-derived (`source/comvars.ixx:2615` and
`:2634`). Resolved addresses on the live build are logged on init.

## Rendering Architecture

```cpp
sdk::render::Reset2DState(dev);          // wipe GTA's tonemap state etc.
sdk::render::LoadTexture(dev, path, &tex); // D3DPOOL_DEFAULT for DXVK
sdk::render::ResolveAlignment("BL", scrW, scrH, sizeX, sizeY,
                              posX, posY, &drawX, &drawY);
D3DXMATRIX mat;
sdk::render::BuildSpriteTransform(&mat, texW, texH,
                                  scaleX, scaleY, centreX, centreY,
                                  radians);
sprite->SetTransform(&mat);
sprite->Draw(tex, NULL, NULL, NULL, color);
```

`BuildSpriteTransform` wraps the correct usage of
`D3DXMatrixTransformation2D` for the scale-then-rotate-around-centre
case. The naive call has a subtle bug where the rotation pivot ends
up in pre-scale texel coords; see `ISSUES.md` #16.

`Reset2DState` MUST be called every frame before
`D3DXSprite::Begin` -- GTA leaves the device with pixel shaders bound
and depth-test enabled, which makes sprite draws appear black.

## Code Conventions

- **C++17**, 32-bit only (i686-w64-mingw32 MinGW for Win32).
- **No MSVC SEH** (`__try / __except`). Use `IsBadReadPtr` (via
  `sdk::IsReadable`) for safe probing.
- Public helpers live in `sdk::` or `d3d9hook::` namespaces.
- Logging via `sdk::Log` / `LogWarn` / `LogError` -- never raw `fopen`.
- Keep `speedometer.cpp` thin. Reusable logic goes into `sdk/`.
- Built artifact must be `pei-i386` -- verify with `objdump -f`.
- **No absolute paths in committed source code**. Paths live in
  `ENVIRONMENT.md` and `deploy.ps1`, both gitignored.

## Common Tasks

**Add a new game memory read**: extend an existing `sdk/<module>.h` or
add a new one. Follow the safe-read pattern: probe with `sdk::IsReadable`
before dereferencing, validate pointer ranges with `sdk::IsHeapPtr` /
`sdk::IsValidPtr`. If you're searching for a new field offset, write
a probe in `tools/` first -- single-snapshot probes can match by
coincidence (see `ISSUES.md` #20).

**Find a new pattern**: write a tool in `tools/` modeled on
`probe_pause.cpp`. Read game memory via `ReadProcessMemory` from an
external process (safer than in-plugin code; can be re-run while you
iterate).

**Tune visuals**: edit `<game>\SpeedoIV\Config.ini`. No code change.

**Add a new render element**: extend the render block in
`speedometer.cpp`. Use the helpers in `sdk::render::`. Don't forget
`Reset2DState` if you're starting a new draw outside the existing
sprite Begin/End pair.

**Verify the hook fires**: set `Debug = true` in `Config.ini`, launch
the game, look for `Frame N: ... resOk=1` lines every 300 frames in
the log.

## Anti-Patterns to Avoid

- Don't use `Direct3DCreate9` to make a dummy device for hook install.
  Doesn't work with DXVK.
- Don't use `D3DXSPRITE_DONOTSAVESTATE`.
- Don't hook the wrapper's Present (`vt[17]`) -- the game uses
  EndScene (`vt[42]`).
- Don't use `D3DPOOL_MANAGED` for textures. Use `D3DPOOL_DEFAULT` and
  release on the Reset callback (see `d3d9hook::SetLostCallback`).
- Don't call `GetForegroundWindow` from inside the render callback --
  observed to crash during DXVK transitional states.
- Don't reintroduce the scored CPedFactory scan. It picks the wrong
  candidate when stationary. The FusionFix-style `FindPlayerVehicle`
  function-call approach is the correct one.
- Don't trust a single-snapshot probe match (e.g. "this offset reads
  38 km/h while I'm driving 38 km/h"). Verify by sampling over time
  with `probe_dynamic.exe`.

## Build Requirements

Static-linked. Runtime DLL dependencies:

- `KERNEL32`, `USER32`, `GDI32`, `D3D9` (game already loads these).
- `D3DX9_40` (typically present from FusionFix / DXVK installers;
  otherwise the Microsoft DirectX End-User Runtime provides it).

Final ASI is ~70-75 KB.

## Versioning + Releases

The release version lives in **`VERSION`** at the repo root -- a single
line, semver style (e.g. `1.0.0`). It is the single source of truth
for the released version number; `publish.ps1`, the bundled
`README.txt`, and the GitHub Actions release workflow all derive
from it.

### Release flow (compulsory steps)

Releases are produced by the **GitHub Actions workflow**
`.github/workflows/release.yml`. It triggers on a pushed semver tag
(`v*.*.*`) and refuses to publish unless every step below is in place.

1. **Edit `VERSION`** -- one line, no leading `v`, no trailing
   whitespace. Follow the bump policy:
   - **Patch** (`1.0.0 -> 1.0.1`): bug fixes, doc-only changes,
     internal refactors that don't change behaviour.
   - **Minor** (`1.0.0 -> 1.1.0`): new user-visible features
     (additional config knobs, new commands, additional SDK modules)
     that remain backward-compatible.
   - **Major** (`1.0.0 -> 2.0.0`): breaking changes -- removed
     config keys, renamed config keys without migration, renamed SDK
     headers, game-version compatibility breaks.

2. **Rewrite `RELEASE_NOTES.md`** for this version. The file becomes
   the GitHub Release body verbatim. Use `RELEASE_NOTES.md.template`
   as the structure. Write it for end users -- engineering detail
   belongs in `ISSUES.md` and commit messages, not in the release
   notes. The workflow rejects empty / short notes (<64 bytes).

3. **Update `ISSUES.md`** with any new findings, fixes, or design
   changes covered by this release. Append a numbered section --
   never rewrite history.

4. **Update `dist/SpeedoIV/Config.ini`** if any new config keys were
   added to `speedometer.cpp`, so upgraders see the new options.

5. **Commit all of the above together**:

   ```
   git add VERSION RELEASE_NOTES.md ISSUES.md dist/SpeedoIV/Config.ini ...
   git commit -m "Release vX.Y.Z: <one-line summary>"
   git push
   ```

6. **Tag and push**. The tag MUST be `v` + the contents of `VERSION`
   verbatim, or the workflow will fail the validate step:

   ```
   git tag "v$(Get-Content VERSION -Raw | ForEach-Object Trim)"
   git push origin --tags
   ```

7. **Watch the Actions tab**. The workflow:
   - Validates `tag == "v<VERSION>"`.
   - Validates `RELEASE_NOTES.md` exists and is non-trivial.
   - Installs MinGW 13.2.0 i686 (cached between runs).
   - Runs `build.ps1 -NoInstall`, then `publish.ps1 -SkipBuild`.
   - Uploads the zip as a workflow artifact.
   - Creates (or replaces) the GitHub Release at
     `/releases/tag/v<VERSION>` with `RELEASE_NOTES.md` as the body
     and the zip attached.

   The release appears at:
   `https://github.com/<owner>/<repo>/releases/tag/v<VERSION>`

### Local-only "publish"

If you want a local `dist\SpeedoIV-CE-v<VERSION>.zip` without
involving GitHub:

```
.\publish.ps1
```

Same script the Actions runner uses; same output. The zip stays on
your machine. Don't commit it -- `.gitignore` excludes `dist/*.zip`.

### What is NOT a release

- Editing `VERSION` does not by itself publish anything. The tag push
  is the trigger.
- Local `.asi` builds in `build/` and the per-user
  `<game>\plugins\SpeedoIV-CE.asi` installed by `deploy.ps1` are
  dev-time artefacts. They do not bump `VERSION` and do not touch
  `dist/`.
- Pushing to `master` without a tag never publishes a release.

### Files that ARE shipped artifacts

These are checked in and act as the canonical source for what users
receive:

- `VERSION` -- the version string.
- `RELEASE_NOTES.md` -- the release body for the **next** release.
- `dist/SpeedoIV/Config.ini` -- end-user defaults.
- `dist/SpeedoIV/Default/` -- end-user dial / needle / NOTICE.
- `LICENSE` -- GPL-3.0.
- All of `sdk/`, `tools/`, `speedometer.cpp`, `*.ps1`, `*.md` --
  the source the workflow rebuilds from.

### Re-running a release

If a release tag is already published and you need to rebuild it
(e.g. workflow was broken the first time):

1. Delete the release in the Actions UI or via
   `gh release delete v<X.Y.Z> --yes`.
2. Delete the tag locally and remotely:
   `git tag -d v<X.Y.Z>; git push origin :refs/tags/v<X.Y.Z>`.
3. Fix the issue. Re-tag. Re-push. The workflow auto-replaces the
   release if one already exists for the same tag.

## Git Hygiene

- Source + docs only. `*.asi`, `*.exe`, `build/`, `dist/*.zip`,
  `ENVIRONMENT.md`, `deploy.ps1`, `*.log` are gitignored.
- `VERSION`, `dist/SpeedoIV/Config.ini`, `dist/SpeedoIV/Default/*`
  are checked in -- they ARE the shipped artifacts.
- Imperative present tense commit messages.
- Ignore the CRLF line-ending warnings; they're harmless on Windows.
