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
 * Resolves CPedFactory -> PlayerPed -> Vehicle -> Velocity at runtime
 * via pattern scanning. Call Init() once, then use GetSpeed().
 * ========================================================================== */

namespace player {

    inline uintptr_t g_pedFactory = 0;
    inline int       g_vehOffset  = 0;
    inline int       g_velOffset  = 0;
    inline bool      g_ready      = false;

    static const int PED_FACTORY_PLAYER_OFF = 0x4;
    static const int VEH_OFFSETS[]  = { 0x32C, 0x330, 0x328, 0x33C, 0x320 };
    static const int VEL_OFFSETS[]  = { 0x70,  0x80,  0x90,  0x60 };
    static const int NUM_VEH = sizeof(VEH_OFFSETS) / sizeof(int);
    static const int NUM_VEL = sizeof(VEL_OFFSETS) / sizeof(int);

    /* Validate a candidate CPedFactory global address.
     * Returns the best speed reading found (or -1 if invalid).
     * Side-effect: sets g_pedFactory/g_vehOffset/g_velOffset to BEST result. */
    inline float TryFactoryStrictScored(uintptr_t ptrAddr, uintptr_t gameBase, size_t gameSize) {
        if (!IsValidPtr(ptrAddr)) return -1;
        uintptr_t fp = 0;
        if (!ReadPtr(ptrAddr, &fp) || !IsHeapPtr(fp)) return -1;
        uintptr_t vt = 0;
        if (!ReadPtr(fp, &vt) || !IsValidPtr(vt)) return -1;
        uintptr_t ped = 0;
        if (!ReadPtr(fp + PED_FACTORY_PLAYER_OFF, &ped) || !IsHeapPtr(ped)) return -1;
        uintptr_t pedVt = 0;
        if (!ReadPtr(ped, &pedVt) || !IsValidPtr(pedVt)) return -1;
        if (pedVt < gameBase || pedVt >= gameBase + gameSize) return -1;

        float bestSpeed = -1;
        int bestVehOff = 0, bestVelOff = 0;
        for (int v = 0; v < NUM_VEH; v++) {
            uintptr_t veh = 0;
            if (!ReadPtr(ped + VEH_OFFSETS[v], &veh)) continue;
            if (veh == 0 || !IsHeapPtr(veh)) continue;
            uintptr_t vehVt = 0;
            if (!ReadPtr(veh, &vehVt) || !IsValidPtr(vehVt)) continue;
            if (vehVt < gameBase || vehVt >= gameBase + gameSize) continue;

            for (int vi = 0; vi < NUM_VEL; vi++) {
                float vx, vy, vz;
                uintptr_t vb = veh + VEL_OFFSETS[vi];
                if (ReadFloat(vb, &vx) && ReadFloat(vb+4, &vy) && ReadFloat(vb+8, &vz)) {
                    float s = sqrtf(vx*vx + vy*vy + vz*vz) * 3.6f;
                    if (s > 0.1f && s <= 500.0f && s > bestSpeed) {
                        bestSpeed = s;
                        bestVehOff = VEH_OFFSETS[v];
                        bestVelOff = VEL_OFFSETS[vi];
                    }
                }
            }
        }
        if (bestSpeed > 0) {
            g_pedFactory = ptrAddr;
            g_vehOffset = bestVehOff;
            g_velOffset = bestVelOff;
            g_ready = true;
        }
        return bestSpeed;
    }

    /* Validate a candidate CPedFactory global address.
     * We REQUIRE the vehicle's vtable to be inside GTAIV.exe (real RAGE vehicle)
     * AND a non-zero velocity reading to filter out stale/dummy peds. */
    inline bool TryFactoryStrict(uintptr_t ptrAddr, uintptr_t gameBase, size_t gameSize) {
        return TryFactoryStrictScored(ptrAddr, gameBase, gameSize) > 0;
    }

