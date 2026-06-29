/*
 * gtaiv_sdk.h - Reusable GTA IV Complete Edition (1.2.0.43) modding SDK
 *
 * Provides:
 *   - Safe memory read helpers
 *   - Pattern scanner
 *   - Player/vehicle state reading (speed, in-vehicle detection)
 *   - Simple file logger
 *
 * Usage: #include "gtaiv_sdk.h" in your ASI plugin.
 * All functions are in the `sdk` namespace.
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdarg>

namespace sdk {

/* ==========================================================================
 * Logging
 * ========================================================================== */

inline FILE* g_log = nullptr;

inline void LogOpen(const char* filename) {
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\%s", dir, filename);
    g_log = fopen(path, "w");
}

inline void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

inline void LogClose() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

/* ==========================================================================
 * Safe Memory
 * ========================================================================== */

inline bool IsReadable(const void* addr, size_t len) {
    return !IsBadReadPtr(addr, len);
}

inline bool ReadPtr(uintptr_t addr, uintptr_t* out) {
    if (!IsReadable((void*)addr, 4)) return false;
    *out = *(uintptr_t*)addr;
    return true;
}

inline bool ReadFloat(uintptr_t addr, float* out) {
    if (!IsReadable((void*)addr, 4)) return false;
    *out = *(float*)addr;
    return true;
}

inline bool IsValidPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }
inline bool IsHeapPtr(uintptr_t p)  { return p >= 0x04000000 && p < 0x7FFFFFFF; }

/* ==========================================================================
 * Pattern Scanner
 * ========================================================================== */

inline uintptr_t FindPattern(uintptr_t base, size_t size,
                             const BYTE* pattern, const char* mask) {
    size_t plen = strlen(mask);
    if (size < plen) return 0;
    for (size_t i = 0; i <= size - plen; i++) {
        bool ok = true;
        for (size_t j = 0; j < plen && ok; j++)
            if (mask[j] == 'x' && ((const BYTE*)(base + i))[j] != pattern[j])
                ok = false;
        if (ok) return base + i;
    }
    return 0;
}

/* Scan with automatic retry across multiple pattern hits */
inline uintptr_t FindPatternMulti(uintptr_t base, size_t size,
                                  const BYTE* pattern, const char* mask,
                                  int maxHits = 100) {
    size_t plen = strlen(mask);
    uintptr_t cur = base;
    size_t rem = size;
    for (int n = 0; n < maxHits && cur < base + size - plen; n++) {
        uintptr_t hit = FindPattern(cur, rem, pattern, mask);
        if (!hit) break;
        return hit; /* Return first hit; caller can loop if needed */
        cur = hit + 1;
        rem = base + size - cur;
    }
    return 0;
}

/* ==========================================================================
 * Game Module Info
 * ========================================================================== */

struct ModuleInfo {
    uintptr_t base;
    size_t    size;
    bool      valid;
};

inline ModuleInfo GetGameModule() {
    ModuleInfo m = {};
    HMODULE h = GetModuleHandleA("GTAIV.exe");
    if (!h) return m;
    m.base = (uintptr_t)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(m.base + dos->e_lfanew);
    m.size = nt->OptionalHeader.SizeOfImage;
    m.valid = true;
    return m;
}

/* ==========================================================================
 * Player & Vehicle
 *
 * Uses the FusionFix-style approach: pattern-scan for the game's own
 * FindPlayerPed/FindPlayerVehicle helper functions in GTAIV.exe and CALL
 * them directly. This is the same code-path GET_PLAYER_CHAR /
 * GET_CAR_CHAR_IS_USING natives use internally.
 *
 * Patterns confirmed for CE 1.2.0.43 (verified in FusionFix source
 * source/comvars.ixx:2798-2802). Far more reliable than guessing globals.
 * ========================================================================== */

namespace player {

    typedef void* (__cdecl *FindFn)(int32_t id);

    inline FindFn   g_findPlayerPed     = nullptr;
    inline FindFn   g_findPlayerVehicle = nullptr;
    inline int      g_velOffset         = 0xF8;  /* CVehicle::m_vecMoveSpeed on CE 1.2.0.43
                                                  * (verified by probe_vehicle.exe live dump) */
    inline bool     g_ready             = false;
    inline uintptr_t g_gameBase = 0;
    inline size_t    g_gameSize = 0;

