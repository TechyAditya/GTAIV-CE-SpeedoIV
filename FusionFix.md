# FusionFix.md

Notes on FusionFix's architecture and how SpeedoIV-CE interoperates with it.

Source: https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix

## What FusionFix Is

FusionFix is a community fix-pack for GTA IV Complete Edition (1.2.0.43)
and EFLC. It ships as **two cooperating modules**:

1. **`d3d9.dll`** -- a tiny **proxy DLL** that the game loads instead of the
   real system `d3d9.dll`. It just forwards every D3D9 export to either
   the real system `d3d9.dll` OR DXVK's `vulkan.dll`-backed `d3d9.dll`.
2. **`plugins/GTAIV.EFLC.FusionFix.asi`** -- the actual fix code, loaded by
   Ultimate ASI Loader. This is where all the patching happens.

The proxy is a "thin trampoline": every `Direct3DCreate9` / `Direct3DCreate9Ex`
call just `jmp`s into the real DLL's export. **It does NOT wrap the
IDirect3DDevice9 vtable.**

```
source/d3d9/d3d9.cpp    -- proxy DllMain, loads vulkan.dll if API=1
source/d3d9/d3d9.def    -- export forwards
```

## How FusionFix Hooks the Game

FusionFix does **not** use D3D9 vtable interception. Instead, the `.asi`
plugin uses **pattern scanning** to find specific instruction sequences
in `GTAIV.exe`, then patches the **game's own call-sites** with mid-hooks
(via `safetyhook` / `injector::MakeInline`).

Examples from the source:

| Pattern | What it finds | What gets hooked |
|---|---|---|
| `A1 ? ? ? ? 50 8B 08 FF 51 40` | game's call to `Reset` | inline hook on Reset call |
| `A1 ? ? ? ? 50 8B 08 FF 91 ? ? ? ?` | game's call to `EndScene` via `vt[42]` | `FusionFix::onEndScene` event fires |
| `F3 0F 10 44 24 ? 6A FF 6A FF 50 ...` | pause-menu text drawing | string replacement (ExtraInfo) |

The first `A1 XX XX XX XX` byte is `mov eax, [g_pDevice]` and `XX XX XX XX`
is the global pointer to the game's wrapper `IDirect3DDevice9*` (`rage::grcDevice`).

## Event System

Inside the `.asi`, FusionFix exposes an internal C++20 event API:

```cpp
// source/common.ixx
class FusionFix {
    static Event<>& onInitEvent();
    static Event<>& onInitEventAsync();
    static Event<>& onGameProcessEvent();   // per-frame game logic
    static Event<>& onMenuDrawingEvent();
    static Event<>& onActivateApp();
    static Event<>& onBeforeReset();
    static Event<>& onEndScene();           // post-EndScene rendering
    // ...
};
```

These events are **internal-only**. They are NOT exported from `d3d9.dll`
or from the `.asi`, so external plugins (like SpeedoIV-CE) cannot subscribe
to them via `GetProcAddress`.

The `D3D9` namespace events (`FusionFix::D3D9::onEndScene(IDirect3DDevice9*)`)
exist in source but are marked `[[deprecated]]` and the entire `FusionDxHook`
subsystem is disabled in the current build (`#pragma message ("FusionDxHook
is disabled")` in `source/fusiondxhook.ixx`). FusionFix's own draws use
game-engine hooks, not D3D9 overlays.

## What This Means for External Plugins

Because FusionFix is a pure proxy at the D3D9 layer and does NOT wrap the
device, external plugins can hook D3D9 normally -- with one important
caveat: you must hook the **game's wrapper device** (RAGE's `grcDevice`),
not the DXVK device created by `Direct3DCreate9`.

The game (RAGE engine) creates its own `IDirect3DDevice9` subclass with
the vtable embedded in `GTAIV.exe`. Find it via the same pattern FusionFix
uses (`A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??`), filter for vtables that live
**inside the game module's address range**, and patch `vtable[42]`.

