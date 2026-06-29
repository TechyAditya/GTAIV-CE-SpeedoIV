/*
 * d3d9_hook.h - DXVK-compatible D3D9 EndScene hook for GTA IV CE
 *
 * Works with FusionFix's DXVK wrapper by creating a device through the
 * same d3d9.dll and patching the shared vtable.
 *
 * Usage:
 *   d3d9hook::SetCallback(MyEndSceneFunc);
 *   d3d9hook::Install(hModule);  // call from init thread after game loads
 *
 * Your callback receives the game's IDirect3DDevice9* each frame.
 */

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "gtaiv_sdk.h"

namespace d3d9hook {

typedef void (*RenderCallback)(IDirect3DDevice9* pDevice);

inline RenderCallback g_callback     = nullptr;
inline HMODULE        g_hModule      = NULL;

/* Internal state */
namespace detail {
    typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
    inline EndScene_t g_origEndScene = nullptr;

    inline HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDev) {
        if (g_callback) g_callback(pDev);
        return g_origEndScene(pDev);
    }
}

/* Set your per-frame render callback */
inline void SetCallback(RenderCallback cb) { g_callback = cb; }

/* Install the hook. Call from a background thread after game has loaded (~10s).
 * Returns true on success. */
inline bool Install(HMODULE hModule) {
    g_hModule = hModule;

    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        sdk::Log("d3d9hook: No d3d9.dll loaded");
        return false;
    }

    typedef IDirect3D9* (WINAPI *Create9_t)(UINT);
    Create9_t pCreate9 = (Create9_t)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pCreate9) {
        sdk::Log("d3d9hook: Direct3DCreate9 not found");
        return false;
    }

    /* Temp window for device creation */
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hModule;
    wc.lpszClassName = "D3D9Hook_Tmp";
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                0, 0, 4, 4, NULL, NULL, hModule, NULL);
    if (!hWnd) { sdk::Log("d3d9hook: CreateWindow failed"); return false; }

    IDirect3D9* pD3D = pCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        sdk::Log("d3d9hook: Direct3DCreate9 returned NULL");
        DestroyWindow(hWnd); UnregisterClassA(wc.lpszClassName, hModule);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hWnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* pDev = NULL;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                    &pp, &pDev);
    if (FAILED(hr) || !pDev) {
        sdk::Log("d3d9hook: CreateDevice failed (0x%08X)", (unsigned)hr);
        pD3D->Release(); DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, hModule);
        return false;
    }

    /* Patch vtable[42] = EndScene */
    void** vt = *(void***)pDev;
    sdk::Log("d3d9hook: device=0x%08X vt=0x%08X vt[42]=0x%08X",
             (unsigned)(uintptr_t)pDev, (unsigned)(uintptr_t)vt,
             (unsigned)(uintptr_t)vt[42]);

    DWORD oldProt;
    if (!VirtualProtect(&vt[42], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
        sdk::Log("d3d9hook: VirtualProtect failed");
        pDev->Release(); pD3D->Release(); DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, hModule);
        return false;
    }

    detail::g_origEndScene = (detail::EndScene_t)vt[42];
    vt[42] = (void*)detail::HookedEndScene;
    VirtualProtect(&vt[42], 4, oldProt, &oldProt);

    sdk::Log("d3d9hook: Hooked EndScene (orig=0x%08X)",
             (unsigned)(uintptr_t)detail::g_origEndScene);

    /* Cleanup -- vtable is shared in DXVK, hook persists */
    pDev->Release();
    pD3D->Release();
    DestroyWindow(hWnd);
    UnregisterClassA(wc.lpszClassName, hModule);
    return true;
}

} // namespace d3d9hook
