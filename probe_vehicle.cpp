/* probe_vehicle.cpp - safer: reads the globals FindPlayerPed/FindPlayerVehicle
 * use, walks the player-info struct to find the ped, then dumps everything
 * around the vehicle pointer offset to identify velocity. No code injection. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>

static HANDLE g_hp = 0;
static uintptr_t g_base = 0;
static size_t g_size = 0;
static FILE* g_f = 0;

static bool R(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_hp, (LPCVOID)a, out, n, &got) && got == n;
}
static uintptr_t Rp(uintptr_t a) { uintptr_t v=0; R(a,&v,4); return v; }
static bool inImage(uintptr_t p) { return p >= g_base && p < g_base + g_size; }
static bool isHeap(uintptr_t p)  { return p >= 0x04000000 && p < 0x7FFFFFFF; }

int main() {
    g_f = fopen("E:\\Code\\gta4\\speedometer\\build\\probe_vehicle.txt", "w");

    /* Locate GTAIV.exe */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) do {
        if (_stricmp(pe.szExeFile, "GTAIV.exe") == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { fprintf(g_f, "GTAIV not running\n"); fclose(g_f); return 1; }

    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(ms, &me)) do {
        if (_stricmp(me.szModule, "GTAIV.exe") == 0) {
            g_base = (uintptr_t)me.modBaseAddr; g_size = me.modBaseSize; break;
        }
    } while (Module32Next(ms, &me));
    CloseHandle(ms);
    fprintf(g_f, "GTAIV base=0x%08X size=0x%X\n", (unsigned)g_base, (unsigned)g_size);

    g_hp = OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_hp) { fprintf(g_f, "OpenProcess err=%lu\n", GetLastError()); fclose(g_f); return 1; }

    /* Read entire game image */
    BYTE* img = (BYTE*)malloc(g_size);
    for (size_t off = 0; off < g_size; off += 4096) {
        size_t chunk = (off + 4096 <= g_size) ? 4096 : g_size - off;
        SIZE_T g = 0;
        ReadProcessMemory(g_hp, (LPCVOID)(g_base + off), img + off, chunk, &g);
    }

    /* Find FindPlayerPed pattern */
    auto findPat = [&](const char* name, const BYTE* pat, const char* mask, size_t plen) -> uintptr_t {
        for (size_t i = 0; i + plen <= g_size; i++) {
            bool ok = true;
            for (size_t j = 0; j < plen; j++)
                if (mask[j] == 'x' && img[i+j] != pat[j]) { ok = false; break; }
            if (ok) { fprintf(g_f, "%s @ +0x%X (0x%08X)\n", name, (unsigned)i, (unsigned)(g_base+i)); return g_base + i; }
        }
        fprintf(g_f, "%s NOT FOUND\n", name); return 0;
    };
    const BYTE patPed[]  = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x18,0xA1};
    const BYTE patVeh[]  = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x15,0xA1,
                            0,0,0,0,0x83,0xF8,0xFF,0x75,0x04,0x33,0xC0,0xEB,0x07};
    uintptr_t aPed = findPat("FindPlayerPed",     patPed, "xxxxxxxxx", 9);
    uintptr_t aVeh = findPat("FindPlayerVehicle", patVeh, "xxxxxxxxx????xxxxxxxxx", 22);

    /* Extract the A1 imm32 - global player-info pointer each function reads */
    uintptr_t gPed = 0, gVeh = 0;
    if (aPed) { gPed = *(uintptr_t*)(img + (aPed - g_base) + 9); fprintf(g_f, "  FindPlayerPed     uses global 0x%08X\n", (unsigned)gPed); }
    if (aVeh) { gVeh = *(uintptr_t*)(img + (aVeh - g_base) + 9); fprintf(g_f, "  FindPlayerVehicle uses global 0x%08X\n", (unsigned)gVeh); }

    /* Decode the FindPlayerPed function more carefully.
     * 8B 44 24 04   mov eax, [esp+4]    ; id
     * 85 C0         test eax, eax
     * 75 18         jnz +0x18
     * A1 ?? ?? ?? ??  mov eax, [global] ; player info pointer
     *
     * Then there are further bytes. Let's print 80 bytes of the function
     * so we can hand-decode. */
    if (aPed) {
        fprintf(g_f, "\nFindPlayerPed first 96 bytes:\n");
        for (int i = 0; i < 96; i++) {
            fprintf(g_f, "%02X ", img[aPed - g_base + i]);
            if ((i % 16) == 15) fprintf(g_f, "\n");
        }
        fprintf(g_f, "\n");
    }
    if (aVeh) {
        fprintf(g_f, "\nFindPlayerVehicle first 96 bytes:\n");
        for (int i = 0; i < 96; i++) {
            fprintf(g_f, "%02X ", img[aVeh - g_base + i]);
            if ((i % 16) == 15) fprintf(g_f, "\n");
        }
        fprintf(g_f, "\n");
    }

    /* The global at gPed is a pointer-to-array-of CPlayerInfo*.
     * On GTA IV: global -> array; array[0] -> CPlayerInfo; CPlayerInfo + ?? -> CPed.
     * Try to walk the chain dynamically. */
    if (gPed) {
        fprintf(g_f, "\nWalking from FindPlayerPed global 0x%08X:\n", (unsigned)gPed);
        uintptr_t arr = Rp(gPed);
        fprintf(g_f, "  [0x%08X] = 0x%08X\n", (unsigned)gPed, (unsigned)arr);
        if (isHeap(arr)) {
            /* CPlayerInfo* is in this array. Dump first 0x100 bytes. */
            fprintf(g_f, "  PlayerInfo struct at 0x%08X dump:\n", (unsigned)arr);
            BYTE pi[0x200];
            if (R(arr, pi, sizeof(pi))) {
                for (int off = 0; off < (int)sizeof(pi); off += 4) {
                    uintptr_t p = *(uintptr_t*)(pi + off);
                    if (isHeap(p)) {
                        uintptr_t vt = Rp(p);
                        if (inImage(vt)) {
                            fprintf(g_f, "    +0x%03X = 0x%08X (vt=0x%08X in game) <-- ped/object candidate\n",
                                    off, (unsigned)p, (unsigned)vt);
                        }
                    }
                }
            } else {
                fprintf(g_f, "    can't read PlayerInfo\n");
            }
        }
    }

    /* Now do the same for FindPlayerVehicle. The global is most likely a
     * pointer to the player-info array index; we need to find what it points
     * to. From FusionFix decode: cmp eax, -1; jnz +4; xor eax, eax; jmp +7
     * suggests this global is checked for -1 sentinel. */
    if (gVeh) {
        fprintf(g_f, "\nWalking from FindPlayerVehicle global 0x%08X:\n", (unsigned)gVeh);
        /* The function is probably: 
         *   if (id == 0) return ((g_PlayerInfo[g_PlayerSlot]) ? g_PlayerInfo[g_PlayerSlot]->vehicle : 0);
         * The "global" is likely the slot index or the playerinfo pointer.
         * Print the raw 4 bytes at the global address. */
        uintptr_t v = Rp(gVeh);
        fprintf(g_f, "  [0x%08X] = 0x%08X (decimal %d)\n", (unsigned)gVeh, (unsigned)v, (int)v);
    }

    /* Heuristic: scan game's writable memory for a CPlayerPed.
     * Better: dump bytes AFTER the A1 in FindPlayerVehicle so we can decode
     * the full function. */
    if (aVeh) {
        fprintf(g_f, "\nFindPlayerVehicle bytes 0x10..0x60 (after the early-return):\n");
        for (int i = 0x10; i < 0x60; i++) {
            fprintf(g_f, "%02X ", img[aVeh - g_base + i]);
            if (((i - 0x10) % 16) == 15) fprintf(g_f, "\n");
        }
        fprintf(g_f, "\n");
    }

    /* Search the writable heap for CPlayerPed-like objects:
     * A CPed has a vtable in GTAIV.exe AND lives in the heap.
     * Most peds have an offset to "ped name" / "model name" -- skip that.
     * Instead, walk every allocated heap region and look for vtables matching
     * known CPlayerPed vtable address (we don't know it). */

    /* Better strategy: enumerate writable regions, find pointers to game-image
     * addresses (vtables), and report cluster sizes. The most-populous vtable
     * is likely the player ped's. But this is too noisy for now.
     *
     * Quick approach: read player info pointer chain by trying common
     * GTAIV layouts: global -> CPlayerInfo*, CPlayerInfo + 0x510 -> CPed*,
     * CPed + 0x32C -> CVehicle*. */

    /* DISASSEMBLY-DRIVEN walk:
     *   slot     = *(int*)gPed        ; 0x01C16F14
     *   if slot==-1: no player
     *   array    = 0x01D88808
     *   pi       = ((CPlayerInfo**)array)[slot]
     *   ped      = *(void**)(pi + 0x598)
     *   inVeh    = *(BYTE*)(ped + 0x26C) & 4
     *   vehicle  = *(void**)(ped + 0xB30)   (only if inVeh)
     *
     * Offsets confirmed from disassembly above. */

    int slot = (int)Rp(gPed);
    fprintf(g_f, "\n=== DIRECT WALK ===\n");
    fprintf(g_f, "slot = %d\n", slot);
    if (slot < 0) {
        fprintf(g_f, "slot=-1: no player active. Get in-game and try again.\n");
    } else {
        uintptr_t arr = 0x01D88808;
        uintptr_t piPtrAddr = arr + slot * 4;
        uintptr_t pi = Rp(piPtrAddr);
        fprintf(g_f, "PlayerInfo* = *(0x%08X) = 0x%08X\n", (unsigned)piPtrAddr, (unsigned)pi);
        if (isHeap(pi)) {
            uintptr_t ped = Rp(pi + 0x598);
            fprintf(g_f, "Ped = *(0x%08X + 0x598) = 0x%08X\n", (unsigned)pi, (unsigned)ped);
            if (isHeap(ped)) {
                BYTE inVehFlag = 0;
                R(ped + 0x26C, &inVehFlag, 1);
                fprintf(g_f, "InVehicleFlag (ped+0x26C) = 0x%02X (in-vehicle bit 2 = %s)\n",
                        inVehFlag, (inVehFlag & 4) ? "YES" : "NO");
                uintptr_t veh = Rp(ped + 0xB30);
                fprintf(g_f, "Vehicle = *(ped+0xB30) = 0x%08X\n", (unsigned)veh);
                if (isHeap(veh)) {
                    uintptr_t vvt = Rp(veh);
                    fprintf(g_f, "Vehicle vtable = 0x%08X (%s game image)\n",
                            (unsigned)vvt, inImage(vvt) ? "in" : "OUT OF");
                    BYTE vehBuf[0x400];
                    if (R(veh, vehBuf, sizeof(vehBuf))) {
                        fprintf(g_f, "\n--- ALL float triplets in vehicle+0x00..0x400 with non-trivial magnitude ---\n");
                        for (int vo = 0; vo + 12 <= (int)sizeof(vehBuf); vo += 4) {
                            float* fp = (float*)(vehBuf + vo);
                            if (!std::isfinite(fp[0]) || !std::isfinite(fp[1]) || !std::isfinite(fp[2])) continue;
                            float mag = sqrtf(fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2]);
                            if (mag > 0.01f && mag < 1000.f) {
                                fprintf(g_f, "  +0x%03X (%9.3f, %9.3f, %9.3f) |m/s|=%7.2f km/h=%7.2f\n",
                                        vo, fp[0], fp[1], fp[2], mag, mag * 3.6f);
                            }
                        }
                        fprintf(g_f, "\n--- Single floats in vehicle+0x00..0x400 (potential scalar speed) ---\n");
                        for (int vo = 0; vo + 4 <= (int)sizeof(vehBuf); vo += 4) {
                            float* fp = (float*)(vehBuf + vo);
                            if (!std::isfinite(*fp)) continue;
                            float v = *fp;
                            /* speed scalar likely 0..150 m/s or 0..500 km/h */
                            if ((v > 1.f && v < 600.f) || (v < -1.f && v > -600.f)) {
                                /* Skip very common values like 1.0 to reduce noise */
                                if (v == 1.0f || v == -1.0f) continue;
                                fprintf(g_f, "  +0x%03X = %9.3f\n", vo, v);
                            }
                        }
                    }
                }
            }
        }
    }


    free(img);
    CloseHandle(g_hp);
    fclose(g_f);
    fprintf(stderr, "Done. Output: build\\probe_vehicle.txt\n");
    return 0;
}
