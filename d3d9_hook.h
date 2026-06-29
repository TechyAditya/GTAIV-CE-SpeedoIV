/*
 * d3d9_hook.h - DXVK-compatible D3D9 EndScene hook for GTA IV CE
 *
 * Uses an inline JMP detour on the EndScene function itself (not vtable
 * patching), which works regardless of how many device instances DXVK creates.
 *
 * Usage:
 *   d3d9hook::SetCallback(MyRenderFunc);
 *   d3d9hook::Install(hModule);  // call from init thread after game loads
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "gtaiv_sdk.h"

namespace d3d9hook {

typedef void (*RenderCallback)(IDirect3DDevice9* pDevice);

inline RenderCallback g_callback = nullptr;
inline HMODULE        g_hModule  = NULL;

namespace detail {
    typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);

    /* Trampoline: stores original bytes + JMP back */
    inline BYTE    g_trampoline[32] = {};
    inline BYTE    g_origBytes[8]   = {};
    inline int     g_hookLen        = 0;
    inline void*   g_endSceneAddr   = nullptr;

    inline EndScene_t GetTrampoline() {
        return (EndScene_t)g_trampoline;
    }

    inline HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        return GetTrampoline()(pDev);
    }

    /* Write a 5-byte JMP rel32 at 'from' to 'to' */
    inline bool WriteJmp(void* from, void* to, int len) {
        DWORD old;
        if (!VirtualProtect(from, len, PAGE_EXECUTE_READWRITE, &old))
            return false;
        /* Save original bytes */
        memcpy(g_origBytes, from, len);
        g_hookLen = len;
        /* Write JMP */
        BYTE* p = (BYTE*)from;
        p[0] = 0xE9; /* JMP rel32 */
        *(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (uintptr_t)from - 5);
        /* NOP remaining bytes */
        for (int i = 5; i < len; i++) p[i] = 0x90;
        VirtualProtect(from, len, old, &old);
        return true;
    }

    /* Build trampoline: original bytes + JMP back to (endscene + len) */
    inline void BuildTrampoline(void* endScene, int len) {
        DWORD old;
        VirtualProtect(g_trampoline, 32, PAGE_EXECUTE_READWRITE, &old);
        /* Copy original instruction bytes */
        memcpy(g_trampoline, endScene, len);
        /* JMP back to original code after our hook */
        g_trampoline[len] = 0xE9;
        *(int32_t*)(g_trampoline + len + 1) =
            (int32_t)((uintptr_t)endScene + len - (uintptr_t)(g_trampoline + len + 5));
        VirtualProtect(g_trampoline, 32, old, &old);
    }

    /* Determine minimum instruction length >= 5 bytes for safe hooking.
     * Simple heuristic for common x86 prologues. */
    inline int GetHookLength(BYTE* fn) {
        /* Common DXVK/GCC function prologues:
         * push ebp; mov ebp,esp = 55 8B EC (3 bytes) -- need more
         * push ebp; mov ebp,esp; push reg = 55 8B EC 5x (4 bytes) -- need more  
         * push ebp; mov ebp,esp; sub esp,X = 55 8B EC 83 EC XX (6 bytes) -- OK
         * push ebp; mov ebp,esp; push esi = 55 8B EC 56 (4+) 
         * sub esp, imm32 = 81 EC XX XX XX XX (6 bytes) -- OK
         * mov edi, edi; push ebp; mov ebp,esp = 8B FF 55 8B EC (5 bytes) -- OK
         * push reg; push reg; push reg; ... common in MSVC/GCC
         */

        /* Just scan for instruction boundaries. 
         * Conservative: if first byte is 0x55 (push ebp), common prologue. */
        if (fn[0] == 0x55 && fn[1] == 0x8B && fn[2] == 0xEC) {
            /* push ebp; mov ebp,esp; -- 3 bytes, need at least 2 more */
            if (fn[3] == 0x83 && fn[4] == 0xEC) return 6; /* sub esp, imm8 */
            if (fn[3] == 0x81 && fn[4] == 0xEC) return 9; /* sub esp, imm32 */
            if ((fn[3] & 0xF8) == 0x50) return 4; /* push reg (1 byte) -- still < 5 */
            if (fn[3] == 0x56 || fn[3] == 0x57 || fn[3] == 0x53) {
                if ((fn[4] & 0xF8) == 0x50 || fn[4] == 0x56 || fn[4] == 0x57) return 5;
                return 5; /* push ebp; mov ebp,esp; push esi + next = 5 min */
            }
            return 6; /* safe default for this pattern */
        }
        if (fn[0] == 0x8B && fn[1] == 0xFF) return 5; /* mov edi,edi; ... */
        if (fn[0] == 0x6A) return 7; /* push imm8 + more */
        /* Generic: assume 5 is safe (risky but common) */
        return 5;
    }
}

inline void SetCallback(RenderCallback cb) { g_callback = cb; }

inline bool Install(HMODULE hModule) {
    g_hModule = hModule;

    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) { sdk::Log("d3d9hook: No d3d9.dll"); return false; }

    /* Create a temp device to find EndScene's address */
    typedef IDirect3D9* (WINAPI *Create9_t)(UINT);
    Create9_t pCreate9 = (Create9_t)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pCreate9) { sdk::Log("d3d9hook: No Direct3DCreate9"); return false; }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hModule; wc.lpszClassName = "D3D9Hook_Tmp";
    RegisterClassExA(&wc);
    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                0, 0, 4, 4, NULL, NULL, hModule, NULL);
    if (!hWnd) { sdk::Log("d3d9hook: No window"); return false; }

    IDirect3D9* pD3D = pCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        sdk::Log("d3d9hook: Create9 NULL");
        DestroyWindow(hWnd); UnregisterClassA(wc.lpszClassName, hModule);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hWnd; pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* pDev = NULL;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDev);
    if (FAILED(hr) || !pDev) {
        sdk::Log("d3d9hook: CreateDevice failed 0x%08X", (unsigned)hr);
        pD3D->Release(); DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, hModule);
        return false;
    }

    /* Get the REAL EndScene function pointer from vtable */
    void** vt = *(void***)pDev;
    detail::g_endSceneAddr = vt[42];
    sdk::Log("d3d9hook: EndScene func at 0x%08X (first bytes: %02X %02X %02X %02X %02X %02X)",
             (unsigned)(uintptr_t)detail::g_endSceneAddr,
             ((BYTE*)detail::g_endSceneAddr)[0], ((BYTE*)detail::g_endSceneAddr)[1],
             ((BYTE*)detail::g_endSceneAddr)[2], ((BYTE*)detail::g_endSceneAddr)[3],
             ((BYTE*)detail::g_endSceneAddr)[4], ((BYTE*)detail::g_endSceneAddr)[5]);

    /* Release temp device -- we only needed the function address */
    pDev->Release();
    pD3D->Release();
    DestroyWindow(hWnd);
    UnregisterClassA(wc.lpszClassName, hModule);

    /* Now install inline JMP hook on the actual EndScene function */
    int hookLen = detail::GetHookLength((BYTE*)detail::g_endSceneAddr);
    sdk::Log("d3d9hook: hookLen=%d", hookLen);

    detail::BuildTrampoline(detail::g_endSceneAddr, hookLen);
    if (!detail::WriteJmp(detail::g_endSceneAddr, (void*)detail::HookedEndScene, hookLen)) {
        sdk::Log("d3d9hook: WriteJmp failed");
        return false;
    }

    sdk::Log("d3d9hook: Inline hook installed OK");
    return true;
}

} // namespace d3d9hook
