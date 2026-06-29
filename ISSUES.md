# ISSUES.md

Chronological log of issues encountered and their resolutions during
SpeedoIV-CE development. Useful as a reference for what doesn't work
and why, so we don't re-tread dead ends.

---

## 1. Original SpeedoIV (2009) incompatible with Complete Edition

**Finding**: The bundled `SpeedoIV.asi` v0.3a (with the retarded_chicken
skin pack) is hard-linked against `ScriptHook.dll` v0.5.1 (Aru's C++ hook,
2009). That ScriptHook only supports GTA IV 1.0.4.0 - 1.0.7.0. The
Complete Edition is 1.2.0.43. ScriptHook is therefore non-functional.

**Fix**: Wrote a new standalone ASI plugin (SpeedoIV-CE) that does not
depend on ScriptHook. Reads vehicle speed directly from game memory.

**Files retained from upstream**:
- `Bck.png`, `Bck_orig.png` -- dial face
- `Pin.png`, `Pin_orig.png` -- needle
- `Config.ini` -- positioning/sizing parameters

---

## 2. Polluted game folder

**Finding**: User had extracted `Dependencies_x64_Release.zip` (a PE
dependency viewer tool) directly into the game folder. This dumped 67
unrelated files including 64-bit `MSVCP140.dll`, `ucrtbase.dll`,
`VCRUNTIME140.dll` etc. -- all 64-bit DLLs in a 32-bit game folder,
some of which would shadow/conflict with the legit DLLs the game needs.

**Fix**: Removed all files matching the Dependencies zip contents.
Verified against extracted archive list to ensure only foreign files
were deleted (not game files).

---

## 3. MinGW 64-bit only, no 32-bit cross-compile

**Finding**: Initial `winget install BrechtSanders.WinLibs.POSIX.UCRT`
installed only x86_64 MinGW. GTA IV is 32-bit so we need i686 output.
`g++ -m32` failed because the 32-bit runtime libraries weren't included.

**Fix**: Downloaded WinLibs i686 (13.2.0 posix-dwarf-ucrt) from GitHub
releases, extracted to a temp dir. Use that g++ explicitly.

---

## 4. MSVC SEH not supported by GCC

**Finding**: First-pass code used `__try / __except(EXCEPTION_EXECUTE_HANDLER)`
to safely probe potentially-invalid memory. GCC rejects this -- SEH
intrinsics are MSVC-only.

**Fix**: Replaced every `__try / __except` block with `IsBadReadPtr`
guards before dereferencing. See `sdk::SafeReadPtr` / `sdk::SafeReadFloat`
in `gtaiv_sdk.h`.

---

## 5. D3D9 dummy-device vtable hook does NOT work with DXVK

**Finding**: The classic approach for an external D3D9 overlay --
`Direct3DCreate9` -> `CreateDevice` on a hidden window -> patch
`vtable[42]` of the resulting device -- did not affect the game's
rendering. Hook installed, but `HookedEndScene` never fired.

**Root cause**: FusionFix's `d3d9.dll` redirects to **DXVK** (`vulkan.dll`)
which uses **per-instance vtables**. The vtable from the dummy device is
NOT shared with the game's device.

**Failed attempts**:
- Vtable hook from dummy device -> no fire
- Vtable hook from dummy device + Reset hook -> no fire
- Scanning all memory for device-shaped objects with vtable in d3d9.dll
  -> found objects with `vt[42] = 0` (some COM Vulkan helper, not a device)
- Inline JMP detour on `vt[42]` function body resolved from dummy device
  -> crash, because the function had a stack-aligning prologue
  (`8D 4C 24 04 83 E4 ??`) that broke when relocated.

**Fix**: See issue #6.

---

## 6. RAGE engine wraps the D3D9 device

**Finding**: While reading FusionFix source, learned that the game
(RAGE engine) creates its own `IDirect3DDevice9` subclass with the
vtable embedded inside `GTAIV.exe` itself. The game stores the wrapper
in a global pointer and pattern-scannable via:

