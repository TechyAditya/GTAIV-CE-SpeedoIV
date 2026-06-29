# AGENTS.md

Instructions for AI agents working on this codebase.

## Project

**SpeedoIV-CE**: a 32-bit ASI plugin for GTA IV Complete Edition (v1.2.0.43)
that renders an analog speedometer overlay using D3DX9 sprites. Compatible
with FusionFix (which loads via DXVK) and other CE mod stacks.

It is a working **rewrite** of the original SpeedoIV by o!nko!nk (2009)
that does NOT depend on the deprecated ScriptHook v0.5.1 (which only
supports GTA IV 1.0.4.0 - 1.0.7.0).

## Working with This Codebase

**To find current status**:
1. Tail the last ~50 lines of `ISSUES.md` to see the most recent findings
   and what's been tried.
2. Check `git log --oneline -10` for recent commits.
3. Check `git diff` and `git status` for uncommitted in-progress work.
4. Read the runtime log (path in `ENVIRONMENT.md`) if you need to see
   the last game session's output.

**Before committing**:
1. Update `ISSUES.md` with any new findings, dead ends, or fixes
   discovered during this session. Add as a new numbered section at
   the bottom -- don't rewrite history.
2. Stage and commit code + the `ISSUES.md` update together so future
   readers can correlate the diff with the explanation.

`ISSUES.md` is the project's institutional memory. Treat it as
append-only documentation of what was tried, what worked, and why.

## Documentation Map

| File | Purpose |
|---|---|
| `AGENTS.md`     | (this file) Conventions, deployment flow, hook architecture |
| `SETUP.md`      | How to set up your local environment from scratch |
| `ENVIRONMENT.md`| Local paths (gitignored, regenerate per machine) |
| `FusionFix.md`  | How FusionFix works and how we coexist with it |
| `ISSUES.md`     | History of bugs encountered + their fixes; useful for context |

If `ENVIRONMENT.md` is missing, follow `SETUP.md` to create it.

## Repository Layout

```
.gitignore
gtaiv_sdk.h        Reusable: logging, safe memory, pattern scanner, player state
d3d9_hook.h        Reusable: find game's RAGE D3D9 wrapper + patch its vtable
speedometer.cpp    Main: config, D3D resources, render. Thin (~300 lines).
debug_scan.cpp     Tool: dump all CPedFactory candidates from a live game
device_scan.cpp    Tool: find D3D9 device pointers stored in game globals
live_probe.cpp     Tool: dump loaded modules + device vtables of running game
inject_hook.cpp    Tool: live-injection POC that counts EndScene calls/sec
find_real.cpp      Tool: locate the real CPedFactory near a known code offset
restore_hook.cpp   Tool: restore a manually-patched vtable entry to its original

build/             Compiled artifacts (gitignored)
ENVIRONMENT.md     Local paths (gitignored)
deploy.ps1         Build + deploy + relaunch (gitignored; template in SETUP.md)
```

## Deployment Flow

1. Edit `speedometer.cpp`, `gtaiv_sdk.h`, or `d3d9_hook.h`.
2. Run `./deploy.ps1` from the repo root.
3. The script: compiles the ASI, kills `GTAIV.exe` if running, copies the
   ASI to the game's `plugins/` folder, launches the game.
4. Read the runtime log (path in `ENVIRONMENT.md`) for diagnostics:
   `[HH:MM:SS.mmm]` prefixed entries from `sdk::Log`.

For **visual** tweaks only (position, size, colors, max speed), do **not**
restart -- edit the game's `SpeedoIV/Config.ini` (path in `ENVIRONMENT.md`)
and the running ASI re-reads it every ~1 second.

## Hook Architecture (Important)

The naive approach -- create a dummy `IDirect3DDevice9` via `Direct3DCreate9`
and patch its `vtable[42]` -- **does not work** with FusionFix/DXVK, because
DXVK uses per-instance vtables. See `ISSUES.md` #5/#6 for the full story.

What works: GTA IV (RAGE engine) wraps `IDirect3DDevice9` in its own class.
The wrapper's vtable is embedded inside `GTAIV.exe`. Pattern-scan the game
for the global pointer to this wrapper:

```
A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??
```

Among multiple matches, find the candidate whose vtable lives in the
game module's address range. Patch its `vtable[42]` (EndScene) and
`vtable[16]` (Reset). EndScene fires every frame; Reset fires on device
recreation (alt+tab) so we can release D3D resources before they get
invalidated.

```cpp
// Usage in your main file:
d3d9hook::SetCallback(OnFrame);        // called every EndScene
d3d9hook::SetLostCallback(OnLost);     // called before Reset
auto mod = sdk::GetGameModule();
d3d9hook::Install(mod.base, mod.size);
```

