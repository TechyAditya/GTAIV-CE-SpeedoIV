/*
 * sdk/all.h - Convenience umbrella header for the GTA IV CE modding SDK
 *
 * Pulls in every module so a plugin can do `#include "sdk/all.h"` and use
 * the full API. Individual headers are also fine to include alone for
 * granular dependency control in reusable libraries.
 *
 * Modules:
 *   logging.h   - timestamped file logger with Debug-flag gating
 *   memory.h    - safe read helpers + pointer validity predicates
 *   scanner.h   - pattern scanner over a memory range
 *   game.h      - main executable discovery (base + size)
 *   ui.h        - game pause / menu state
 *   player.h    - player ped + vehicle + speed
 *   d3d9_hook.h - GTA IV RAGE D3D9 device hook (EndScene + Reset)
 *   render.h    - D3DX9 sprite helpers (texture load, state reset, transform)
 *
 * The SDK targets GTA IV Complete Edition 1.2.0.43. Patterns and offsets
 * are taken from / verified against:
 *   - FusionFix source (https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
 *   - Live probes in tools/ (probe_vehicle, probe_dynamic, probe_pause)
 */

#pragma once

#include "logging.h"
#include "memory.h"
#include "scanner.h"
#include "game.h"
#include "ui.h"
#include "player.h"
#include "d3d9_hook.h"
#include "render.h"
