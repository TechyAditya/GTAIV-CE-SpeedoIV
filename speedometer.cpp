/*
 * SpeedoIV-CE - Simple Speedometer for GTA IV Complete Edition (1.2.0.43)
 *
 * A lightweight ASI plugin that displays vehicle speed as a text HUD overlay.
 * Compatible with FusionFix and the Complete Edition.
 *
 * Approach: Hook IDirect3DDevice9::EndScene via vtable to draw text using
 * GDI (no D3DX dependency). Read vehicle speed from game memory using
 * pattern scanning for CE compatibility.
 *
 * Built with MinGW GCC (i686) - no MSVC SEH, uses safe pointer checks.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#include <cstring>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */
static const int   SPEEDO_POS_X_OFFSET = 220;  /* pixels from right edge   */
static const int   SPEEDO_POS_Y_OFFSET = 80;   /* pixels from bottom edge  */
static const int   FONT_SIZE           = 28;
static const char  FONT_NAME[]         = "Consolas";
static const DWORD TEXT_COLOR          = RGB(255, 255, 255);
static const DWORD SHADOW_COLOR        = RGB(0, 0, 0);
static const float KMH_MULTIPLIER     = 3.6f;  /* m/s -> km/h              */

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */
static HMODULE g_hModule      = NULL;
static HFONT   g_hFont        = NULL;
static bool    g_initialized  = false;
static bool    g_showSpeedo   = true;

/* D3D9 hook state */
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static EndScene_t  g_origEndScene = NULL;
static Reset_t     g_origReset    = NULL;

/* Game memory pointers (resolved via pattern scan) */
static uintptr_t g_gameBase    = 0;
static uintptr_t g_gameSize    = 0;

/* --------------------------------------------------------------------------
 * Safe memory reading helpers (GCC-compatible, no SEH)
 * -------------------------------------------------------------------------- */
static inline bool IsReadable(const void* addr, size_t len) {
    return !IsBadReadPtr(addr, len);
}

static inline bool SafeReadPtr(uintptr_t addr, uintptr_t* out) {
    if (!IsReadable((const void*)addr, sizeof(uintptr_t))) return false;
    *out = *(uintptr_t*)addr;
    return true;
}

static inline bool SafeReadFloat(uintptr_t addr, float* out) {
    if (!IsReadable((const void*)addr, sizeof(float))) return false;
    *out = *(float*)addr;
    return true;
}

static inline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFF;
}

/* --------------------------------------------------------------------------
 * Pattern Scanner
 * -------------------------------------------------------------------------- */
static bool PatternMatch(const BYTE* data, const BYTE* pattern,
                         const char* mask, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (mask[i] == 'x' && data[i] != pattern[i])
            return false;
    }
    return true;
}

