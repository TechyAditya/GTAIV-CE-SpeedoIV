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
    inline IDirect3DDevice9** g_ppDevice = nullptr;

    /* Exported so external patchers can find this function by name */
    extern "C" __declspec(dllexport) HRESULT __stdcall HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        if (g_origEndScene) return g_origEndScene(pDev);
        return S_OK;
    }

    /* Exported setter so external patcher can install the original function pointer */
    extern "C" __declspec(dllexport) void SetOrigEndScene(void* fn) {
        g_origEndScene = (EndScene_t)fn;
    }
}

inline void SetCallback(RenderCallback cb) { g_callback = cb; }

/*
 * Find the game's device pointer and hook EndScene on its vtable.
 *
 * GTA IV uses its OWN D3D9 wrapper (rage::grcDevice) with a custom vtable
 * that lives inside GTAIV.exe itself. We must find THIS wrapper, not the
 * real DXVK device.
 *
 * Strategy: scan for the device-pointer pattern and select the candidate
 * whose vtable is inside the game's image (not in d3d9.dll/vulkan.dll).
 */
inline bool Install(uintptr_t gameBase, size_t gameSize) {
    sdk::Log("d3d9hook: Scanning for game's RAGE D3D9 wrapper...");

    /* Pattern: A1 ?? ?? ?? ?? 50 8B 08 FF 51 ?? */
    BYTE pat[] = {0xA1,0,0,0,0, 0x50, 0x8B,0x08, 0xFF,0x51,0};
    char msk[] = "x????xxxxx?";

    uintptr_t scanCur = gameBase;
    size_t scanRem = gameSize;
    IDirect3DDevice9* foundDev = nullptr;
    void** foundVt = nullptr;

    for (int n = 0; n < 50; n++) {
        uintptr_t hit = sdk::FindPattern(scanCur, scanRem, pat, msk);
        if (!hit) break;

        uintptr_t ptrAddr = *(uintptr_t*)(hit + 1);
        if (sdk::IsValidPtr(ptrAddr) && sdk::IsReadable((void*)ptrAddr, 4)) {
            uintptr_t devPtr = *(uintptr_t*)ptrAddr;
            /* Note: the RAGE wrapper device lives in low memory (~0x024BC2B8),
             * NOT necessarily heap range. Use IsValidPtr instead. */
            if (sdk::IsValidPtr(devPtr) && sdk::IsReadable((void*)devPtr, 4)) {
                uintptr_t vtPtr = *(uintptr_t*)devPtr;
                /* Check if vtable is INSIDE GTAIV.exe (the RAGE wrapper) */
                if (vtPtr >= gameBase && vtPtr < gameBase + gameSize) {
                    if (sdk::IsReadable((void*)vtPtr, 50 * 4)) {
                        uintptr_t es = ((uintptr_t*)vtPtr)[42];
                        uintptr_t pr = ((uintptr_t*)vtPtr)[17];
                        if (es && pr && sdk::IsValidPtr(es) && sdk::IsValidPtr(pr)) {
                            sdk::Log("d3d9hook: FOUND RAGE wrapper at +0x%X: global=0x%08X dev=0x%08X vt=0x%08X (in game)",
                                     (unsigned)(hit - gameBase), (unsigned)ptrAddr,
                                     (unsigned)devPtr, (unsigned)vtPtr);
                            sdk::Log("  vt[17]Present=0x%08X vt[42]EndScene=0x%08X",
                                     (unsigned)pr, (unsigned)es);
                            foundDev = (IDirect3DDevice9*)devPtr;
                            foundVt = (void**)vtPtr;
                            detail::g_ppDevice = (IDirect3DDevice9**)ptrAddr;
                            break;
                        }
                    }
                }
            }
        }
        scanCur = hit + 1;
        scanRem = gameBase + gameSize - scanCur;
    }

    if (!foundDev) {
        sdk::Log("d3d9hook: RAGE wrapper not found");
        return false;
    }

    /* Hook EndScene (vt[42]) on the wrapper. When the game's render code
     * calls EndScene through the wrapper, our hook fires. The wrapper then
     * delegates to the real DXVK device. */
    DWORD oldProt;
    if (!VirtualProtect(&foundVt[42], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        sdk::Log("d3d9hook: VirtualProtect failed");
        return false;
    }
    detail::g_origEndScene = (detail::EndScene_t)foundVt[42];
    foundVt[42] = (void*)detail::HookedEndScene;
    VirtualProtect(&foundVt[42], 4, oldProt, &oldProt);

    sdk::Log("d3d9hook: Hooked wrapper EndScene! orig=0x%08X new=0x%08X",
             (unsigned)(uintptr_t)detail::g_origEndScene,
             (unsigned)(uintptr_t)detail::HookedEndScene);
    return true;
}

} // namespace d3d9hook