DXVK uses per-instance vtables, so the classic "create a dummy device and
patch its vtable" trick will not affect the game's draws -- it patches a
different vtable.

## Coexistence with FusionFix

SpeedoIV-CE installs its `EndScene` hook on the **RAGE wrapper** vtable.
FusionFix patches the game's **call site** that calls EndScene through
that vtable. The two hooks are in different places:

- FusionFix: patches the CALL instruction in game code at `+0x250E9` (or similar)
- SpeedoIV-CE: patches `vtable[42]` of the wrapper device

Both can coexist. FusionFix's call site still dispatches through the patched
vtable, so SpeedoIV-CE's hook fires too.

## Files of Interest in the FusionFix Repo

| Path | Why it matters |
|---|---|
| `source/d3d9/d3d9.cpp`        | The proxy DllMain (DXVK loader logic) |
| `source/d3d9/d3d9.def`        | Export forwards |
| `source/dllmain.cpp`          | Pattern-scan entry; installs game-code hooks |
| `source/common.ixx`           | Event system + utilities |
| `source/comvars.ixx`          | Globals (game pointers found via patterns) |
| `source/extrainfo.ixx`        | Example of how it hooks game text rendering |
| `source/fusiondxhook.ixx`     | (disabled) D3D9 vtable hook attempt |
| `source/turnindicators.ixx`   | CE-specific vehicle offsets used here |

## CE-Specific Offsets Discovered

From `source/turnindicators.ixx` and other CE-tagged code:

- `CVehicle::pDriverOffset` = `0xF50` (CE) / `0xFA0` (legacy)
- `CVehicle::steerAngleOffset` = `0x1088` (CE) / `0x10D8` (legacy)
- `CVehicle::flagsOffset` = `0xF16` (CE) / `0xF66` (legacy)

SpeedoIV-CE additionally verified via live probes (see `tools/`):

- **`FindPlayerPed` @ `0x014B1C10`** -- entry point of the game's
  `void* (__cdecl)(int id)` helper. Same pattern FusionFix uses
  (`source/comvars.ixx:2798`). Returns `playerInfo->m_pPed` when
  called with id=0.
- **`FindPlayerVehicle` @ `0x014B1C40`** -- the corresponding vehicle
  helper. Returns `ped->m_pMyVehicle` if `inVehicle` flag set.
- **`CPlayerInfo + 0x598` -> `CPed*`**, **`CPed + 0xB30` -> `CVehicle*`**,
  **`CPed + 0x26C` bit 2** = inVehicle flag (from disassembling the
  helpers above).
- **`CVehicle + 0x1270` -> vec3 m/s** velocity vector. This was the
  one offset that actually moved with real driving (60.78 -> 64.75
  -> 63.95 km/h across three 1s-apart snapshots). Earlier guesses
  of `0x70 / 0x80 / 0xF8` were stationary scalars that coincidentally
  matched once. See `ISSUES.md` #20.
- **`CTimer::m_UserPause` @ `0x01D53590`**, **`m_CodePause` @ `0x01D53591`**
  -- byte flags; either non-zero means the game world is frozen.
- **`CMenuManager::m_MenuActive` @ `0x01D409F6`** -- byte flag for any
  blocking menu (pause, map, brief, stats, etc.).

The factory-globals approach (`CPedFactory* -> playerPed -> vehicle`)
from the very first iteration is gone -- it picks the wrong factory
when the player is stationary. The `FindPlayerVehicle` function-call
approach is the right one. See `ISSUES.md` #17.

## Build / Runtime Info

- FusionFix builds with **MSVC C++20 modules** (`.ixx`). The build is
  non-trivial to reproduce. SpeedoIV-CE uses GCC and doesn't link against
  FusionFix in any way -- it just coexists at runtime.
- Required mode for SpeedoIV-CE to work: `d3d9.cfg` has `API = 1` (DXVK on).
  The hook works the same on `API = 0` (native D3D9) but DXVK is the
  recommended FusionFix config.
