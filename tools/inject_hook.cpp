/*
 * inject_hook.exe - Install a wrapper-vtable hook into running GTAIV.exe
 *
 * Writes a small assembly stub to game memory that:
 *   - Calls our reporter function (logs that EndScene was called)
 *   - Then jumps to the original EndScene
 *
 * Then patches RAGE wrapper vtable[42] to point to our stub.
 *
 * This is a proof-of-concept to confirm the wrapper-vtable hook approach works.
 * If we see "EndScene called!" in the log file, we know the approach is correct.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

static FILE* g_out = nullptr;
static void L(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(g_out, fmt, ap); va_end(ap);
    fputc('\n', g_out); fflush(g_out);
}

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

int main() {
    g_out = fopen("E:\\Code\\gta4\\speedometer\\build\\inject.txt", "w");
    L("=== Inject Hook ===");
    
    DWORD pid = FindPid("GTAIV.exe");
    if (!pid) { L("Not running"); fclose(g_out); return 1; }
    L("PID: %lu", pid);
    
    uintptr_t gameBase; size_t gameSize;
    if (!GetMod(pid, "GTAIV.exe", &gameBase, &gameSize)) { L("No GTAIV.exe"); fclose(g_out); return 1; }
    
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) { L("OpenProcess failed: %lu", GetLastError()); fclose(g_out); return 1; }
    
    /* Read game image */
    BYTE* img = (BYTE*)malloc(gameSize);
    for (size_t off = 0; off < gameSize; off += 4096) {
        size_t chunk = (off + 4096 <= gameSize) ? 4096 : gameSize - off;
        ReadProcessMemory(hp, (LPCVOID)(gameBase + off), img + off, chunk, NULL);
    }
    
    /* Find RAGE wrapper */
    uintptr_t wrapperVt = 0, wrapperDev = 0;
    for (size_t i = 0; i <= gameSize - 11; i++) {
        if (img[i] == 0xA1 && img[i+5] == 0x50 && img[i+6] == 0x8B && img[i+7] == 0x08 &&
            img[i+8] == 0xFF && img[i+9] == 0x51) {
            uintptr_t ptrAddr = *(uintptr_t*)(img + i + 1);
            uintptr_t dev = 0;
            ReadProcessMemory(hp, (LPCVOID)ptrAddr, &dev, 4, NULL);
            if (dev > 0x10000 && dev < 0x7FFF0000) {
                uintptr_t vt = 0;
                ReadProcessMemory(hp, (LPCVOID)dev, &vt, 4, NULL);
                if (vt >= gameBase && vt < gameBase + gameSize) {
                    uintptr_t es = 0;
                    ReadProcessMemory(hp, (LPCVOID)(vt + 42*4), &es, 4, NULL);
                    if (es > 0x10000) {
                        wrapperVt = vt; wrapperDev = dev;
                        L("Wrapper: dev=0x%08X vt=0x%08X vt[42]=0x%08X", (unsigned)dev, (unsigned)vt, (unsigned)es);
                        break;
                    }
                }
            }
        }
    }
    free(img);
    if (!wrapperVt) { L("No wrapper"); fclose(g_out); CloseHandle(hp); return 1; }
    
    /* Read original EndScene */
    uintptr_t origEndScene = 0;
    ReadProcessMemory(hp, (LPCVOID)(wrapperVt + 42*4), &origEndScene, 4, NULL);
    L("Original vt[42] = 0x%08X", (unsigned)origEndScene);
    
    /* Allocate executable memory in the game for our counter + stub */
    LPVOID remoteMem = VirtualAllocEx(hp, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteMem) { L("VirtualAllocEx failed"); fclose(g_out); CloseHandle(hp); return 1; }
    L("Remote stub at 0x%08X", (unsigned)(uintptr_t)remoteMem);
    
    /*
     * Stub layout at remoteMem:
     *   +0x00: DWORD counter
     *   +0x04: stub code:
     *
     *     ; HRESULT __stdcall MyEndScene(IDirect3DDevice9* this)
     *     8B 44 24 04         mov eax, [esp+4]    ; this -> eax (we ignore)
     *     FF 05 XX XX XX XX   inc dword [counter] ; bump counter
     *     B8 XX XX XX XX      mov eax, origEndScene
     *     FF E0               jmp eax             ; tail-call original
     */
    
    uintptr_t counterAddr = (uintptr_t)remoteMem;       /* +0x00 */
    uintptr_t stubAddr = (uintptr_t)remoteMem + 0x10;   /* +0x10 (aligned) */
    
    BYTE stub[64] = {};
    int p = 0;
    /* mov eax, [esp+4]  - just to test stack works (could omit) */
    stub[p++] = 0x8B; stub[p++] = 0x44; stub[p++] = 0x24; stub[p++] = 0x04;
    /* inc dword [counter] */
    stub[p++] = 0xFF; stub[p++] = 0x05;
    *(uint32_t*)(stub + p) = (uint32_t)counterAddr; p += 4;
    /* mov eax, origEndScene */
    stub[p++] = 0xB8;
    *(uint32_t*)(stub + p) = (uint32_t)origEndScene; p += 4;
    /* jmp eax */
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    
    /* Initialize counter to 0 */
    DWORD zero = 0;
    WriteProcessMemory(hp, remoteMem, &zero, 4, NULL);
    /* Write stub at +0x10 */
    WriteProcessMemory(hp, (LPVOID)stubAddr, stub, p, NULL);
    L("Stub written: %d bytes", p);
    
    /* Patch vtable[42] to point to our stub */
    DWORD oldProt;
    VirtualProtectEx(hp, (LPVOID)(wrapperVt + 42*4), 4, PAGE_READWRITE, &oldProt);
    WriteProcessMemory(hp, (LPVOID)(wrapperVt + 42*4), &stubAddr, 4, NULL);
    VirtualProtectEx(hp, (LPVOID)(wrapperVt + 42*4), 4, oldProt, &oldProt);
    
    uintptr_t verify = 0;
    ReadProcessMemory(hp, (LPCVOID)(wrapperVt + 42*4), &verify, 4, NULL);
    L("vt[42] now = 0x%08X (should be 0x%08X)", (unsigned)verify, (unsigned)stubAddr);
    
    /* Sample counter for 5 seconds */
    L("Sampling EndScene call count every 1s for 5s...");
    DWORD prev = 0;
    for (int i = 0; i < 5; i++) {
        Sleep(1000);
        DWORD curr = 0;
        ReadProcessMemory(hp, remoteMem, &curr, 4, NULL);
        L("  t+%ds: counter = %lu  (delta: %lu)", i+1, curr, curr - prev);
        prev = curr;
    }
    
    /* Restore vtable so the game doesn't crash when we exit */
    /* Actually - LEAVE IT HOOKED so the user can see if it works long-term */
    /* If counter is incrementing, EndScene IS being called through the wrapper */
    
    L("=== Done. Hook left installed. ===");
    CloseHandle(hp);
    fclose(g_out);
    return 0;
}
