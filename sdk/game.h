/*
 * sdk/game.h - Game-module discovery for GTA IV CE (and other Win32 hosts)
 *
 * Resolves the primary executable's load address + size. Use the result with
 * scanner.h to scan game code/data. Works for any single-executable Win32
 * process; not GTA-specific.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace sdk {

struct GameModule {
    uintptr_t base = 0;
    size_t    size = 0;
    bool      valid = false;
};

/* Returns the main executable's module info. NULL handle => current process. */
inline GameModule GetGameModule() {
    GameModule m;
    HMODULE h = GetModuleHandleA(NULL);
    if (!h) return m;
    m.base = (uintptr_t)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(m.base + dos->e_lfanew);
    m.size = nt->OptionalHeader.SizeOfImage;
    m.valid = true;
    return m;
}

} // namespace sdk
