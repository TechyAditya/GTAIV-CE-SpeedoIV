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
    typedef HRESULT (__stdcall *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (__stdcall *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    inline EndScene_t g_origEndScene = nullptr;
    inline Reset_t    g_origReset    = nullptr;
    inline Present_t  g_origPresent  = nullptr;
    inline bool g_hookPresent = false;  /* true if we hooked Present instead of EndScene */
    inline IDirect3DDevice9** g_ppDevice = nullptr;

    /* User callback for device-lost notification */
    typedef void (*LostCallback)();
    inline LostCallback g_lostCallback = nullptr;

    /* EndScene hook */
    extern "C" __declspec(dllexport) HRESULT __stdcall HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        if (g_origEndScene) return g_origEndScene(pDev);
        return S_OK;
    }

    /* Present hook (5 params) */
    extern "C" __declspec(dllexport) HRESULT __stdcall HookedPresent(
            IDirect3DDevice9* pDev, const RECT* src, const RECT* dst,
            HWND hWnd, const RGNDATA* dirty) {
        if (g_callback) g_callback(pDev);
        if (g_origPresent) return g_origPresent(pDev, src, dst, hWnd, dirty);
        return S_OK;
    }

    /* Reset hook - release resources */
    extern "C" __declspec(dllexport) HRESULT __stdcall HookedReset(
            IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pp) {
        if (g_lostCallback) g_lostCallback();
        if (g_origReset) return g_origReset(pDev, pp);
        return S_OK;
    }

    extern "C" __declspec(dllexport) void SetOrigEndScene(void* fn) {
        g_origEndScene = (EndScene_t)fn;
    }
}

inline void SetCallback(RenderCallback cb) { g_callback = cb; }
inline void SetLostCallback(detail::LostCallback cb) { detail::g_lostCallback = cb; }

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

    /* Hook EndScene -- fires every frame on the game's RAGE wrapper */
    int hookIdx = 42; /* EndScene */
    sdk::Log("d3d9hook: Hooking vtable[%d] (EndScene)", hookIdx);

    DWORD oldProt;
    if (!VirtualProtect(&foundVt[hookIdx], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        sdk::Log("d3d9hook: VirtualProtect failed");
        return false;
    }
    if (hookIdx == 17) {
        detail::g_hookPresent = true;
        detail::g_origPresent = (detail::Present_t)foundVt[hookIdx];
        foundVt[hookIdx] = (void*)detail::HookedPresent;
    } else {
        detail::g_origEndScene = (detail::EndScene_t)foundVt[hookIdx];
        foundVt[hookIdx] = (void*)detail::HookedEndScene;
    }
    VirtualProtect(&foundVt[hookIdx], 4, oldProt, &oldProt);

    /* Hook Reset (vtable[16]) */
    if (VirtualProtect(&foundVt[16], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        detail::g_origReset = (detail::Reset_t)foundVt[16];
        foundVt[16] = (void*)detail::HookedReset;
        VirtualProtect(&foundVt[16], 4, oldProt, &oldProt);
        sdk::Log("d3d9hook: Hooked Reset orig=0x%08X", (unsigned)(uintptr_t)detail::g_origReset);
    }

    sdk::Log("d3d9hook: Hook installed");
    return true;
}

} // namespace d3d9hook
