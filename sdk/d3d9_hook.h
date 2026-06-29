/*
 * sdk/d3d9_hook.h - D3D9 EndScene/Reset hook for GTA IV CE
 *
 * GTA IV's renderer uses a RAGE D3D9 wrapper whose vtable lives inside
 * GTAIV.exe (not inside d3d9.dll). FusionFix's d3d9.dll is a thin proxy
 * that forwards to DXVK (vulkan.dll). The legacy approach of hooking the
 * d3d9.dll vtable doesn't work because DXVK builds per-device vtables.
 *
 * We scan GTAIV.exe for the device-pointer access pattern, validate the
 * candidate by checking that its vtable lives inside the game image, then
 * patch vtable[42] (EndScene) and vtable[16] (Reset) on THAT device.
 *
 * Pattern (FusionFix-style):
 *   A1 ?? ?? ?? ?? 50 8B 08 FF 51 ??
 *   = mov eax, [g_pDevice]; push eax; mov ecx, [eax]; call [ecx + ??]
 *
 * Usage:
 *   d3d9hook::SetCallback(MyRenderFunc);
 *   d3d9hook::SetLostCallback(MyDeviceLostFunc);
 *   d3d9hook::Install(gameBase, gameSize);
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "logging.h"
#include "memory.h"
#include "scanner.h"

namespace d3d9hook {

typedef void (*RenderCallback)(IDirect3DDevice9* pDevice);
typedef void (*LostCallback)();

inline RenderCallback g_callback     = nullptr;
inline LostCallback   g_lostCallback = nullptr;

namespace detail {
    typedef HRESULT (__stdcall *EndScene_t)(IDirect3DDevice9*);
    typedef HRESULT (__stdcall *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT (__stdcall *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

    inline EndScene_t g_origEndScene = nullptr;
    inline Reset_t    g_origReset    = nullptr;
    inline Present_t  g_origPresent  = nullptr;
    inline bool       g_hookPresent  = false;
    inline IDirect3DDevice9** g_ppDevice = nullptr;

    extern "C" __declspec(dllexport) HRESULT __stdcall HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        if (g_origEndScene) return g_origEndScene(pDev);
        return S_OK;
    }

    extern "C" __declspec(dllexport) HRESULT __stdcall HookedPresent(
            IDirect3DDevice9* pDev, const RECT* src, const RECT* dst,
            HWND hWnd, const RGNDATA* dirty) {
        if (g_callback) g_callback(pDev);
        if (g_origPresent) return g_origPresent(pDev, src, dst, hWnd, dirty);
        return S_OK;
    }

    extern "C" __declspec(dllexport) HRESULT __stdcall HookedReset(
            IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pp) {
        if (g_lostCallback) g_lostCallback();
        if (g_origReset) return g_origReset(pDev, pp);
        return S_OK;
    }
}

inline void SetCallback(RenderCallback cb)     { g_callback = cb; }
inline void SetLostCallback(LostCallback cb)   { g_lostCallback = cb; }

inline bool Install(uintptr_t gameBase, size_t gameSize) {
    sdk::Log("d3d9hook: Scanning for game's RAGE D3D9 wrapper...");

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
            if (sdk::IsValidPtr(devPtr) && sdk::IsReadable((void*)devPtr, 4)) {
                uintptr_t vtPtr = *(uintptr_t*)devPtr;
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
                            foundVt  = (void**)vtPtr;
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
        sdk::LogError("d3d9hook: RAGE wrapper not found");
        return false;
    }

    int hookIdx = 42;  /* EndScene */
    sdk::Log("d3d9hook: Hooking vtable[%d] (EndScene)", hookIdx);

    DWORD oldProt;
    if (!VirtualProtect(&foundVt[hookIdx], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        sdk::LogError("d3d9hook: VirtualProtect failed");
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

    /* Reset (vtable[16]) -- DXVK triggers this on alt+tab and resolution change */
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