```
A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??
mov eax, [g_pDevice]
push eax
mov ecx, [eax]      ; vtable
call [ecx+0xNN]     ; vtable[NN]
```

The right device's vtable lives in `GTAIV.exe` (not in `d3d9.dll`).
Once we patch THAT vtable's `[42]`, the hook fires every frame.

Confirmed by `inject_hook.exe`: patched `vt[42]` from outside the
process, counter ticked at ~88 Hz = game's framerate.

**Fix**: `d3d9hook::Install()` now scans the game for the wrapper-device
pointer and patches its in-game vtable directly. No dummy device.

---

## 7. Black/dark sprite rendering

**Finding**: After EndScene hook fired correctly, our D3DX sprite rendered
as a black silhouette instead of the colorful PNG texture.

**Root cause**: GTA IV finishes its rendering pass with a tonemap /
HDR-style pixel shader still bound. Our sprite was being processed
through that shader, producing dark output.

**Fix**: Before calling `g_sprite->Begin`, reset device state explicitly:

```cpp
dev->SetPixelShader(NULL);
dev->SetVertexShader(NULL);
dev->SetRenderState(D3DRS_LIGHTING, FALSE);
dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
dev->SetRenderState(D3DRS_ZENABLE, FALSE);
dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
// ... etc
```

After this, colors render correctly.

---

## 8. Alt+tab crash

**Finding**: Alt-tabbing out of the game caused immediate process crash.

**Root cause**: DXVK doesn't reliably return `D3DERR_DEVICELOST` from
`TestCooperativeLevel`. When the device is recreated (alt+tab in
fullscreen-borderless mode that DXVK uses), our cached `ID3DXSprite`,
`IDirect3DTexture9*` and `ID3DXFont` reference the OLD device. Drawing
with them crashes.

**Fix**:
- Also hook the wrapper's `vtable[16]` (Reset). When the game calls
  Reset (which DXVK does on focus changes), our pre-hook callback fires
  and releases all D3D resources.
- The next EndScene re-acquires them against the new device.
- See `d3d9hook::SetLostCallback` + `speedometer::OnDeviceLost`.

---

## 9. CPedFactory pattern scan picked wrong factory

**Finding**: Several memory pointers look like a valid CPedFactory
(vtable + ped pointer + vehicle pointer), but most are stale or
dummy objects. The first match we accepted always reported speed
near zero, no matter how fast you were actually driving.

**Investigation tools**:
- `debug_scan.exe` -- iterates pattern hits, dereferences each
  candidate, tries every plausible vehicle/velocity offset, prints
  the resulting "speed in km/h"
- `find_real.exe` -- locates the specific candidate at the verified
  byte offset that gives a realistic moving-speed reading

