/* probe_dynamic.cpp - sample the player vehicle struct repeatedly over N seconds
 * and report which float-triplets change. The live velocity vector will jitter
 * across samples while driving; rotation matrices and constants will stay still
 * (or change but in a non-velocity-like way). */
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
static bool isHeap(uintptr_t p)  { return p >= 0x04000000 && p < 0x7FFFFFFF; }
static bool inImage(uintptr_t p) { return p >= g_base && p < g_base + g_size; }

int main(int argc, char** argv) {
    /* Mode 1: ./probe_dynamic.exe N seconds at 10Hz (default)
     * Mode 2: ./probe_dynamic.exe -snap   = 3 single snapshots, 1s apart */
    bool snapMode = (argc > 1 && _stricmp(argv[1], "-snap") == 0);
    int durationSec = (argc > 1 && !snapMode) ? atoi(argv[1]) : 6;
    int sampleHz = 10;
    int NUM_SAMPLES;
    if (snapMode) {
        NUM_SAMPLES = 3;
    } else {
        NUM_SAMPLES = durationSec * sampleHz;
    }

    g_f = fopen("E:\\Code\\gta4\\speedometer\\build\\probe_dynamic.txt", "w");
    if (!g_f) return 1;
    if (snapMode)
        fprintf(g_f, "Snapshot mode: %d snapshots, 1 second apart...\n", NUM_SAMPLES);
    else
        fprintf(g_f, "Sampling for %d seconds at %d Hz...\n", durationSec, sampleHz);
    fflush(g_f);

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

    g_hp = OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_hp) { fprintf(g_f, "OpenProcess err=%lu\n", GetLastError()); fclose(g_f); return 1; }

    /* Get vehicle pointer once via known chain (initial check only - re-resolved per sample) */
    const uintptr_t slotAddr = 0x01C16F14;
    const uintptr_t arrAddr  = 0x01D88808;
    int slot = (int)Rp(slotAddr);
    uintptr_t pi = 0, ped = 0, veh = 0;
    if (slot >= 0) {
        pi  = Rp(arrAddr + slot*4);
        if (isHeap(pi)) {
            ped = Rp(pi + 0x598);
            if (isHeap(ped)) {
                veh = Rp(ped + 0xB30);
            }
        }
    }
    fprintf(g_f, "Initial state: slot=%d pi=0x%08X ped=0x%08X veh=0x%08X\n",
            slot, (unsigned)pi, (unsigned)ped, (unsigned)veh);
    if (!isHeap(veh)) {
        fprintf(g_f, "WARNING: not in vehicle right now. Will retry each sample.\n");
    }

    const int STRUCT_SIZE = 0x1400;  /* GTA IV CVehicle is ~5000 bytes */
    BYTE** samples = (BYTE**)calloc(NUM_SAMPLES, sizeof(BYTE*));
    DWORD* timestamps = (DWORD*)calloc(NUM_SAMPLES, sizeof(DWORD));
    for (int i = 0; i < NUM_SAMPLES; i++) samples[i] = (BYTE*)malloc(STRUCT_SIZE);

    fprintf(g_f, "Collecting samples...\n"); fflush(g_f);
    DWORD t0 = GetTickCount();
    int sleepMs = snapMode ? 1000 : (1000 / sampleHz);
    for (int i = 0; i < NUM_SAMPLES; i++) {
        timestamps[i] = GetTickCount() - t0;
        /* RE-RESOLVE the vehicle pointer each sample! Vehicle can change. */
        int curSlot = (int)Rp(slotAddr);
        uintptr_t curVeh = 0;
        if (curSlot >= 0) {
            uintptr_t curPi  = Rp(arrAddr + curSlot*4);
            if (isHeap(curPi)) {
                uintptr_t curPed = Rp(curPi + 0x598);
                if (isHeap(curPed)) {
                    curVeh = Rp(curPed + 0xB30);
                }
            }
        }
        if (isHeap(curVeh)) {
            R(curVeh, samples[i], STRUCT_SIZE);
            fprintf(g_f, "  Sample %d at t=%lums: vehicle=0x%08X\n", i, timestamps[i], (unsigned)curVeh);
        } else {
            memset(samples[i], 0, STRUCT_SIZE);
            fprintf(g_f, "  Sample %d at t=%lums: vehicle=NULL\n", i, timestamps[i]);
        }
        fflush(g_f);
        if (i + 1 < NUM_SAMPLES) Sleep(sleepMs);
    }

    /* For each 4-byte offset, compute min/max/mean/stddev across samples */
    fprintf(g_f, "\n=== Per-offset variance analysis ===\n");
    fprintf(g_f, "Looking for floats that VARY across samples (live data, not static).\n\n");
    fprintf(g_f, "%-6s %12s %12s %12s %12s  %s\n",
            "Offset", "min", "max", "mean", "stddev", "comment");

    /* Also collect float-triplet magnitudes per sample */
    for (int off = 0; off + 12 <= STRUCT_SIZE; off += 4) {
        float mn = 1e30f, mx = -1e30f, sum = 0;
        int n = 0;
        bool allFinite = true;
        for (int s = 0; s < NUM_SAMPLES; s++) {
            float v = *(float*)(samples[s] + off);
            if (!std::isfinite(v)) { allFinite = false; break; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v; n++;
        }
        if (!allFinite || n == 0) continue;
        float mean = sum / n;
        float var = 0;
        for (int s = 0; s < NUM_SAMPLES; s++) {
            float v = *(float*)(samples[s] + off);
            var += (v - mean) * (v - mean);
        }
        float sd = sqrtf(var / n);

        /* Filter: only show changing values, but allow ranges as small as 0.05.
         * Skip insane absolute values (>100k) since those are garbage. */
        float range = mx - mn;
        if (range < 0.05f) continue;
        if (fabsf(mn) > 100000.f || fabsf(mx) > 100000.f) continue;
        const char* note = "";
        if (range > 30.f && fabsf(mean) < 10000.f && fabsf(mean) > 100.f) note = "<- position-like";
        if (range > 0.5f && range < 60.f && fabsf(mean) < 60.f) note = "<- could be velocity component";

        fprintf(g_f, "+0x%03X %12.4f %12.4f %12.4f %12.4f  %s\n",
                off, mn, mx, mean, sd, note);
    }

    /* For each 4-byte offset, also try interpreting as a velocity triplet
     * (off, off+4, off+8) and report the per-sample magnitude variance. */
    fprintf(g_f, "\n=== Triplet magnitude trace (km/h) - shows ALL plausibly velocity-shaped ===\n");
    int candidates[512]; int nc = 0;
    for (int off = 0; off + 12 <= STRUCT_SIZE; off += 4) {
        /* Compute per-sample magnitudes */
        float magMin = 1e30f, magMax = -1e30f;
        bool good = true;
        for (int s = 0; s < NUM_SAMPLES; s++) {
            float* fp = (float*)(samples[s] + off);
            if (!std::isfinite(fp[0]) || !std::isfinite(fp[1]) || !std::isfinite(fp[2])) { good = false; break; }
            float m = sqrtf(fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2]);
            if (m > 10000.f) { good = false; break; }
            if (m < magMin) magMin = m;
            if (m > magMax) magMax = m;
        }
        if (!good) continue;
        float magRange = magMax - magMin;
        /* Velocity should swing. Be permissive: max <=200 m/s (720 km/h sanity), range >= 0.1 */
        if (magMax > 200.f) continue;
        if (magRange < 0.1f) continue;
        if (nc < 512) candidates[nc++] = off;
    }
    fprintf(g_f, "Found %d triplet offsets with magnitude range >= 0.1 m/s and max <= 200 m/s\n\n", nc);

    /* Header: print first 24 column labels */
    int maxCols = (nc < 24) ? nc : 24;
    fprintf(g_f, "%-6s ", "t(ms)");
    for (int c = 0; c < maxCols; c++) fprintf(g_f, "+%03X    ", candidates[c]);
    fprintf(g_f, "\n");
    for (int s = 0; s < NUM_SAMPLES; s++) {
        fprintf(g_f, "%6lu ", timestamps[s]);
        for (int c = 0; c < maxCols; c++) {
            float* fp = (float*)(samples[s] + candidates[c]);
            float m = sqrtf(fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2]) * 3.6f;
            fprintf(g_f, "%7.2f ", m);
        }
        fprintf(g_f, "\n");
    }

    /* If more than 24 candidates, also print the second batch */
    if (nc > 24) {
        fprintf(g_f, "\n--- batch 2 ---\n");
        int end = (nc < 48) ? nc : 48;
        fprintf(g_f, "%-6s ", "t(ms)");
        for (int c = 24; c < end; c++) fprintf(g_f, "+%03X    ", candidates[c]);
        fprintf(g_f, "\n");
        for (int s = 0; s < NUM_SAMPLES; s++) {
            fprintf(g_f, "%6lu ", timestamps[s]);
            for (int c = 24; c < end; c++) {
                float* fp = (float*)(samples[s] + candidates[c]);
                float m = sqrtf(fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2]) * 3.6f;
                fprintf(g_f, "%7.2f ", m);
            }
            fprintf(g_f, "\n");
        }
    }

    /* === DEEP DIVE: scan SUB-OBJECTS pointed to from the vehicle ===
     * GTAIV's CVehicle likely stores velocity in a sub-object like
     * m_pPhysicalInstance or m_pFragment. Enumerate every heap pointer
     * in the vehicle struct, follow it, and look for velocity-like
     * triplets in the FIRST sample only (for brevity). */
    fprintf(g_f, "\n=== Sub-object scan (heap pointers in vehicle, follow them) ===\n");
    for (int off = 0; off + 4 <= STRUCT_SIZE; off += 4) {
        uintptr_t p = *(uintptr_t*)(samples[0] + off);
        if (!isHeap(p)) continue;
        /* Sanity check: must look like an object (has vtable in game image at +0) */
        uintptr_t vt = Rp(p);
        bool hasVt = inImage(vt);
        BYTE subBuf[0x200];
        if (!R(p, subBuf, sizeof(subBuf))) continue;
        /* Also re-read for samples 1, 2 to detect change */
        BYTE subBuf1[0x200], subBuf2[0x200];
        bool gotS1 = (NUM_SAMPLES > 1) && R(p, subBuf1, sizeof(subBuf1));
        bool gotS2 = (NUM_SAMPLES > 2) && R(p, subBuf2, sizeof(subBuf2));

        /* Look for velocity-like triplets in sub-object */
        bool printedHdr = false;
        for (int so = 0; so + 12 <= (int)sizeof(subBuf); so += 4) {
            float* fp = (float*)(subBuf + so);
            if (!std::isfinite(fp[0]) || !std::isfinite(fp[1]) || !std::isfinite(fp[2])) continue;
            float mag = sqrtf(fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2]);
            if (mag < 0.5f || mag > 200.f) continue;
            /* Filter: at least one component is non-zero ground-plane (X or Y) */
            if (fabsf(fp[0]) + fabsf(fp[1]) < 0.5f) continue;
            if (!printedHdr) {
                fprintf(g_f, "\nveh+0x%03X -> 0x%08X (vt=0x%08X %s):\n",
                        off, (unsigned)p, (unsigned)vt, hasVt ? "in-image" : "no-vt");
                printedHdr = true;
            }
            /* Magnitude across samples */
            char trace[256] = "";
            if (gotS1 && gotS2) {
                float* fp1 = (float*)(subBuf1 + so);
                float* fp2 = (float*)(subBuf2 + so);
                float m0 = sqrtf(fp[0]*fp[0]+fp[1]*fp[1]+fp[2]*fp[2]) * 3.6f;
                float m1 = sqrtf(fp1[0]*fp1[0]+fp1[1]*fp1[1]+fp1[2]*fp1[2]) * 3.6f;
                float m2 = sqrtf(fp2[0]*fp2[0]+fp2[1]*fp2[1]+fp2[2]*fp2[2]) * 3.6f;
                snprintf(trace, sizeof(trace), "  km/h: %6.2f -> %6.2f -> %6.2f", m0, m1, m2);
            }
            fprintf(g_f, "  +0x%03X (%8.3f, %8.3f, %8.3f) |m/s|=%6.2f km/h=%6.2f%s\n",
                    so, fp[0], fp[1], fp[2], mag, mag * 3.6f, trace);
        }
    }

    for (int i = 0; i < NUM_SAMPLES; i++) free(samples[i]);
    free(samples); free(timestamps);
    CloseHandle(g_hp);
    fclose(g_f);
    fprintf(stderr, "Done. Output: build\\probe_dynamic.txt\n");
    return 0;
}
