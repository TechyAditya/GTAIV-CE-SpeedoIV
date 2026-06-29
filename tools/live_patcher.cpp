/*
 * live_patcher.exe - Install the speedometer hook into the running game
 *
 * 1. Find RAGE wrapper device (vtable inside GTAIV.exe)
 * 2. Resolve HookedEndScene and SetOrigEndScene from loaded SpeedoIV-CE.asi
 * 3. Save current vt[42] as the "original"
 * 4. Call SetOrigEndScene via remote thread to save it in the ASI
 * 5. Overwrite vt[42] with HookedEndScene's address
 *
 * After this, every EndScene call goes: game -> HookedEndScene -> callback (draws) -> orig
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
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

/* Resolve an export from a remote module by reading its export table */
static uintptr_t ResolveExport(HANDLE hp, uintptr_t modBase, size_t modSize, const char* name) {
    BYTE* img = (BYTE*)malloc(modSize);
    for (size_t off = 0; off < modSize; off += 4096) {
        size_t chunk = (off + 4096 <= modSize) ? 4096 : modSize - off;
        ReadProcessMemory(hp, (LPCVOID)(modBase + off), img + off, chunk, NULL);
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(img + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp.Size == 0) { free(img); return 0; }
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(img + exp.VirtualAddress);
    DWORD* names = (DWORD*)(img + ed->AddressOfNames);
    WORD* ords = (WORD*)(img + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(img + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* fn = (const char*)(img + names[i]);
        if (strcmp(fn, name) == 0) {
            DWORD rva = funcs[ords[i]];
            free(img);
            return modBase + rva;
        }
    }
    free(img);
    return 0;
}

int main() {
    g_out = fopen("E:\\Code\\gta4\\speedometer\\build\\patcher.txt", "w");
    L("=== Live Hook Installer ===");
    
    DWORD pid = FindPid("GTAIV.exe");
    if (!pid) { L("Game not running"); return 1; }
    L("PID: %lu", pid);
    
    uintptr_t gameBase, asiBase;
    size_t gameSize, asiSize;
    if (!GetMod(pid, "GTAIV.exe", &gameBase, &gameSize)) { L("No GTAIV.exe"); return 1; }
    if (!GetMod(pid, "SpeedoIV-CE.asi", &asiBase, &asiSize)) { L("ASI not loaded"); return 1; }
    L("Game: 0x%08X  ASI: 0x%08X", (unsigned)gameBase, (unsigned)asiBase);
    
    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | 
                            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) { L("OpenProcess failed: %lu", GetLastError()); return 1; }
    
    /* Resolve exports */
    uintptr_t pHookFn = ResolveExport(hp, asiBase, asiSize, "HookedEndScene");
    uintptr_t pSetOrig = ResolveExport(hp, asiBase, asiSize, "SetOrigEndScene");
    L("HookedEndScene = 0x%08X", (unsigned)pHookFn);
    L("SetOrigEndScene = 0x%08X", (unsigned)pSetOrig);
    if (!pHookFn || !pSetOrig) { L("Missing exports"); return 1; }
    
    /* Scan game for RAGE wrapper vtable */
    BYTE* img = (BYTE*)malloc(gameSize);
    for (size_t off = 0; off < gameSize; off += 4096) {
        size_t chunk = (off + 4096 <= gameSize) ? 4096 : gameSize - off;
        ReadProcessMemory(hp, (LPCVOID)(gameBase + off), img + off, chunk, NULL);
    }
    
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
                    /* Check vt[42] is non-null */
                    uintptr_t es = 0;
                    ReadProcessMemory(hp, (LPCVOID)(vt + 42*4), &es, 4, NULL);
                    if (es > 0x10000) {
                        L("Found wrapper: dev=0x%08X vt=0x%08X vt[42]=0x%08X",
                            (unsigned)dev, (unsigned)vt, (unsigned)es);
                        wrapperVt = vt;
                        wrapperDev = dev;
                        break;
                    }
                }
            }
        }
    }
    free(img);
    
    if (!wrapperVt) { L("Wrapper not found"); return 1; }
    
    /* Read original EndScene */
    uintptr_t origEndScene = 0;
    ReadProcessMemory(hp, (LPCVOID)(wrapperVt + 42*4), &origEndScene, 4, NULL);
    L("Original EndScene: 0x%08X", (unsigned)origEndScene);
    
    /* Set the original in the ASI via CreateRemoteThread on SetOrigEndScene(origEndScene) */
    L("Calling SetOrigEndScene(0x%08X) in remote process...", (unsigned)origEndScene);
    HANDLE hThread = CreateRemoteThread(hp, NULL, 0, 
        (LPTHREAD_START_ROUTINE)pSetOrig, (LPVOID)origEndScene, 0, NULL);
    if (!hThread) { L("CreateRemoteThread failed: %lu", GetLastError()); return 1; }
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    L("SetOrigEndScene done");
    
    /* Patch vtable[42] to point to HookedEndScene */
    DWORD oldProt;
    if (!VirtualProtectEx(hp, (LPVOID)(wrapperVt + 42*4), 4, PAGE_READWRITE, &oldProt)) {
        L("VirtualProtectEx failed: %lu", GetLastError());
        return 1;
    }
    SIZE_T written = 0;
    WriteProcessMemory(hp, (LPVOID)(wrapperVt + 42*4), &pHookFn, 4, &written);
    VirtualProtectEx(hp, (LPVOID)(wrapperVt + 42*4), 4, oldProt, &oldProt);
    L("Wrote HookedEndScene to vt[42]: %zu bytes", written);
    
    /* Verify */
    uintptr_t verify = 0;
    ReadProcessMemory(hp, (LPCVOID)(wrapperVt + 42*4), &verify, 4, NULL);
    L("Verify vt[42] = 0x%08X (should be 0x%08X)", (unsigned)verify, (unsigned)pHookFn);
    
    L("=== Hook installed live! ===");
    CloseHandle(hp);
    fclose(g_out);
    return 0;
}
