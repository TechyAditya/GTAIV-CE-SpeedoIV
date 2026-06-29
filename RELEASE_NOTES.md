# SpeedoIV-CE v1.0.0

First public release.

## Highlights

- Live vehicle speed readout, routed through the game's own
  ``FindPlayerVehicle`` helper -- the same code path
  ``GET_CAR_CHAR_IS_USING`` uses.
- Auto-hides when on foot, in the pause menu, on the map, during
  briefings, and in cutscene transitions.
- Live-tunable position, size, alpha, sweep arc, max speed, and
  toggle key via ``Config.ini``. Changes apply within ~1 second --
  no restart.
- Toggle hotkey defaults to F6 (F5 is reserved by GTA's quicksave).
- Bundled dial + needle artwork from the original 2009 SpeedoIV by
  retarded_chicken (assets only -- none of the original code is
  reused).
- Compatible with FusionFix + DXVK out of the box.

## Install

Extract the zip into your GTA IV install folder (the folder
containing ``GTAIV.exe``). The bundled ``README.txt`` walks through
the layout in detail.

## Requirements

- **GTA IV: The Complete Edition v1.2.0.43** (Rockstar Launcher or
  Steam). Older 1.0.x versions are not supported.
- **Ultimate ASI Loader** as ``dinput8.dll`` in the game folder.
- **FusionFix 5.x** strongly recommended (provides the DXVK
  ``d3d9.dll`` bridge that SpeedoIV-CE expects).

## Credits

- Bundled artwork (c) **retarded_chicken**, 2009.
- Pattern-scan blueprint derived from
  [**FusionFix**](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
  by **ThirteenAG**, GPL-3.0.
- DXVK by Philip Rebohle.

SpeedoIV-CE is released under GPL-3.0. See ``LICENSE`` in the source
repository.
