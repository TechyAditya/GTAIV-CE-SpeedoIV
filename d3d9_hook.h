/*
 * d3d9_hook.h - D3D9 EndScene hook for GTA IV CE + FusionFix/DXVK
 *
 * Finds the game's actual IDirect3DDevice9* via the same pattern FusionFix
 * uses, then hooks EndScene on THAT device's vtable.
 *
 * FusionFix's d3d9.dll is a thin proxy that forwards to DXVK (vulkan.dll).
 * The game stores its device pointer in a global -- we find it by scanning
 * for the code pattern that accesses it.
 *
 * Usage:
 *   d3d9hook::SetCallback(MyRenderFunc);
 *   d3d9hook::Install(gameBase, gameSize);
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "gtaiv_sdk.h"

namespace d3d9hook {

typedef void (*RenderCallback)(IDirect3DDevice9* pDevice);
inline RenderCallback g_callback = nullptr;

namespace detail {
    typedef HRESULT (__stdcall *EndScene_t)(IDirect3DDevice9*);
    inline EndScene_t g_origEndScene = nullptr;
    inline IDirect3DDevice9** g_ppDevice = nullptr; /* ptr to game's device ptr */

    inline HRESULT __stdcall HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        return g_origEndScene(pDev);
    }
}

inline void SetCallback(RenderCallback cb) { g_callback = cb; }

/*
 * Find the game's device pointer and hook EndScene on its vtable.
 * Must be called after the game has initialized D3D9.
 */
inline bool Install(uintptr_t gameBase, size_t gameSize) {
    /*
     * FusionFix finds the device pointer with this pattern:
     *   A1 ?? ?? ?? ?? 50 8B 08 FF 51 40
     *   mov eax, [g_pDevice]    ; A1 XX XX XX XX
     *   push eax                ; 50
     *   mov ecx, [eax]          ; 8B 08 (read vtable)
     *   call [ecx+0x40]         ; FF 51 40 (vtable[16] = Reset)
     *
     * We also try a variant for EndScene:
     *   A1 ?? ?? ?? ?? 50 8B 08 FF 91 A8 00 00 00
     *   call [ecx+0xA8] = vtable[42] = EndScene
     *
     * And a simpler one:
     *   A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??
     */

    sdk::Log("d3d9hook: Scanning for device pointer pattern...");

    /* Pattern 1: A1 ?? ?? ?? ?? 50 8B 08 FF 51 40 (Reset call) */
    {
        BYTE pat[] = {0xA1,0,0,0,0, 0x50, 0x8B,0x08, 0xFF,0x51,0x40};
        char msk[] = "x????xxxxxx";
        uintptr_t hit = sdk::FindPattern(gameBase, gameSize, pat, msk);
        if (hit) {
            uintptr_t ptrAddr = *(uintptr_t*)(hit + 1);
            sdk::Log("d3d9hook: Pattern1 hit at +0x%X, device** = 0x%08X",
                     (unsigned)(hit - gameBase), (unsigned)ptrAddr);
            if (sdk::IsValidPtr(ptrAddr) && sdk::IsReadable((void*)ptrAddr, 4)) {
                detail::g_ppDevice = (IDirect3DDevice9**)ptrAddr;
            }
        }
    }

    /* Pattern 2: A1 ?? ?? ?? ?? 50 8B 08 FF 91 A8 00 00 00 (EndScene call) */
    if (!detail::g_ppDevice) {
        BYTE pat[] = {0xA1,0,0,0,0, 0x50, 0x8B,0x08, 0xFF,0x91, 0xA8,0x00,0x00,0x00};
        char msk[] = "x????xxxxxxxxx";
        uintptr_t hit = sdk::FindPattern(gameBase, gameSize, pat, msk);
        if (hit) {
            uintptr_t ptrAddr = *(uintptr_t*)(hit + 1);
            sdk::Log("d3d9hook: Pattern2 hit at +0x%X, device** = 0x%08X",
                     (unsigned)(hit - gameBase), (unsigned)ptrAddr);
            if (sdk::IsValidPtr(ptrAddr) && sdk::IsReadable((void*)ptrAddr, 4)) {
                detail::g_ppDevice = (IDirect3DDevice9**)ptrAddr;
            }
        }
    }

    /* Pattern 3: generic A1 ?? ?? ?? ?? 50 8B 08 FF 51 ?? */
    if (!detail::g_ppDevice) {
        BYTE pat[] = {0xA1,0,0,0,0, 0x50, 0x8B,0x08, 0xFF,0x51,0};
        char msk[] = "x????xxxxx?";
        uintptr_t cur = gameBase;
        size_t rem = gameSize;
        for (int n = 0; n < 20; n++) {
            uintptr_t hit = sdk::FindPattern(cur, rem, pat, msk);
            if (!hit) break;
            uintptr_t ptrAddr = *(uintptr_t*)(hit + 1);
            if (sdk::IsValidPtr(ptrAddr) && sdk::IsReadable((void*)ptrAddr, 4)) {
                IDirect3DDevice9* dev = *(IDirect3DDevice9**)ptrAddr;
                if (dev && sdk::IsHeapPtr((uintptr_t)dev) &&
                    sdk::IsReadable((void*)dev, 4)) {
                    sdk::Log("d3d9hook: Pattern3 hit at +0x%X, device** = 0x%08X -> dev=0x%08X",
                             (unsigned)(hit - gameBase), (unsigned)ptrAddr, (unsigned)(uintptr_t)dev);
                    detail::g_ppDevice = (IDirect3DDevice9**)ptrAddr;
                    break;
                }
            }
            cur = hit + 1;
            rem = gameBase + gameSize - cur;
        }
    }

    if (!detail::g_ppDevice) {
        sdk::Log("d3d9hook: Device pointer pattern not found");
        return false;
    }

    /* Read the actual device pointer */
    IDirect3DDevice9* pDev = *detail::g_ppDevice;
    if (!pDev || !sdk::IsReadable((void*)pDev, 4)) {
        sdk::Log("d3d9hook: Device pointer is NULL or invalid (0x%08X)",
                 (unsigned)(uintptr_t)pDev);
        return false;
    }

    /* Get its vtable */
    void** vt = *(void***)pDev;
    sdk::Log("d3d9hook: Device=0x%08X vtable=0x%08X EndScene=0x%08X",
             (unsigned)(uintptr_t)pDev, (unsigned)(uintptr_t)vt,
             (unsigned)(uintptr_t)vt[42]);

    /* Patch vtable[42] = EndScene */
    DWORD oldProt;
    if (!VirtualProtect(&vt[42], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        sdk::Log("d3d9hook: VirtualProtect failed");
        return false;
    }
    detail::g_origEndScene = (detail::EndScene_t)vt[42];
    vt[42] = (void*)detail::HookedEndScene;
    VirtualProtect(&vt[42], 4, oldProt, &oldProt);

    sdk::Log("d3d9hook: Hooked EndScene! orig=0x%08X", (unsigned)(uintptr_t)detail::g_origEndScene);
    return true;
}

} // namespace d3d9hook
