#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>

static DWORD FindPid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do { if (_stricmp(pe.szExeFile, name) == 0) { CloseHandle(snap); return pe.th32ProcessID; } } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap); return 0;
}
int main() {
    DWORD pid = FindPid("GTAIV.exe");
    if (!pid) { printf("not running\n"); return 1; }
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    /* Restore wrapper vt[42] = 0x01001770 */
    uintptr_t orig = 0x01001770;
    uintptr_t vtSlot = 0x01BAA294 + 42*4;
    DWORD oldP;
    VirtualProtectEx(hp, (LPVOID)vtSlot, 4, PAGE_READWRITE, &oldP);
    WriteProcessMemory(hp, (LPVOID)vtSlot, &orig, 4, NULL);
    VirtualProtectEx(hp, (LPVOID)vtSlot, 4, oldP, &oldP);
    uintptr_t v = 0;
    ReadProcessMemory(hp, (LPCVOID)vtSlot, &v, 4, NULL);
    printf("Restored vt[42] = 0x%08X\n", (unsigned)v);
    CloseHandle(hp);
    return 0;
}
