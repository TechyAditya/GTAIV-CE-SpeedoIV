/* find_real_factory.cpp - scan the game like the SDK would, see if we'd find 0x01D3A46C */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

int main() {
    FILE* f = fopen("E:\\Code\\gta4\\speedometer\\build\\find_real.txt", "w");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) do { if (_stricmp(pe.szExeFile, "GTAIV.exe") == 0) { pid = pe.th32ProcessID; break; } } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { fprintf(f, "Not running\n"); fclose(f); return 1; }
    
    HANDLE hmodSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    uintptr_t gameBase = 0; size_t gameSize = 0;
    if (Module32First(hmodSnap, &me)) do { if (_stricmp(me.szModule, "GTAIV.exe") == 0) { gameBase = (uintptr_t)me.modBaseAddr; gameSize = me.modBaseSize; break; } } while (Module32Next(hmodSnap, &me));
    CloseHandle(hmodSnap);
    
    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    BYTE* img = (BYTE*)malloc(gameSize);
    for (size_t off = 0; off < gameSize; off += 4096) {
        size_t chunk = (off + 4096 <= gameSize) ? 4096 : gameSize - off;
        ReadProcessMemory(hp, (LPCVOID)(gameBase + off), img + off, chunk, NULL);
    }
    
    /* Find hits at +0x488D16 area */
    fprintf(f, "Searching for pattern around +0x488D16...\n");
    for (size_t i = 0x488000; i < 0x488F00 && i < gameSize - 11; i++) {
        if (img[i] == 0xA1) {
            fprintf(f, "+0x%X: A1 %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                (unsigned)i, img[i+1], img[i+2], img[i+3], img[i+4], img[i+5],
                img[i+6], img[i+7], img[i+8], img[i+9], img[i+10]);
        }
    }
    
    /* Specifically check what's at +0x488D16 */
    fprintf(f, "\nBytes at +0x488D16: ");
    for (int k = 0; k < 16; k++) fprintf(f, "%02X ", img[0x488D16 + k]);
    fprintf(f, "\n");
    
    uintptr_t target = *(uintptr_t*)(img + 0x488D16 + 1);
    fprintf(f, "Pattern at +0x488D16 -> ptr 0x%08X (expected 0x01D3A46C)\n", (unsigned)target);
    
    /* Count A1...50 8B 08 FF 51 ?? hits */
    int count = 0;
    fprintf(f, "\nAll matches for 'A1 ? ? ? ? 50 8B 08 FF 51 ?':\n");
    for (size_t i = 0; i <= gameSize - 11; i++) {
        if (img[i] == 0xA1 && img[i+5] == 0x50 && img[i+6] == 0x8B && img[i+7] == 0x08 &&
            img[i+8] == 0xFF && img[i+9] == 0x51) {
            count++;
            if (count <= 50 || (i >= 0x488000 && i < 0x489000)) {
                uintptr_t pp = *(uintptr_t*)(img + i + 1);
                fprintf(f, "  #%d +0x%06X global=0x%08X vt=0x%02X\n", count, (unsigned)i, (unsigned)pp, img[i+10]);
            }
        }
    }
    fprintf(f, "\nTotal matches: %d\n", count);
    
    free(img);
    CloseHandle(hp);
    fclose(f);
    return 0;
}
