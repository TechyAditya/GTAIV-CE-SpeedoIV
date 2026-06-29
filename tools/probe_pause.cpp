/* probe_pause.cpp - find m_UserPause + m_CodePause globals and read them live */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

static HANDLE g_hp = 0;
static uintptr_t g_base = 0;
static size_t g_size = 0;

static bool R(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_hp, (LPCVOID)a, out, n, &got) && got == n;
}

int main() {
    FILE* f = fopen("E:\\Code\\gta4\\speedometer\\build\\probe_pause.txt", "w");
    if (!f) return 1;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) do {
        if (_stricmp(pe.szExeFile, "GTAIV.exe") == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (!pid) { fprintf(f, "GTAIV not running\n"); fclose(f); return 1; }

    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    if (Module32First(ms, &me)) do {
        if (_stricmp(me.szModule, "GTAIV.exe") == 0) {
            g_base = (uintptr_t)me.modBaseAddr; g_size = me.modBaseSize; break;
        }
    } while (Module32Next(ms, &me));
    CloseHandle(ms);

    g_hp = OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_hp) { fprintf(f, "OpenProcess err=%lu\n", GetLastError()); fclose(f); return 1; }

    BYTE* img = (BYTE*)malloc(g_size);
    for (size_t off = 0; off < g_size; off += 4096) {
        size_t chunk = (off + 4096 <= g_size) ? 4096 : g_size - off;
        SIZE_T g = 0;
        ReadProcessMemory(g_hp, (LPCVOID)(g_base + off), img + off, chunk, &g);
    }

    auto find = [&](const char* name, const BYTE* pat, const char* mask, size_t plen) -> uintptr_t {
        for (size_t i = 0; i + plen <= g_size; i++) {
            bool ok = true;
            for (size_t j = 0; j < plen; j++)
                if (mask[j] == 'x' && img[i+j] != pat[j]) { ok = false; break; }
            if (ok) {
                fprintf(f, "%s @ +0x%X (0x%08X)\n", name, (unsigned)i, (unsigned)(g_base+i));
                return g_base + i;
            }
        }
        fprintf(f, "%s NOT FOUND\n", name); return 0;
    };

    /* m_UserPause/m_CodePause: 0A 05 ?? ?? ?? ?? 0A 05 ?? ?? ?? ?? 75 38 */
    const BYTE patP[] = {0x0A,0x05,0,0,0,0,0x0A,0x05,0,0,0,0,0x75,0x38};
    uintptr_t hit = find("UserPause+CodePause pattern", patP, "xx????xx????xx", 14);
    if (hit) {
        uintptr_t userPauseAddr = *(uintptr_t*)(img + (hit - g_base) + 2);
        uintptr_t codePauseAddr = *(uintptr_t*)(img + (hit - g_base) + 8);
        fprintf(f, "  m_UserPause = 0x%08X\n", (unsigned)userPauseAddr);
        fprintf(f, "  m_CodePause = 0x%08X\n", (unsigned)codePauseAddr);
        BYTE u=0xFF, c=0xFF;
        R(userPauseAddr, &u, 1);
        R(codePauseAddr, &c, 1);
        fprintf(f, "  *m_UserPause = %d\n", u);
        fprintf(f, "  *m_CodePause = %d\n", c);
        fprintf(f, "  paused = %s\n", (u || c) ? "YES" : "NO");
    }

    /* Try fallback pattern */
    const BYTE patPb[] = {0x0A,0x05,0,0,0,0,0x0A,0x05};
    uintptr_t hit2 = find("UserPause+CodePause fallback", patPb, "xx????xx", 8);
    if (hit2 && hit2 != hit) {
        uintptr_t u2 = *(uintptr_t*)(img + (hit2 - g_base) + 2);
        uintptr_t c2 = *(uintptr_t*)(img + (hit2 - g_base) + 8);
        fprintf(f, "  fallback -> m_UserPause = 0x%08X  m_CodePause = 0x%08X\n", (unsigned)u2, (unsigned)c2);
    }

    /* m_MenuActive: 80 3D ?? ?? ?? ?? ?? 74 4B E8 ?? ?? ?? ?? 84 C0 */
    const BYTE patM[] = {0x80,0x3D,0,0,0,0,0,0x74,0x4B,0xE8,0,0,0,0,0x84,0xC0};
    uintptr_t hitM = find("m_MenuActive pattern", patM, "xx?????xxx????xx", 16);
    if (hitM) {
        uintptr_t a = *(uintptr_t*)(img + (hitM - g_base) + 2);
        fprintf(f, "  m_MenuActive = 0x%08X\n", (unsigned)a);
        BYTE v=0xFF; R(a, &v, 1);
        fprintf(f, "  *m_MenuActive = %d (%s)\n", v, v ? "menu open" : "no menu");
    }

    free(img);
    CloseHandle(g_hp);
    fclose(f);
    fprintf(stderr, "Done. Output: build\\probe_pause.txt\n");
    return 0;
}
