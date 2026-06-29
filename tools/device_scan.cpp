/*
 * Find rage::grcDevice::ms_pD3DDevice in running GTAIV.exe
 * Look for patterns that reference the global device pointer.
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
    if (Module32First(snap, &me)) { do { if (_stricmp(me.szModule, mod) == 0) { *base = (uintptr_t)me.modBaseAddr; *size = me.modBaseSize; CloseHandle(snap); return true; } } while (Module32Next(snap, &me)); }
    CloseHandle(snap); return false;
}

int main() {
    freopen("E:\\Code\\gta4\\speedometer\\build\\device_scan.txt", "w", stdout);
    DWORD pid = FindPid("GTAIV.exe");
    if (!pid) { printf("Not running\n"); return 1; }
    uintptr_t base = 0; size_t size = 0;
    if (!GetMod(pid, "GTAIV.exe", &base, &size)) { printf("Can't get module\n"); return 1; }
    printf("Base: 0x%08X Size: 0x%08X\n\n", (unsigned)base, (unsigned)size);

    HANDLE hp = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!hp) { printf("Can't open process\n"); return 1; }

    BYTE* img = (BYTE*)malloc(size);
    for (size_t off = 0; off < size; off += 4096) {
        size_t chunk = (off + 4096 <= size) ? 4096 : size - off;
        ReadProcessMemory(hp, (LPCVOID)(base + off), img + off, chunk, NULL);
    }

    /* 
     * In GTA IV, the device pointer is stored in a global. Code accesses it like:
     * A1 XX XX XX XX          mov eax, [g_pDevice]
     * ... then calls through vtable
     * 
     * Or commonly:
     * 8B 0D XX XX XX XX       mov ecx, [g_pDevice]  
     * 8B 01                   mov eax, [ecx]    (vtable)
     * FF 50 XX                call [eax+XX]     (vtable method)
     *
     * Also look for Present (vtable index 17): FF 50 44 (call [eax+0x44])
     * Or EndScene (vtable index 42): FF 50 A8 (call [eax+0xA8])
     * Or BeginScene (vtable index 41): FF 90 A4 00 00 00
     */

    printf("--- Pattern: 8B 0D ?? ?? ?? ?? 8B 01 FF 50 A8 (EndScene call) ---\n");
    int count = 0;
    for (size_t i = 0; i <= size - 11 && count < 20; i++) {
        if (img[i] == 0x8B && img[i+1] == 0x0D &&
            img[i+6] == 0x8B && img[i+7] == 0x01 &&
            img[i+8] == 0xFF && img[i+9] == 0x50 && img[i+10] == 0xA8) {
            uintptr_t gptr = *(uintptr_t*)(img + i + 2);
            uintptr_t devPtr = 0;
            ReadProcessMemory(hp, (LPCVOID)gptr, &devPtr, 4, NULL);
            printf("  +0x%06X: global=0x%08X -> device=0x%08X\n",
                   (unsigned)i, (unsigned)gptr, (unsigned)devPtr);
            count++;
        }
    }

    printf("\n--- Pattern: 8B 0D ?? ?? ?? ?? 8B 01 FF 50 44 (Present call) ---\n");
    count = 0;
    for (size_t i = 0; i <= size - 11 && count < 20; i++) {
        if (img[i] == 0x8B && img[i+1] == 0x0D &&
            img[i+6] == 0x8B && img[i+7] == 0x01 &&
            img[i+8] == 0xFF && img[i+9] == 0x50 && img[i+10] == 0x44) {
            uintptr_t gptr = *(uintptr_t*)(img + i + 2);
            uintptr_t devPtr = 0;
            ReadProcessMemory(hp, (LPCVOID)gptr, &devPtr, 4, NULL);
            printf("  +0x%06X: global=0x%08X -> device=0x%08X\n",
                   (unsigned)i, (unsigned)gptr, (unsigned)devPtr);
            count++;
        }
    }

    /* Also try: A1 XX XX XX XX 8B 08 (mov eax,[ptr]; mov ecx,[eax] = vtable) */
    printf("\n--- Pattern: A1 ?? ?? ?? ?? 8B 08 FF 51 (vtable call via eax) ---\n");
    count = 0;
    for (size_t i = 0; i <= size - 9 && count < 20; i++) {
        if (img[i] == 0xA1 && img[i+5] == 0x8B && img[i+6] == 0x08 &&
            img[i+7] == 0xFF && img[i+8] == 0x51) {
            uintptr_t gptr = *(uintptr_t*)(img + i + 1);
            if (gptr > 0x10000 && gptr < 0x7FFFFFFF) {
                uintptr_t devPtr = 0;
                ReadProcessMemory(hp, (LPCVOID)gptr, &devPtr, 4, NULL);
                if (devPtr > 0x10000 && devPtr < 0x7FFFFFFF) {
                    printf("  +0x%06X: global=0x%08X -> ptr=0x%08X vtIdx=0x%02X\n",
                           (unsigned)i, (unsigned)gptr, (unsigned)devPtr, img[i+9]);
                    count++;
                }
            }
        }
    }

    /* Broader: look for the EndScene vtable index 42*4 = 0xA8 called via register */
    printf("\n--- Pattern: 8B 0D ?? ?? ?? ?? 8B 11 FF 92 A8000000 (EndScene via edx) ---\n");
    count = 0;
    for (size_t i = 0; i <= size - 12 && count < 20; i++) {
        if (img[i] == 0x8B && img[i+1] == 0x0D &&
            img[i+6] == 0x8B && img[i+7] == 0x11 &&
            img[i+8] == 0xFF && img[i+9] == 0x92 &&
            *(uint32_t*)(img+i+10) == 0xA8) {
            uintptr_t gptr = *(uintptr_t*)(img + i + 2);
            uintptr_t devPtr = 0;
            ReadProcessMemory(hp, (LPCVOID)gptr, &devPtr, 4, NULL);
            printf("  +0x%06X: global=0x%08X -> device=0x%08X\n",
                   (unsigned)i, (unsigned)gptr, (unsigned)devPtr);
            count++;
        }
    }

    free(img);
    CloseHandle(hp);
    printf("\nDone\n");
    return 0;
}