    /* Validate a candidate CPedFactory global address.
     * Accepts any candidate where ped is in heap AND has vehicle. */
    inline bool TryFactory(uintptr_t ptrAddr) {
        if (!IsValidPtr(ptrAddr)) return false;
        uintptr_t fp = 0;
        if (!ReadPtr(ptrAddr, &fp) || !IsHeapPtr(fp)) return false;
        uintptr_t vt = 0;
        if (!ReadPtr(fp, &vt) || !IsValidPtr(vt)) return false;
        uintptr_t ped = 0;
        if (!ReadPtr(fp + PED_FACTORY_PLAYER_OFF, &ped) || !IsHeapPtr(ped)) return false;
        uintptr_t pedVt = 0;
        if (!ReadPtr(ped, &pedVt) || !IsValidPtr(pedVt)) return false;

        for (int v = 0; v < NUM_VEH; v++) {
            uintptr_t veh = 0;
            if (!ReadPtr(ped + VEH_OFFSETS[v], &veh)) continue;
            if (veh == 0) continue;
            if (!IsHeapPtr(veh)) continue;
            uintptr_t vehVt = 0;
            if (!ReadPtr(veh, &vehVt) || !IsValidPtr(vehVt)) continue;

            for (int vi = 0; vi < NUM_VEL; vi++) {
                float vx, vy, vz;
                uintptr_t vb = veh + VEL_OFFSETS[vi];
                if (ReadFloat(vb, &vx) && ReadFloat(vb+4, &vy) && ReadFloat(vb+8, &vz)) {
                    float s = sqrtf(vx*vx + vy*vy + vz*vz) * 3.6f;
                    if (s >= 0.0f && s <= 500.0f && (vx != 0.0f || vy != 0.0f || vz != 0.0f)) {
                        g_pedFactory = ptrAddr; g_vehOffset = VEH_OFFSETS[v];
                        g_velOffset = VEL_OFFSETS[vi]; g_ready = true;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /* Same but also accepts on-foot state if no in-vehicle match is found.
     * Use as a fallback after the strict TryFactory fails. */
    inline bool TryFactoryFallback(uintptr_t ptrAddr) {
        if (!IsValidPtr(ptrAddr)) return false;
        uintptr_t fp = 0;
        if (!ReadPtr(ptrAddr, &fp) || !IsHeapPtr(fp)) return false;
        uintptr_t vt = 0;
        if (!ReadPtr(fp, &vt) || !IsValidPtr(vt)) return false;
        uintptr_t ped = 0;
        if (!ReadPtr(fp + PED_FACTORY_PLAYER_OFF, &ped) || !IsHeapPtr(ped)) return false;
        uintptr_t pedVt = 0;
        if (!ReadPtr(ped, &pedVt) || !IsValidPtr(pedVt)) return false;
        for (int v = 0; v < NUM_VEH; v++) {
            uintptr_t veh = 0;
            if (!ReadPtr(ped + VEH_OFFSETS[v], &veh)) continue;
            if (veh == 0) {
                g_pedFactory = ptrAddr; g_vehOffset = VEH_OFFSETS[v];
                g_velOffset = 0x80; g_ready = true; return true;
            }
        }
        return false;
    }

    /* Scan game memory for CPedFactory. Scores ALL candidates and picks
     * the one with the highest reported speed (= real moving player). */
    inline bool Init(uintptr_t gameBase, size_t gameSize) {
        if (g_ready) return true;

        struct PatInfo { BYTE pat[9]; char mask[10]; int addrOff; };
        PatInfo patterns[] = {
            { {0xA1,0,0,0,0,0x85,0xC0,0x74,0}, "x????xxx", 1 },
            { {0x8B,0x0D,0,0,0,0,0x8B,0x41,0x04}, "xx????xxx", 2 },
            { {0x8B,0x35,0,0,0,0,0x85,0xF6,0x74}, "xx????xxx", 2 },
        };

        /* Pass 1: SCORED STRICT - find the candidate with the highest speed */
        uintptr_t bestFactory = 0;
        int bestVeh = 0, bestVel = 0;
        float bestScore = 0.0f;

        for (auto& p : patterns) {
            uintptr_t cur = gameBase;
            size_t rem = gameSize;
            for (int n = 0; n < 10000 && cur < gameBase + gameSize - 9; n++) {
                uintptr_t hit = FindPattern(cur, rem, p.pat, p.mask);
                if (!hit) break;
                uintptr_t candidate = *(uintptr_t*)(hit + p.addrOff);
                /* Test factory without committing */
                float s = TryFactoryStrictScored(candidate, gameBase, gameSize);
                if (s > bestScore) {
                    bestScore = s;
                    bestFactory = g_pedFactory;
                    bestVeh = g_vehOffset;
                    bestVel = g_velOffset;
                    /* Reset ready flag - we'll commit best at end */
                    g_ready = false;
                }
                cur = hit + 1;
                rem = gameBase + gameSize - cur;
            }
        }

        if (bestFactory != 0) {
            g_pedFactory = bestFactory;
            g_vehOffset = bestVeh;
            g_velOffset = bestVel;
            g_ready = true;
            return true;
        }

        /* Pass 2: non-zero velocity (less strict, any heap factory) */
        for (auto& p : patterns) {
            uintptr_t cur = gameBase;
            size_t rem = gameSize;
            for (int n = 0; n < 10000 && cur < gameBase + gameSize - 9; n++) {
                uintptr_t hit = FindPattern(cur, rem, p.pat, p.mask);
                if (!hit) break;
                uintptr_t candidate = *(uintptr_t*)(hit + p.addrOff);
                if (TryFactory(candidate)) return true;
                cur = hit + 1;
                rem = gameBase + gameSize - cur;
            }
        }

        /* Pass 3: fallback - on-foot */
        for (auto& p : patterns) {
            uintptr_t cur = gameBase;
            size_t rem = gameSize;
            for (int n = 0; n < 10000 && cur < gameBase + gameSize - 9; n++) {
                uintptr_t hit = FindPattern(cur, rem, p.pat, p.mask);
                if (!hit) break;
                uintptr_t candidate = *(uintptr_t*)(hit + p.addrOff);
                if (TryFactoryFallback(candidate)) return true;
                cur = hit + 1;
                rem = gameBase + gameSize - cur;
            }
        }
        return false;
    }

    /* Get current vehicle speed in km/h. Returns -1 if on foot or error. */
    inline float GetSpeed() {
        if (!g_ready) return -1.0f;
        uintptr_t fp = 0, ped = 0, veh = 0;
        if (!ReadPtr(g_pedFactory, &fp) || !IsHeapPtr(fp)) return -1.0f;
        if (!ReadPtr(fp + PED_FACTORY_PLAYER_OFF, &ped) || !IsHeapPtr(ped)) return -1.0f;
        if (!ReadPtr(ped + g_vehOffset, &veh)) return -1.0f;
        if (veh == 0 || !IsHeapPtr(veh)) return -1.0f;

        float vx, vy, vz;
        uintptr_t vb = veh + g_velOffset;
        if (!ReadFloat(vb, &vx) || !ReadFloat(vb+4, &vy) || !ReadFloat(vb+8, &vz))
            return -1.0f;
        float s = sqrtf(vx*vx + vy*vy + vz*vz) * 3.6f;
        return (s >= 0.0f && s <= 500.0f) ? s : 0.0f;
    }

    /* Returns true if player is currently in a vehicle */
    inline bool InVehicle() { return GetSpeed() >= 0.0f; }

} // namespace player
} // namespace sdk
