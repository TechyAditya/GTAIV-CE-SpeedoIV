/*
 * sdk/player.h - Player ped / vehicle / speed access for GTA IV CE
 *
 * Pattern-scans GTAIV.exe for the game's own FindPlayerPed and
 * FindPlayerVehicle helpers (the same code path GET_PLAYER_CHAR /
 * GET_CAR_CHAR_IS_USING natives use). Calls them as plain __cdecl
 * functions to return the live pointers.
 *
 * Velocity is read at CVehicle + 0x1270 on CE 1.2.0.43 (verified by
 * the dynamic probe tool -- see tools/probe_dynamic.cpp).
 *
 * Patterns originally from FusionFix source/comvars.ixx:2798-2802.
 */

#pragma once

#include "memory.h"
#include "scanner.h"
#include <cstdint>
#include <cmath>

namespace sdk {
namespace player {

typedef void* (__cdecl *FindFn)(int32_t id);

inline FindFn   g_findPlayerPed     = nullptr;
inline FindFn   g_findPlayerVehicle = nullptr;
inline int      g_velOffset         = 0x1270;
inline bool     g_ready             = false;
inline uintptr_t g_gameBase = 0;
inline size_t    g_gameSize = 0;

static const int VEL_CANDIDATES[]  = { 0x1270 };
static const int NUM_VEL_CANDS = sizeof(VEL_CANDIDATES) / sizeof(int);

inline float ReadSpeedAt(uintptr_t veh, int off) {
    float vx = 0, vy = 0, vz = 0;
    if (!ReadFloat(veh + off,   &vx)) return -1.f;
    if (!ReadFloat(veh + off+4, &vy)) return -1.f;
    if (!ReadFloat(veh + off+8, &vz)) return -1.f;
    if (vx != vx || vy != vy || vz != vz) return -1.f;
    return sqrtf(vx*vx + vy*vy + vz*vz) * 3.6f;
}

inline bool Init(uintptr_t gameBase, size_t gameSize) {
    if (g_ready) return true;
    g_gameBase = gameBase;
    g_gameSize = gameSize;

    /* FindPlayerPed: 8B 44 24 04 85 C0 75 18 A1 */
    {
        const BYTE pat[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x18,0xA1};
        uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xxxxxxxxx");
        if (hit) g_findPlayerPed = (FindFn)hit;
    }
    /* FindPlayerVehicle:
     *   8B 44 24 04 85 C0 75 15 A1 ? ? ? ? 83 F8 FF 75 04 33 C0 EB 07     */
    {
        const BYTE pat[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x15,0xA1,
                            0,0,0,0,0x83,0xF8,0xFF,0x75,0x04,0x33,0xC0,0xEB,0x07};
        uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xxxxxxxxx????xxxxxxxxx");
        if (hit) g_findPlayerVehicle = (FindFn)hit;
    }

    g_ready = (g_findPlayerPed != nullptr && g_findPlayerVehicle != nullptr);
    return g_ready;
}

/* Return the player's current vehicle, or 0 if on foot / mid-enter. */
inline uintptr_t GetVehicle() {
    if (!g_findPlayerVehicle) return 0;
    void* v = g_findPlayerVehicle(0);
    if (!v || !IsHeapPtr((uintptr_t)v)) return 0;
    uintptr_t vt = 0;
    if (!ReadPtr((uintptr_t)v, &vt)) return 0;
    if (vt < g_gameBase || vt >= g_gameBase + g_gameSize) return 0;
    return (uintptr_t)v;
}

inline uintptr_t GetPed() {
    if (!g_findPlayerPed) return 0;
    void* p = g_findPlayerPed(0);
    if (!p || !IsHeapPtr((uintptr_t)p)) return 0;
    return (uintptr_t)p;
}

inline void MaybeRebindVelOffset(uintptr_t veh) {
    static bool locked = false;
    if (locked) return;
    float curSpeed = ReadSpeedAt(veh, g_velOffset);
    if (curSpeed > 0.5f && curSpeed < 500.f) { locked = true; return; }
    for (int i = 0; i < NUM_VEL_CANDS; i++) {
        float s = ReadSpeedAt(veh, VEL_CANDIDATES[i]);
        if (s > 0.5f && s < 500.f) {
            g_velOffset = VEL_CANDIDATES[i];
            locked = true;
            return;
        }
    }
}

/* Speed in km/h. Returns -1 if on foot. */
inline float GetSpeed() {
    uintptr_t veh = GetVehicle();
    if (!veh) return -1.f;
    MaybeRebindVelOffset(veh);
    float s = ReadSpeedAt(veh, g_velOffset);
    if (s < 0.f) return -1.f;
    return (s > 500.f) ? 0.f : s;
}

inline bool InVehicle() { return GetVehicle() != 0; }

} // namespace player
} // namespace sdk
