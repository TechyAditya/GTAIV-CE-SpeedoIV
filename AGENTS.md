# AGENTS.md

Instructions for AI agents working on this codebase.

## Project

SpeedoIV-CE: an ASI plugin (32-bit DLL) for GTA IV Complete Edition (CE, v1.2.0.43)
that renders a speedometer overlay using D3DX9 sprites. Compatible with FusionFix
(DXVK) and other CE mod loaders.

## Architecture

```
gtaiv_sdk.h    Reusable SDK: logging, safe memory, pattern scanner,
               player/vehicle state (CPedFactory, speed reading).
d3d9_hook.h    Reusable hook: finds the game's RAGE D3D9 wrapper via
               pattern scan, patches its vtable to intercept EndScene.
speedometer.cpp  Main: config, D3D resources, render. ~300 lines.
deploy.ps1     Build + kill game + deploy ASI + relaunch.
```

Helpers (not built into the ASI; used for debugging):
- `debug_scan.cpp`   - dumps CPedFactory candidates from running game
- `device_scan.cpp`  - finds D3D9 device globals in game memory
- `live_probe.cpp`   - dumps loaded modules + device vtables
- `inject_hook.cpp`  - proves wrapper-vtable hook approach by counting EndScene calls
- `find_real.cpp`    - locates the real CPedFactory by scanning a specific code range
- `restore_hook.cpp` - restores a manually-patched vtable entry

## Deployment Flow

1. Edit code in `speedometer.cpp` / `gtaiv_sdk.h` / `d3d9_hook.h`.
2. Run `deploy.ps1`. It:
   - Builds the ASI with MinGW i686 GCC.
   - Kills GTAIV.exe if running.
   - Copies the ASI to the game's `plugins/` directory.
   - Launches the game.
3. Inspect the runtime log (see ENVIRONMENT.md for path) for diagnostics.
4. Live-tune visual params by editing `SpeedoIV/Config.ini` -- reloads every ~1s.

When making changes that affect rendering or hook installation, restart is required
(use `deploy.ps1`). When tweaking position/size/colors, edit `Config.ini` while the
game is running.

## Iteration Loop

- Read the runtime log to verify the hook installed, factory was found, and
  EndScene callback is firing each frame.
- The pattern scanner picks the candidate with the highest reported speed.
  The player must be **driving** when the scan runs for the right factory to win.
- If the speedometer is stuck at near-zero speed, the wrong factory was picked.
  A re-scan triggers after ~10s of low-speed reading.

## Code Conventions

- C++17, 32-bit. Built with i686-w64-mingw32 (MinGW for Win32).
- NO MSVC `__try / __except` (SEH not supported in MinGW GCC).
  Use `IsBadReadPtr` for safe memory probing.
- All public helpers live in the `sdk::` or `d3d9hook::` namespaces.
- Logs go through `sdk::Log()` -- timestamped, flushed every line.
- Keep `speedometer.cpp` thin; put reusable game-modding logic in `gtaiv_sdk.h`
  and reusable D3D9 hooking logic in `d3d9_hook.h`.

## Constraints

- DO NOT use SDL/Qt/any heavyweight runtime. Static-linked minimal binary.
- DO NOT add external dependencies beyond what MinGW + d3dx9_40 already provides.
- DO NOT hardcode absolute paths in code (paths belong in `ENVIRONMENT.md`,
  which is `.gitignore`d).
- Built artifact must be 32-bit (`pei-i386`). Verify with `objdump -f`.

## Hook Approach (Important)

The game is GTA IV CE with FusionFix's DXVK d3d9 wrapper. The naive approach
(create dummy device, hook vtable) does NOT work because DXVK uses per-instance
vtables. The correct approach (used here):

1. Pattern-scan GTAIV.exe for code that calls D3D9 methods via a global ptr:
   `A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??`
2. Among the resulting device-pointer candidates, find the one whose vtable
   lives inside GTAIV.exe -- that's RAGE's grcDevice wrapper, not DXVK.
3. Patch this wrapper's vtable[42] (EndScene) and vtable[16] (Reset).
4. The wrapper's EndScene gets called every frame (FusionFix doesn't
   intercept the wrapper itself).