static uintptr_t FindPattern(uintptr_t base, size_t size,
                             const BYTE* pattern, const char* mask) {
    size_t patLen = strlen(mask);
    if (size < patLen) return 0;
    for (size_t i = 0; i <= size - patLen; i++) {
        if (PatternMatch((const BYTE*)(base + i), pattern, mask, patLen))
            return base + i;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Vehicle Speed Reading
 *
 * Strategy: Find the World / CPedFactory global pointer via pattern scan.
 *   CPedFactory + 0x4  -> local player CPed pointer
 *   CPed + offset      -> current CVehicle pointer (0 if on foot)
 *   CVehicle + offset  -> velocity vector (3 floats: vx, vy, vz)
 *
 * Offsets are probed at runtime since CE 1.2.0.43 differs from legacy.
 * -------------------------------------------------------------------------- */

static const int OFF_PEDFACTORY_PLAYER = 0x4;

static const int VEHICLE_OFFSETS[]  = { 0x32C, 0x330, 0x328, 0x33C, 0x320 };
static const int VELOCITY_OFFSETS[] = { 0x70,  0x80,  0x90,  0x60         };
static const int NUM_VEH_OFFS  = sizeof(VEHICLE_OFFSETS)  / sizeof(int);
static const int NUM_VEL_OFFS  = sizeof(VELOCITY_OFFSETS) / sizeof(int);

static uintptr_t g_pPedFactory  = 0;
static int       g_vehicleOff   = 0;
static int       g_velocityOff  = 0;
static bool      g_offsetsFound = false;

/* Try one candidate CPedFactory global address; return true if it validates */
static bool TryFactory(uintptr_t ptrAddr) {
    if (!IsValidPtr(ptrAddr)) return false;

    uintptr_t factoryPtr = 0;
    if (!SafeReadPtr(ptrAddr, &factoryPtr) || !IsValidPtr(factoryPtr))
        return false;

    uintptr_t pedPtr = 0;
    if (!SafeReadPtr(factoryPtr + OFF_PEDFACTORY_PLAYER, &pedPtr) ||
        !IsValidPtr(pedPtr))
        return false;

    for (int v = 0; v < NUM_VEH_OFFS; v++) {
        uintptr_t vehPtr = 0;
        if (!SafeReadPtr(pedPtr + VEHICLE_OFFSETS[v], &vehPtr))
            continue;
        /* vehPtr == 0 means on foot (valid), or a heap pointer */
        if (vehPtr == 0 || IsValidPtr(vehPtr)) {
            g_pPedFactory  = ptrAddr;
            g_vehicleOff   = VEHICLE_OFFSETS[v];
            g_velocityOff  = 0x70; /* most common in RAGE */
            g_offsetsFound = true;
            return true;
        }
    }
    return false;
}

static bool FindGamePointers() {
    if (g_gameBase == 0) return false;

    /* Pattern 1: A1 XX XX XX XX 85 C0 74
       mov eax,[CPedFactory]; test eax,eax; jz ... */
    {
        BYTE pat[] = { 0xA1,0x00,0x00,0x00,0x00, 0x85,0xC0, 0x74 };
        char msk[] = "x????xxx";
        uintptr_t cur = g_gameBase;
        size_t    rem = g_gameSize;

        for (int n = 0; n < 50 && cur < g_gameBase + g_gameSize - 8; n++) {
            uintptr_t hit = FindPattern(cur, rem, pat, msk);
            if (!hit) break;
            uintptr_t candidate = *(uintptr_t*)(hit + 1);
            if (TryFactory(candidate)) return true;
            cur = hit + 1;
            rem = g_gameBase + g_gameSize - cur;
        }
    }

    /* Pattern 2: 8B 0D XX XX XX XX 8B 41 04
       mov ecx,[CPedFactory]; mov eax,[ecx+4] */
    {
        BYTE pat[] = { 0x8B,0x0D,0x00,0x00,0x00,0x00, 0x8B,0x41,0x04 };
        char msk[] = "xx????xxx";
        uintptr_t cur = g_gameBase;
        size_t    rem = g_gameSize;

        for (int n = 0; n < 50 && cur < g_gameBase + g_gameSize - 10; n++) {
            uintptr_t hit = FindPattern(cur, rem, pat, msk);
            if (!hit) break;
            uintptr_t candidate = *(uintptr_t*)(hit + 2);
            if (TryFactory(candidate)) return true;
            cur = hit + 1;
            rem = g_gameBase + g_gameSize - cur;
        }
    }

    return false;
}

/* Read speed from the resolved pointers; returns -1 on foot / error */
static float GetVehicleSpeed() {
    if (!g_offsetsFound || g_pPedFactory == 0) return -1.0f;

    uintptr_t factoryPtr = 0, pedPtr = 0, vehPtr = 0;
    if (!SafeReadPtr(g_pPedFactory, &factoryPtr) || !IsValidPtr(factoryPtr))
        return -1.0f;
    if (!SafeReadPtr(factoryPtr + OFF_PEDFACTORY_PLAYER, &pedPtr) ||
        !IsValidPtr(pedPtr))
        return -1.0f;
    if (!SafeReadPtr(pedPtr + g_vehicleOff, &vehPtr))
        return -1.0f;
    if (vehPtr == 0) return -1.0f;          /* on foot */
    if (!IsValidPtr(vehPtr)) return -1.0f;

    /* Read velocity vector */
    float vx, vy, vz;
    uintptr_t vBase = vehPtr + g_velocityOff;
    if (!SafeReadFloat(vBase,     &vx) ||
        !SafeReadFloat(vBase + 4, &vy) ||
        !SafeReadFloat(vBase + 8, &vz))
        return -1.0f;

    float speed = sqrtf(vx*vx + vy*vy + vz*vz) * KMH_MULTIPLIER;

    /* Sanity: max ~500 km/h */
    if (speed >= 0.0f && speed <= 500.0f)
        return speed;

    /* Wrong velocity offset -- probe alternatives */
    for (int i = 0; i < NUM_VEL_OFFS; i++) {
        int off = VELOCITY_OFFSETS[i];
        if (off == g_velocityOff) continue;
        vBase = vehPtr + off;
        if (!SafeReadFloat(vBase,     &vx) ||
            !SafeReadFloat(vBase + 4, &vy) ||
            !SafeReadFloat(vBase + 8, &vz))
            continue;
        float s = sqrtf(vx*vx + vy*vy + vz*vz) * KMH_MULTIPLIER;
        if (s >= 0.0f && s <= 500.0f) {
            g_velocityOff = off;
            return s;
        }
    }
    return 0.0f;
}

/* --------------------------------------------------------------------------
 * D3D9 EndScene Hook - Draw speedometer using GDI on the back-buffer
 * -------------------------------------------------------------------------- */
static void CreateFontIfNeeded() {
    if (g_hFont == NULL) {
        g_hFont = CreateFontA(
            FONT_SIZE, 0, 0, 0, FW_BOLD,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            FONT_NAME);
    }
}

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice) {
    if (!g_initialized) {
        HMODULE hGame = GetModuleHandleA("GTAIV.exe");
        if (hGame) {
            g_gameBase = (uintptr_t)hGame;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
            IMAGE_NT_HEADERS* nt  =
                (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
            g_gameSize = nt->OptionalHeader.SizeOfImage;
            FindGamePointers();
        }
        g_initialized = true;
    }

    /* Toggle with F5 */
    static bool f5Prev = false;
    bool f5Now = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5Now && !f5Prev) g_showSpeedo = !g_showSpeedo;
    f5Prev = f5Now;

    if (g_showSpeedo && g_offsetsFound) {
        float speed = GetVehicleSpeed();
        if (speed >= 0.0f) {                    /* in a vehicle */
            CreateFontIfNeeded();

            IDirect3DSurface9* pBB = NULL;
            if (SUCCEEDED(pDevice->GetBackBuffer(
                    0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB))) {
                HDC hdc = NULL;
                if (SUCCEEDED(pBB->GetDC(&hdc))) {
                    D3DVIEWPORT9 vp;
                    pDevice->GetViewport(&vp);

                    int x = vp.Width  - SPEEDO_POS_X_OFFSET;
                    int y = vp.Height - SPEEDO_POS_Y_OFFSET;

                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d KM/H", (int)speed);
                    int len = (int)strlen(buf);

                    HFONT old = (HFONT)SelectObject(hdc, g_hFont);
                    SetBkMode(hdc, TRANSPARENT);

                    /* shadow */
                    SetTextColor(hdc, SHADOW_COLOR);
                    TextOutA(hdc, x + 2, y + 2, buf, len);
                    /* text */
                    SetTextColor(hdc, TEXT_COLOR);
                    TextOutA(hdc, x, y, buf, len);

                    SelectObject(hdc, old);
                    pBB->ReleaseDC(hdc);
                }
                pBB->Release();
            }
        }
    }

    return g_origEndScene(pDevice);
}

static HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice,
                                  D3DPRESENT_PARAMETERS* pParams) {
    if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
    return g_origReset(pDevice, pParams);
}