**Findings**:
- Real CPedFactory global: `0x01D3A46C` (in the game's data section)
- vehicle pointer offset on CPed: `0x32C` (CE-specific)
- velocity vector offset on CVehicle: `0x80` (CE-specific, not `0x70`
  which is the legacy offset)
- Wrong candidates were earlier in the scan order, so they won.

**Fix**:
1. Increased pattern scan limit from 200 to 10000 hits.
2. Implemented **scored factory selection**: every candidate that
   passes basic validation is given a score (= highest reported
   speed across all velocity-offset tries). We pick the candidate
   with the highest score after scanning ALL hits.
3. Periodic re-scan if speed stays under 1 km/h for ~10 seconds
   despite player being in vehicle.

**Caveat**: For the scored scan to work, the player needs to be
**driving** at the moment Init runs. Spawning stationary will lock
onto a wrong factory until the re-scan triggers.

---

## 10. F5 hijacked as quicksave

**Finding**: Using `VK_F5` (`116`) as the speedometer toggle caused
GTA IV to also trigger its built-in F5 action (quicksave/screenshot).
The toggle "didn't work" because the game's F5 binding interfered.

**Fix**: Changed default `ToggleKey` to F6 (`117`). Documented
alternative keys in `Config.ini` comment.

---

## 11. Config live-reload clobbered toggle state

**Finding**: After fixing the toggle key, pressing F6 logged
`Toggle: visible=0` then `Toggle: visible=1` rapidly because
`LoadConfig` runs every ~1 second and was setting
`g_visible = g_cfg.autostart`, undoing the user's toggle.

**Fix**: Only initialize `g_visible` on the first config load. On
subsequent reloads, keep the user's current visibility state.

---

## 12. Position math broke after refactor

**Finding**: After changing the D3DX sprite center to texture-pixel
coords for needle rotation, the dial appeared in the wrong place
(off-screen / very small).

**Root cause**: `D3DXMatrixTransformation2D` uses texture-pixel coords
for both scaling-center and rotation-center anchors. Mixing screen
coords for one and texture coords for the other broke the position.

**Fix**: Use `(0, 0)` for scaling center (scale anchored at sprite
origin) and `(texSize/2, texSize/2)` for rotation center (needle pivots
on dial center). Translation `pos` is in screen pixels and adds AFTER
scaling, so the final position math is straightforward.

---

## 13. Needle rotation direction / origin

**Finding**: After fixing position, the needle texture either didn't
move or rotated from the wrong start point.

**Root cause**: The retarded_chicken skin's `Pin.png` already shows
the needle in the "0" position (bottom-left of dial). The needle
needs to rotate clockwise from this position by `(speed / maxSpeed) *
270 degrees`, NOT from straight-up.

**Fix**: Changed needle angle formula to `ratio * 270.f` (no -135
offset).

---

## 14. Pin.png was a black silhouette in the new skin

(Resolved by issue #7.) When colors first started rendering, the
needle was visible as a colored bar -- previously it appeared as a
random horizontal line because the texture pixels weren't being
properly modulated through D3D state.

---

## 15. Iteration speed -- locked ASI file during development

**Finding**: While the game is running, the loaded ASI file is
locked by the process. Copy-replacing it fails. Original workflow
required closing the game manually between every code change.

**Fix**: `deploy.ps1` automates: kill process -> wait -> copy ASI ->
launch. One command, full iteration.

**Live-tunable params**: For visual tweaks (position, size, alpha,
colors), `Config.ini` is re-read every ~1 second. No restart needed.

---

## 16. Position math: rotation pivot in scaled vs unscaled space

**Finding**: After verifying that `D3DXMatrixTransformation2D` applies
`T(-Sc)*S*T(Sc)*T(-Rc)*R*T(Rc)*T(Mt)`, our previous code used
`rotCenter = (texW/2, texH/2)` (unscaled texel coords) but the actual
sprite was being scaled before rotation, so the rotation pivot was at
the wrong screen position. The pin appeared rotated ~120 degrees around a point
offset from the dial centre.

**Fix**: Build the matrix with
`scalingCenter = (0,0)`, `rotationCenter = (texW/2 * scaleX, texH/2 * scaleY)`
(in post-scale units), and translation
`pos = (cx - rotCenter.x, cy - rotCenter.y)` so the scaled visual
centre lands exactly at `(cx, cy)`.

Verified: dial + pin both centre correctly at the configured screen point.

---

## 17. CPedFactory pattern-scan was fundamentally unreliable

**Finding**: Scoring CPedFactory candidates by `best reported speed`
only ever locked onto the right factory if the player was already in
motion at scan time. Even then, a stale candidate at `0x02499B24`
(`vehOff 0x330 velOff 0x80`) consistently beat the real one at
`0x01D3A46C`. Stuck-low rescan never recovered. Hard-coding the CE
factory address didn't help either: the ped/vehicle offsets were also
wrong guesses.

**Fix**: Replaced the entire factory-globals approach with FusionFix's
proven method: **pattern-scan for the game's own `FindPlayerPed` and
`FindPlayerVehicle` helper functions** inside GTAIV.exe, then call
them as a normal `__cdecl` function with `id=0`. These functions
internally walk `g_PlayerInfoSlot -> g_PlayerInfoArray[slot] ->
playerInfo->m_pPed -> ped->m_pVehicle` and return the same pointer
that `GET_CAR_CHAR_IS_USING` native uses.

CE 1.2.0.43 patterns (verified against FusionFix source/comvars.ixx
and our own probe):
- `FindPlayerPed`     `8B 44 24 04 85 C0 75 18 A1`
- `FindPlayerVehicle` `8B 44 24 04 85 C0 75 15 A1 ? ? ? ? 83 F8 FF 75 04 33 C0 EB 07`

Resolved addresses on our build:
- `FindPlayerPed     @ 0x014B1C10`
- `FindPlayerVehicle @ 0x014B1C40`
- Shared global `g_currentPlayerSlot @ 0x01C16F14`
- `g_PlayerInfoArray @ 0x01D88808`

---

## 18. Real CVehicle and CPed offsets (CE 1.2.0.43)

**Finding**: Disassembly of `FindPlayerVehicle` revealed the actual
ped-to-vehicle indirection and the in-vehicle flag, contradicting
guesses based on legacy mods:

`\\\
+0x0B30   CPed -> CVehicle* (m_pMyVehicle)
+0x026C   CPed inVehicle flag byte (bit 2 = `in vehicle`)
+0x0598   CPlayerInfo -> CPed* (m_pPlayerPed)
\\\

Live probe of the resulting CVehicle struct while driving ~38 km/h
showed the velocity vector at **CVehicle + 0xF8** (NOT 0x70/0x80
that legacy 1.0.x mods use):

`\\\
+0x0F8 (10.588, 0.000, 0.000) |km/h|=38.12
\\\

**Fix**: Set `sdk::player::g_velOffset = 0xF8` as the primary CE
constant. Auto-detect fallback tries `{0xF8, 0x80, 0x70, ...}` if
the default produces zero readings on an unusual build.

---

## 19. alt+tab left speedometer stuck invisible

**Finding**: After alt+tab the device-reset hook releases all D3D
resources and sets `g_resOk = false`. `InitResources` is called
again from `Render()` -- but `Render` was gated on
`GetSpeed() >= 0`, which returns `-1` when the player is on foot
or not driving. Result: resources never re-initialised, dial stayed
black until you drove again, and toggling F6 didn't help.

**Fix**: Always call `Render()` when `g_visible` is set. Clamp
`speed = 0` when `GetSpeed()` returns negative so the dial shows
a sensible idle reading. Resource re-init runs every frame as needed.

---

## 20. Velocity offset was wrong: 0xF8 vs 0x1270

**Finding**: Setting `g_velOffset = 0xF8` (based on the static probe
that captured a 38 km/h reading at +0xF8 while we were driving ~38
km/h) produced a needle that appeared to track speed, but actually
was stuck on a stale engine-state scalar. Symptoms: needle showed
~38 while sitting in the car, ~28 mid-exit, 0 on foot, but NEVER
changed while actively driving.

**Root cause**: `vehicle+0xF8` is a (component-only-X) scalar that
correlates with engine state but does not equal velocity. The first
probe captured it by coincidence -- our filter only looked at static
triplets in the first sample and called magnitude == 38.12 km/h a
match because the user was driving 38 km/h at that exact moment.
Confirmation bias struck.

**Diagnosis**: `probe_dynamic.exe -snap` captured 3 snapshots 1s
apart while driving, then enumerated every 4-byte float triplet
inside the vehicle struct AND every sub-object pointed to by the
vehicle. The per-sample magnitude trace revealed that ONLY `+0x1270`
moved with real velocity (60.78 -> 64.75 -> 63.95 km/h). All other
"velocity-looking" offsets stayed flat across samples.

**Fix**: `g_velOffset = 0x1270`. Removed 0xF8 from the candidate
list to prevent regression. Auto-detect now has only the verified
CE offset.

**Lessons**:
- Single-snapshot probes pick up coincidental matches. Always
  capture multiple samples to verify a value is **changing** with
  the variable you care about.
- Trust per-sample variance more than absolute magnitudes.
- Sub-object scan is critical: GTAIV's CVehicle has nested
  CPhysical-derived data that's reached via internal pointers --
  velocity often lives in one of those, not in the parent struct.

---

## 21. Refactor into sdk/ + tools/ + one-time setup.ps1

**Finding**: After the velocity fix the project worked end-to-end, but
the codebase was a flat pile of files (gtaiv_sdk.h, d3d9_hook.h,
speedometer.cpp, plus 10 dev probes at the root) with no clear path
for reusing the SDK in a different mod. Setup required hand-rolling
ENVIRONMENT.md and deploy.ps1 from a SETUP.md template.

**Fix**:

1. Split `gtaiv_sdk.h` + `d3d9_hook.h` into `sdk/*.h`:
   `logging.h`, `memory.h`, `scanner.h`, `game.h`, `ui.h`,
   `player.h`, `d3d9_hook.h`, `render.h`, plus an umbrella
   `all.h`. Each header has one responsibility and a top-of-file
   comment explaining its API.
2. Moved 10 dev probes into `tools/`. `build.ps1 -Tools` rebuilds
   them all. Probe output paths are still hardcoded to
   `build\probe_*.txt` (they live at the repo root regardless of
   tool location, so still works).
3. New `sdk/render.h` extracts the D3DX9 helpers from speedometer.cpp:
   `LoadTexture` (DXVK-friendly), `Reset2DState`,
   `BuildSpriteTransform` (correct rotation pivot in scaled coords),
   `ResolveAlignment`. The render block in speedometer.cpp shrank
   from ~150 lines to ~30 of just configuration + `Begin/Draw/End`.
4. New `sdk/logging.h` adds a Debug flag with two modes:
   - `Debug=true`: truncate the log on first call, write every line
     immediately (the old behaviour).
   - `Debug=false`: do not open the log file. Info-level calls go to
     a 64-entry ring buffer. On first warn/error, lazy-open the file in
     APPEND mode, flush the ring under a `--- buffered context ---`
     header, then write the warning. Quiet by default, contextual when
     it matters, never wipes history without consent.
5. Added `setup.ps1` as the one-time bootstrap:
   - Downloads + extracts 32-bit MinGW 13.2.0 i686 if missing.
   - Auto-detects the game folder (Rockstar, Steam, common drive paths)
     or prompts once.
   - Generates `ENVIRONMENT.md` (first time only -- it's agent-managed
     after that).
   - Generates `deploy.ps1` with the discovered paths. The new
     `deploy.ps1` kills the game FIRST then calls `build.ps1`,
     fixing the file-lock race where `build.ps1` tried to overwrite
     the deployed ASI before the running game released its handle.
6. Added `README.md` as the human-facing entry point. Credits
   `retarded_chicken` for the bundled artwork (assets only -- no
   logic reused) and `FusionFix` / `ThirteenAG` for the
   pattern-scan blueprint. `NOTICE.txt` placed alongside the
   artwork in `SpeedoIV/Default/`.
7. `build.ps1` reads `GAME_DIR = "..."` from `ENVIRONMENT.md`
   and auto-installs the built ASI into `<game>\plugins\`.

**Lessons**:
- A one-file SDK is fine until you want to ship it. The split into
  per-responsibility headers makes drop-in reuse for a new ASI mod a
  matter of copying the `sdk/` folder.
- Debug-by-default logs grow without bound and clutter the game folder.
  Lazy-open + ring-buffer means we still get diagnostic value on the
  rare occasions something goes wrong, without paying the disk cost on
  every launch.
- PowerShell here-strings need bare `-Wl,--kill-at` followed by a
  `)`-closed array literal, not a backtick-continued line. Backtick
  line continuations choke when there's trailing whitespace or a
  comma immediately before a newline -- the parser produces
  `Missing argument in parameter list` with no useful line number.
