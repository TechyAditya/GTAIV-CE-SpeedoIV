/*
 * debug_scan.exe - Attach to running GTAIV.exe and find CPedFactory
 * Compile with 32-bit MinGW: g++ debug_scan.cpp -o debug_scan.exe -static
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>

static DWORD FindProcessId(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}

static bool GetModuleInfo(DWORD pid, const char* modName,
                          uintptr_t* base, size_t* size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, modName) == 0) {
                *base = (uintptr_t)me.modBaseAddr;
                *size = me.modBaseSize;
                CloseHandle(snap);
                return true;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return false;
}

static bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFF;
}

int main() {
    /* Redirect output to file so we can read it */
    FILE* fout = fopen("E:\\Code\\gta4\\speedometer\\build\\debug_output.txt", "w");
    if (fout) { /* redirect stdout */ 
        fclose(fout);
        freopen("E:\\Code\\gta4\\speedometer\\build\\debug_output.txt", "w", stdout);
    }
    printf("=== SpeedoIV-CE Debug Scanner ===\n\n");

    DWORD pid = FindProcessId("GTAIV.exe");
    if (!pid) { printf("ERROR: GTAIV.exe not running\n"); return 1; }
    printf("Found GTAIV.exe PID: %lu\n", pid);

    uintptr_t gameBase = 0; size_t gameSize = 0;
    if (!GetModuleInfo(pid, "GTAIV.exe", &gameBase, &gameSize)) {
        printf("ERROR: Cannot get module info (try running as admin)\n");
        return 1;
    }
    printf("Module base: 0x%08X, size: 0x%08X (%.1f MB)\n\n",
           (unsigned)gameBase, (unsigned)gameSize, gameSize / (1024.0*1024.0));

    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid);
    if (!hProc) {
        printf("ERROR: Cannot open process (try running as admin)\n");
        return 1;
    }

    /* Read the entire game image */
    BYTE* image = (BYTE*)malloc(gameSize);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProc, (LPCVOID)gameBase, image, gameSize, &bytesRead)) {
        printf("ERROR: ReadProcessMemory failed (got %zu bytes)\n", bytesRead);
        /* Try smaller chunks */
        memset(image, 0, gameSize);
        for (size_t off = 0; off < gameSize; off += 4096) {
            size_t chunk = (off + 4096 <= gameSize) ? 4096 : gameSize - off;
            ReadProcessMemory(hProc, (LPCVOID)(gameBase + off),
                              image + off, chunk, NULL);
        }
        printf("Read via chunks instead\n");
    }
    printf("Image read: %zu bytes\n\n", (size_t)gameSize);

    /* Pattern 1: A1 XX XX XX XX 85 C0 74 */
    printf("--- Pattern 1: A1 ?? ?? ?? ?? 85 C0 74 ---\n");
    int found1 = 0;
    for (size_t i = 0; i <= gameSize - 8 && found1 < 10; i++) {
        if (image[i] == 0xA1 &&
            image[i+5] == 0x85 && image[i+6] == 0xC0 && image[i+7] == 0x74) {
            uintptr_t ptrAddr = *(uintptr_t*)(image + i + 1);
            if (!IsValidPtr(ptrAddr)) continue;

            /* Read the global pointer value */
            uintptr_t factoryPtr = 0;
            if (ReadProcessMemory(hProc, (LPCVOID)ptrAddr,
                                  &factoryPtr, 4, NULL) && IsValidPtr(factoryPtr)) {
                /* Try reading ped pointer at +4 */
                uintptr_t pedPtr = 0;
                ReadProcessMemory(hProc, (LPCVOID)(factoryPtr + 0x4),
                                  &pedPtr, 4, NULL);

                printf("  Hit at +0x%06X: global=0x%08X -> factory=0x%08X -> ped[+4]=0x%08X",
                       (unsigned)(i), (unsigned)ptrAddr,
                       (unsigned)factoryPtr, (unsigned)pedPtr);

                if (IsValidPtr(pedPtr)) {
                    /* Try vehicle offsets */
                    int vehOffsets[] = {0x32C, 0x330, 0x328, 0x33C, 0x320};
                    for (int v = 0; v < 5; v++) {
                        uintptr_t vehPtr = 0;
                        ReadProcessMemory(hProc, (LPCVOID)(pedPtr + vehOffsets[v]),
                                          &vehPtr, 4, NULL);
                        if (vehPtr == 0 || IsValidPtr(vehPtr)) {
                            printf(" -> veh[+0x%X]=0x%08X",
                                   vehOffsets[v], (unsigned)vehPtr);

                            if (IsValidPtr(vehPtr)) {
                                /* Read velocity at various offsets */
                                int velOffsets[] = {0x70, 0x80, 0x90, 0x60};
                                for (int vo = 0; vo < 4; vo++) {
                                    float vel[3] = {};
                                    ReadProcessMemory(hProc,
                                        (LPCVOID)(vehPtr + velOffsets[vo]),
                                        vel, 12, NULL);
                                    float spd = sqrtf(vel[0]*vel[0] +
                                                     vel[1]*vel[1] +
                                                     vel[2]*vel[2]) * 3.6f;
                                    if (spd >= 0.0f && spd <= 500.0f) {
                                        printf(" -> vel[+0x%X]=(%.2f,%.2f,%.2f) = %.0f km/h",
                                               velOffsets[vo],
                                               vel[0], vel[1], vel[2], spd);
                                    }
                                }
                            }
                            printf(" [MATCH vehOff=0x%X]\n", vehOffsets[v]);
                            found1++;
                            goto next1;
                        }
                    }
                }
                printf("\n");
                next1:;
            }
        }
    }
    if (found1 == 0) printf("  No valid matches\n");

    /* Pattern 2: 8B 0D XX XX XX XX 8B 41 04 */
    printf("\n--- Pattern 2: 8B 0D ?? ?? ?? ?? 8B 41 04 ---\n");
    int found2 = 0;
    for (size_t i = 0; i <= gameSize - 9 && found2 < 10; i++) {
        if (image[i] == 0x8B && image[i+1] == 0x0D &&
            image[i+6] == 0x8B && image[i+7] == 0x41 && image[i+8] == 0x04) {
            uintptr_t ptrAddr = *(uintptr_t*)(image + i + 2);
            if (!IsValidPtr(ptrAddr)) continue;

            uintptr_t factoryPtr = 0;
            if (ReadProcessMemory(hProc, (LPCVOID)ptrAddr,
                                  &factoryPtr, 4, NULL) && IsValidPtr(factoryPtr)) {
                uintptr_t pedPtr = 0;
                ReadProcessMemory(hProc, (LPCVOID)(factoryPtr + 0x4),
                                  &pedPtr, 4, NULL);
                printf("  Hit at +0x%06X: global=0x%08X -> factory=0x%08X -> ped[+4]=0x%08X",
                       (unsigned)i, (unsigned)ptrAddr,
                       (unsigned)factoryPtr, (unsigned)pedPtr);

                if (IsValidPtr(pedPtr)) {
                    int vehOffsets[] = {0x32C, 0x330, 0x328, 0x33C, 0x320};
                    for (int v = 0; v < 5; v++) {
                        uintptr_t vehPtr = 0;
                        ReadProcessMemory(hProc, (LPCVOID)(pedPtr + vehOffsets[v]),
                                          &vehPtr, 4, NULL);
                        if (vehPtr == 0 || IsValidPtr(vehPtr)) {
                            printf(" -> veh[+0x%X]=0x%08X", vehOffsets[v], (unsigned)vehPtr);
                            if (IsValidPtr(vehPtr)) {
                                int velOffsets[] = {0x70, 0x80, 0x90, 0x60};
                                for (int vo = 0; vo < 4; vo++) {
                                    float vel[3] = {};
                                    ReadProcessMemory(hProc,
                                        (LPCVOID)(vehPtr + velOffsets[vo]),
                                        vel, 12, NULL);
                                    float spd = sqrtf(vel[0]*vel[0] +
                                                     vel[1]*vel[1] +
                                                     vel[2]*vel[2]) * 3.6f;
                                    if (spd >= 0.0f && spd <= 500.0f) {
                                        printf(" -> vel[+0x%X]=(%.2f,%.2f,%.2f) = %.0f km/h",
                                               velOffsets[vo],
                                               vel[0], vel[1], vel[2], spd);
                                    }
                                }
                            }
                            printf(" [MATCH]\n");
                            found2++;
                            goto next2;
                        }
                    }
                }
                printf("\n");
                next2:;
            }
        }
    }
    if (found2 == 0) printf("  No valid matches\n");

    printf("\n--- Done ---\n");
    printf("If you see speed readings above, the pattern scanner works.\n");
    printf("If all vehicle pointers are 0x00000000, you're on foot - get in a car and rerun.\n");

    free(image);
    CloseHandle(hProc);
    return 0;
}