## Player Speed Reading

The game stores a `CPedFactory*` in a global. From there:
- `factory + 0x04` -> player `CPed*`
- `ped + 0x32C` -> current `CVehicle*` (0 if on foot) [CE-specific offset]
- `vehicle + 0x80` -> 3 floats (velocity x/y/z) [CE-specific offset]

`sdk::player::Init(gameBase, gameSize)` scans the game for the factory.
Because the game has many objects that "look like" a CPedFactory, it
scores every candidate by the highest reported speed and picks the one
with the most realistic reading. **The player must be driving when Init
runs** for the scored scan to identify the right factory.

A re-scan triggers automatically if speed stays under 1 km/h for ~10s
while in vehicle (signals we locked onto a wrong factory).

After Init: `sdk::player::GetSpeed()` returns speed in km/h, or -1 if
on foot / pointer invalid.

## Rendering Architecture

`speedometer.cpp::Render()` runs from the EndScene callback:

1. Check `TestCooperativeLevel` -- skip if device lost.
2. Reset stale render states (GTA leaves pixel shaders bound, etc).
3. `D3DXSprite::Begin(D3DXSPRITE_ALPHABLEND)`.
4. Draw `Bck.png` (the dial face) with scale + position.
5. Draw `Pin.png` (the needle) with same transform + rotation by
   `(speed / maxSpeed) * 270 degrees`.
6. `D3DXSprite::End`.

If we don't reset shaders/states first, the dial renders as a black
silhouette because GTA's HDR tonemap shader is still bound. See
`ISSUES.md` #7.

## Code Conventions

- **C++17**, 32-bit only. Built with i686-w64-mingw32 (MinGW for Win32).
- **No MSVC SEH** (`__try / __except`). Use `IsBadReadPtr` for safe probing.
- All public helpers in `sdk::` or `d3d9hook::` namespaces.
- Logging via `sdk::Log(fmt, ...)` -- timestamped, flushed every line.
- Keep `speedometer.cpp` thin; put reusable logic in `gtaiv_sdk.h` / `d3d9_hook.h`.
- No external dependencies beyond what MinGW + `d3dx9_40` already provide.
- Built artifact must be `pei-i386`. Verify with `objdump -f`.
- No absolute paths in source code -- paths belong in `ENVIRONMENT.md`.

## Common Tasks (How To)

**Add a new game memory read**: extend `sdk::player` namespace in
`gtaiv_sdk.h`. Follow the safe-read pattern (`SafeReadPtr` / `SafeReadFloat`),
add candidate offsets to a static array, validate via `IsHeapPtr` + vtable
check.

**Tune visual params**: edit `Config.ini` while the game runs. No code
change needed.

**Add a new render element**: extend `Render()` in `speedometer.cpp`. Make
sure to call the render-state reset block (see `ISSUES.md` #7) before
drawing if anything new is being drawn.

**Debug a stuck speed reading**: run `build/debug_scan.exe` (must be admin)
while the game is running. It dumps every CPedFactory candidate with the
speed each velocity offset produces.

**Verify the hook fires**: read the log -- you should see `Frame N: visible=1
speed=X.X resOk=1` lines every 300 frames. If not, the hook didn't install
or wasn't on the right vtable.

## Anti-Patterns to Avoid

- Don't use `Direct3DCreate9` to make a dummy device for hook installation.
  Doesn't work with DXVK.
- Don't use `D3DXSPRITE_DONOTSAVESTATE` -- we WANT D3DX to manage state.
- Don't hook the wrapper's Present (`vtable[17]`) -- the game uses EndScene.
- Don't use `D3DPOOL_MANAGED` for textures -- DXVK prefers `D3DPOOL_DEFAULT`
  and re-acquisition via Reset hook.
- Don't add code that requires the player to be on foot at startup -- the
  factory scan needs the player to be driving for accurate offset selection.
- Don't call `GetForegroundWindow` inside the render callback -- it has been
  observed to cause crashes during DXVK transitional states.

## Build Requirements

Static-linked, no runtime DLL dependencies beyond:
- `KERNEL32`, `USER32`, `GDI32`, `D3D9` (game already loads these)
- `D3DX9_40` (loaded on demand from `SysWOW64`; if missing,
  Microsoft DirectX End-User Runtime is required)

Final binary should be ~50 KB.

## Git Hygiene

- Commit only source/docs. `*.asi`, `*.exe`, `build/`, `ENVIRONMENT.md`,
  `deploy.ps1`, and `*.log` are gitignored.
- Write commit messages in imperative present tense.
- Don't commit Windows line-ending warnings; they're fine.