/* --------------------------------------------------------------------------
 * D3D9 vtable hook setup
 * -------------------------------------------------------------------------- */
static bool HookVTable(void** vt, int idx, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &old))
        return false;
    *orig  = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

static bool SetupD3D9Hook() {
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance      = g_hModule;
    wc.lpszClassName  = "SpeedoIVCE_Dummy";
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "",
                                WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100,
                                NULL, NULL, g_hModule, NULL);
    if (!hWnd) return false;

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, g_hModule);
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = hWnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* pDev = NULL;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                    hWnd,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                    &pp, &pDev);
    if (FAILED(hr) || !pDev) {
        pD3D->Release();
        DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, g_hModule);
        return false;
    }

    void** vt = *(void***)pDev;
    HookVTable(vt, 42, (void*)HookedEndScene, (void**)&g_origEndScene);
    HookVTable(vt, 16, (void*)HookedReset,    (void**)&g_origReset);

    pDev->Release();
    pD3D->Release();
    DestroyWindow(hWnd);
    UnregisterClassA(wc.lpszClassName, g_hModule);
    return true;
}

/* --------------------------------------------------------------------------
 * Init thread - waits for the game to start before hooking D3D9
 * -------------------------------------------------------------------------- */
static DWORD WINAPI InitThread(LPVOID) {
    Sleep(5000);
    for (int i = 0; i < 10; i++) {
        if (SetupD3D9Hook()) break;
        Sleep(2000);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * DLL Entry Point
 * -------------------------------------------------------------------------- */
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
    }
    return TRUE;
}
