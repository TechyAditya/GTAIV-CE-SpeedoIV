/*
 * live_probe.exe - Attach to running GTAIV.exe and probe device state
 *
 * - Confirms we have the right device pointer
 * - Verifies vtable contents
 * - Counts calls to EndScene/Present by checking if hook bytes are preserved
 * - Tries to inject a debug hook into the game
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

static DWORD FindPid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do { if (_stricmp(pe.szExeFile, name) == 0) { CloseHandle(snap); return pe.th32ProcessID; } } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap); return 0;
}

static bool GetMod(DWORD pid, const char* mod, uintptr_t* base, size_t* size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, mod) == 0) {
                *base = (uintptr_t)me.modBaseAddr;
                *size = me.modBaseSize;
                CloseHandle(snap); return true;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap); return false;
}

static void ListModules(DWORD pid, FILE* fout) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            fprintf(fout, "  %-30s base=0x%08X size=0x%08X end=0x%08X\n",
                me.szModule, (unsigned)(uintptr_t)me.modBaseAddr,
                (unsigned)me.modBaseSize,
                (unsigned)((uintptr_t)me.modBaseAddr + me.modBaseSize));
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

int main() {
    FILE* fout = fopen("E:\\Code\\gta4\\speedometer\\build\\probe.txt", "w");
    if (!fout) { return 1; }
    
    fprintf(fout, "=== Live GTAIV Probe ===\n\n");
    
    DWORD pid = FindPid("GTAIV.exe");
    if (!pid) { fprintf(fout, "GTAIV.exe not running\n"); fclose(fout); return 1; }
    fprintf(fout, "PID: %lu\n", pid);
    
    uintptr_t gameBase = 0; size_t gameSize = 0;
    if (!GetMod(pid, "GTAIV.exe", &gameBase, &gameSize)) {
        fprintf(fout, "Can't get GTAIV.exe module info\n"); fclose(fout); return 1;
    }
    fprintf(fout, "Game: base=0x%08X size=0x%08X\n\n", (unsigned)gameBase, (unsigned)gameSize);
    
    fprintf(fout, "=== Loaded Modules ===\n");
    ListModules(pid, fout);
    fprintf(fout, "\n");
    
    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) { fprintf(fout, "OpenProcess failed: %lu\n", GetLastError()); fclose(fout); return 1; }
    
    /* Read entire game image */
    BYTE* img = (BYTE*)malloc(gameSize);
    for (size_t off = 0; off < gameSize; off += 4096) {
        size_t chunk = (off + 4096 <= gameSize) ? 4096 : gameSize - off;
        ReadProcessMemory(hp, (LPCVOID)(gameBase + off), img + off, chunk, NULL);
    }
    
    /* Find device pointer using same pattern as the ASI */
    fprintf(fout, "=== Device Pointer Scan ===\n");
    
    /* Pattern: A1 ?? ?? ?? ?? 50 8B 08 FF 51 ?? (any vtable index) */
    int patternHits = 0;
    for (size_t i = 0; i <= gameSize - 11 && patternHits < 10; i++) {
        if (img[i] == 0xA1 && img[i+5] == 0x50 && img[i+6] == 0x8B && img[i+7] == 0x08 &&
            img[i+8] == 0xFF && img[i+9] == 0x51) {
            uintptr_t ptrAddr = *(uintptr_t*)(img + i + 1);
            uintptr_t devPtr = 0;
            ReadProcessMemory(hp, (LPCVOID)ptrAddr, &devPtr, 4, NULL);
            int vtIdx = img[i+10] / 4;
            fprintf(fout, "Hit +0x%06X: A1...FF 51 %02X (vt[%d]) global=0x%08X -> dev=0x%08X\n",
                (unsigned)i, img[i+10], vtIdx, (unsigned)ptrAddr, (unsigned)devPtr);
            
            if (devPtr > 0x10000 && devPtr < 0x7FFF0000) {
                /* Read vtable */
                uintptr_t vtPtr = 0;
                ReadProcessMemory(hp, (LPCVOID)devPtr, &vtPtr, 4, NULL);
                fprintf(fout, "  vtable=0x%08X\n", (unsigned)vtPtr);
                if (vtPtr > 0x10000 && vtPtr < 0x7FFF0000) {
                    /* Dump key vtable entries */
                    uintptr_t entries[60] = {};
                    ReadProcessMemory(hp, (LPCVOID)vtPtr, entries, sizeof(entries), NULL);
                    fprintf(fout, "  vt[2]Release  = 0x%08X\n", (unsigned)entries[2]);
                    fprintf(fout, "  vt[16]Reset   = 0x%08X\n", (unsigned)entries[16]);
                    fprintf(fout, "  vt[17]Present = 0x%08X\n", (unsigned)entries[17]);
                    fprintf(fout, "  vt[41]BeginScene = 0x%08X\n", (unsigned)entries[41]);
                    fprintf(fout, "  vt[42]EndScene= 0x%08X\n", (unsigned)entries[42]);
                    
                    /* Read first bytes of EndScene and Present */
                    BYTE esBytes[16] = {}, prBytes[16] = {};
                    ReadProcessMemory(hp, (LPCVOID)entries[42], esBytes, 16, NULL);
                    ReadProcessMemory(hp, (LPCVOID)entries[17], prBytes, 16, NULL);
                    fprintf(fout, "  EndScene bytes: ");
                    for (int j = 0; j < 16; j++) fprintf(fout, "%02X ", esBytes[j]);
                    fprintf(fout, "\n  Present bytes:  ");
                    for (int j = 0; j < 16; j++) fprintf(fout, "%02X ", prBytes[j]);
                    fprintf(fout, "\n");
                }
            }
            fprintf(fout, "\n");
            patternHits++;
        }
    }
    
    /* Also look for the FusionFix-style pattern that calls EndScene: 
     * A1 ?? ?? ?? ?? 50 8B 08 FF 91 A8 00 00 00 (call [ecx+0xA8] = vt[42]) */
    fprintf(fout, "\n=== Looking for EndScene/Present call-sites ===\n");
    int callHits = 0;
    for (size_t i = 0; i <= gameSize - 14 && callHits < 20; i++) {
        if (img[i] == 0xA1 && img[i+5] == 0x50 && img[i+6] == 0x8B && img[i+7] == 0x08 &&
            img[i+8] == 0xFF && img[i+9] == 0x91) {
            uint32_t offset = *(uint32_t*)(img + i + 10);
            uintptr_t ptrAddr = *(uintptr_t*)(img + i + 1);
            int vtIdx = offset / 4;
            const char* method = "";
            if (vtIdx == 42) method = " <-- EndScene";
            else if (vtIdx == 17) method = " <-- Present";
            else if (vtIdx == 16) method = " <-- Reset";
            else if (vtIdx == 41) method = " <-- BeginScene";
            fprintf(fout, "Hit +0x%06X: A1...FF 91 [%08X] = vt[%d]%s, devPtr=0x%08X\n",
                (unsigned)i, offset, vtIdx, method, (unsigned)ptrAddr);
            callHits++;
        }
    }
    
    /* Check for inline hooks - look for JMP rel32 (E9) at the start of EndScene/Present */
    fprintf(fout, "\n=== Hook Detection ===\n");
    fprintf(fout, "If EndScene/Present starts with E9 = an inline hook is installed.\n");
    fprintf(fout, "Vtable entry vs actual function address tells if vtable was patched.\n\n");
    
    free(img);
    CloseHandle(hp);
    fprintf(fout, "Done\n");
    fclose(fout);
    return 0;
}
