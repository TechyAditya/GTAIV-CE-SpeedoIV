/*
 * sdk/ui.h - GTA IV pause / menu state detection
 *
 * Pattern-scans GTAIV.exe for CTimer::m_UserPause, CTimer::m_CodePause and
 * CMenuManager::m_MenuActive. The same patterns FusionFix uses
 * (source/comvars.ixx:2615 and 2634). Verified live on Complete Edition
 * 1.2.0.43.
 *
 * Game is considered paused if ANY of:
 *   - user opened the pause menu (m_UserPause != 0)
 *   - engine is in a code-paused state (load, save, cutscene transition)
 *   - any blocking menu is being drawn (map, brief, stats, ...)
 *
 * Defaults to "not paused" if pattern scan failed so we never accidentally
 * hide functionality forever.
 */

#pragma once

#include "memory.h"
#include "scanner.h"
#include <cstdint>

namespace sdk {
namespace ui {

inline BYTE* g_userPause  = nullptr;
inline BYTE* g_codePause  = nullptr;
inline BYTE* g_menuActive = nullptr;
inline bool  g_ready      = false;

inline bool Init(uintptr_t gameBase, size_t gameSize) {
    if (g_ready) return true;

    /* m_UserPause / m_CodePause: paired `or al, [global]` instructions.
     *   0A 05 <UserPause> 0A 05 <CodePause> 75 38                        */
    {
        const BYTE pat[] = {0x0A,0x05,0,0,0,0,0x0A,0x05,0,0,0,0,0x75,0x38};
        uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xx????xx????xx");
        if (!hit) {
            /* Fallback without the `75 38` tail */
            const BYTE patB[] = {0x0A,0x05,0,0,0,0,0x0A,0x05};
            hit = FindPattern(gameBase, gameSize, patB, "xx????xx");
        }
        if (hit) {
            g_userPause = *(BYTE**)(hit + 2);
            g_codePause = *(BYTE**)(hit + 8);
        }
    }

    /* m_MenuActive:
     *   80 3D <global> ?? 74 4B E8 ?? ?? ?? ?? 84 C0                     */
    {
        const BYTE pat[] = {0x80,0x3D,0,0,0,0,0,0x74,0x4B,0xE8,0,0,0,0,0x84,0xC0};
        uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xx?????xxx????xx");
        if (hit) g_menuActive = *(BYTE**)(hit + 2);
    }

    g_ready = (g_userPause != nullptr || g_menuActive != nullptr);
    return g_ready;
}

inline bool IsGamePaused() {
    if (g_userPause  && IsReadable(g_userPause, 1)  && *g_userPause)  return true;
    if (g_codePause  && IsReadable(g_codePause, 1)  && *g_codePause)  return true;
    if (g_menuActive && IsReadable(g_menuActive, 1) && *g_menuActive) return true;
    return false;
}

inline bool IsUserPaused() { return g_userPause  && IsReadable(g_userPause, 1)  && *g_userPause; }
inline bool IsMenuOpen()   { return g_menuActive && IsReadable(g_menuActive, 1) && *g_menuActive; }
inline bool IsCodePaused() { return g_codePause  && IsReadable(g_codePause, 1)  && *g_codePause; }

} // namespace ui
} // namespace sdk