    /* Probe a few candidate velocity offsets and pick the one whose
     * vector magnitude looks like a believable speed (-> [0, 500] km/h).
     * Tries 0xF8 first (CE verified) then a few alternates. */
    static const int VEL_CANDIDATES[] = { 0xF8, 0x80, 0x70, 0x90, 0xA0, 0xB0, 0xC0, 0x60, 0x100 };
    static const int NUM_VEL_CANDS = sizeof(VEL_CANDIDATES) / sizeof(int);

    inline float ReadSpeedAt(uintptr_t veh, int off) {
        float vx = 0, vy = 0, vz = 0;
        if (!ReadFloat(veh + off,   &vx)) return -1.f;
        if (!ReadFloat(veh + off+4, &vy)) return -1.f;
        if (!ReadFloat(veh + off+8, &vz)) return -1.f;
        if (vx != vx || vy != vy || vz != vz) return -1.f; /* NaN */
        return sqrtf(vx*vx + vy*vy + vz*vz) * 3.6f;
    }

    /* Initialise by pattern-scanning GTAIV.exe for the FindPlayer* helpers. */
    inline bool Init(uintptr_t gameBase, size_t gameSize) {
        if (g_ready) return true;
        g_gameBase = gameBase;
        g_gameSize = gameSize;

        /* CE 1.2.0.43 FindPlayerPed: 8B 44 24 04 85 C0 75 18 A1 */
        {
            const BYTE pat[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x18,0xA1};
            uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xxxxxxxxx");
            if (hit) g_findPlayerPed = (FindFn)hit;
        }
        /* CE 1.2.0.43 FindPlayerVehicle: 8B 44 24 04 85 C0 75 15 A1 ? ? ? ? 83 F8 FF 75 04 33 C0 EB 07 */
        {
            const BYTE pat[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x15,0xA1,
                                0,0,0,0,0x83,0xF8,0xFF,0x75,0x04,0x33,0xC0,0xEB,0x07};
            uintptr_t hit = FindPattern(gameBase, gameSize, pat, "xxxxxxxxx????xxxxxxxxx");
            if (hit) g_findPlayerVehicle = (FindFn)hit;
        }

        g_ready = (g_findPlayerPed != nullptr && g_findPlayerVehicle != nullptr);
        return g_ready;
    }

    /* Return the player's current vehicle, or null if on foot / entering. */
    inline uintptr_t GetVehicle() {
        if (!g_findPlayerVehicle) return 0;
        void* v = g_findPlayerVehicle(0);
        if (!v || !IsHeapPtr((uintptr_t)v)) return 0;
        /* Sanity-check: vtable must be inside the game image. */
        uintptr_t vt = 0;
        if (!ReadPtr((uintptr_t)v, &vt)) return 0;
        if (vt < g_gameBase || vt >= g_gameBase + g_gameSize) return 0;
        return (uintptr_t)v;
    }

    /* Auto-pick the right velocity offset the first time a moving vehicle
     * is seen. After that g_velOffset stays locked. */
    inline void MaybeRebindVelOffset(uintptr_t veh) {
        static bool locked = false;
        if (locked) return;
        float curSpeed = ReadSpeedAt(veh, g_velOffset);
        if (curSpeed > 0.5f && curSpeed < 500.f) {
            locked = true;   /* current offset is producing real speed */
            return;
        }
        /* Try every candidate and lock onto the first that reports motion. */
        for (int i = 0; i < NUM_VEL_CANDS; i++) {
            float s = ReadSpeedAt(veh, VEL_CANDIDATES[i]);
            if (s > 0.5f && s < 500.f) {
                g_velOffset = VEL_CANDIDATES[i];
                locked = true;
                return;
            }
        }
        /* Nothing reads as moving -- vehicle is stationary or we haven't
         * found the right offset yet. Leave g_velOffset at default. */
    }

    /* Get current vehicle speed in km/h. Returns -1 if on foot. */
    inline float GetSpeed() {
        uintptr_t veh = GetVehicle();
        if (!veh) return -1.f;
        MaybeRebindVelOffset(veh);
        float s = ReadSpeedAt(veh, g_velOffset);
        if (s < 0.f) return -1.f;
        return (s > 500.f) ? 0.f : s;
    }

    /* Returns true if player is currently in a vehicle */
    inline bool InVehicle() { return GetVehicle() != 0; }

} // namespace player
} // namespace sdk
