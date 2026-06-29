/*
 * SpeedoIV-CE - Simple Speedometer for GTA IV Complete Edition (1.2.0.43)
 * 
 * A lightweight ASI plugin that displays vehicle speed as a text HUD overlay.
 * Compatible with FusionFix and the Complete Edition.
 * 
 * Approach: Hook IDirect3DDevice9::EndScene via vtable to draw text using
 * GDI (no D3DX dependency). Read vehicle speed from game memory using
 * pattern scanning for CE compatibility.
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
static const int   SPEEDO_POS_X_OFFSET = 220;  // pixels from right edge
static const int   SPEEDO_POS_Y_OFFSET = 80;   // pixels from bottom edge
static const int   FONT_SIZE           = 28;
static const char  FONT_NAME[]         = "Consolas";
static const DWORD TEXT_COLOR          = RGB(255, 255, 255);
static const DWORD SHADOW_COLOR        = RGB(0, 0, 0);
static const float KMH_MULTIPLIER     = 3.6f;  // m/s -> km/h

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
static BYTE        g_endSceneTrampoline[16];
static BYTE        g_resetTrampoline[16];

/* Game memory pointers (resolved via pattern scan) */
static uintptr_t g_gameBase    = 0;
static uintptr_t g_gameSize    = 0;

/* --------------------------------------------------------------------------
 * Pattern Scanner
 * -------------------------------------------------------------------------- */
static bool PatternMatch(const BYTE* data, const BYTE* pattern, const char* mask, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (mask[i] == 'x' && data[i] != pattern[i])
            return false;
    }
    return true;
}

static uintptr_t FindPattern(uintptr_t base, size_t size, const BYTE* pattern, const char* mask) {
    size_t patLen = strlen(mask);
    for (size_t i = 0; i <= size - patLen; i++) {
        if (PatternMatch((const BYTE*)(base + i), pattern, mask, patLen))
            return base + i;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Vehicle Speed Reading
 * 
 * GTA IV CE stores the player's world info in a global structure.
 * We use pattern scanning to find:
 *   1. The CPlayerInfo / World pointer that leads to player ped
 *   2. From ped -> current vehicle pointer  
 *   3. From vehicle -> velocity vector (3 floats)
 * 
 * CE 1.2.0.43 layout (from FusionFix/IV-SDK research):
 *   - CPed at player ped ptr
 *   - CPed + 0x314 = pointer to CPlayerInfo (or vehicle-related)
 *   - In RAGE, CEntity base has matrix at +0x20, and CPhysical 
 *     has velocity embedded in the object
 *
 * We use a robust approach: scan for the "FindPlayerVehicle" 
 * function pattern, or alternatively read from known global offsets.
 *
 * Fallback: Scan for the player ped global pointer pattern.
 * -------------------------------------------------------------------------- */

/* 
 * Strategy: Find the World/CPedFactory global pointer.
 * In GTA IV CE, there's a global pointer to CPedFactory which has:
 *   CPedFactory + 0x0  = vtable
 *   CPedFactory + 0x4  = pointer to local player CPed
 * Then:
 *   CPed + 0x32C = pointer to current CVehicle (CE offset, differs from legacy)
 * Then:
 *   CVehicle inherits CPhysical -> CEntity
 *   Velocity is at CPhysical + 0x70 (3 floats: vx, vy, vz) for CE
 *   Or more reliably, at the matrix velocity row
 */

/* CE-specific offsets */
static const int OFF_PEDFACTORY_PLAYER = 0x4;   // CPedFactory -> player ped ptr

/* 
 * For CE 1.2.0.43, the ped-to-vehicle offset and velocity offsets
 * need to be found via pattern scanning since they differ from legacy.
 * We'll try multiple known offsets.
 */
static const int VEHICLE_OFFSETS_TO_TRY[] = { 0x32C, 0x330, 0x328, 0x33C, 0x320 };
static const int VELOCITY_OFFSETS_TO_TRY[] = { 0x70, 0x80, 0x90, 0x60 };

static uintptr_t g_pPedFactory = 0;
static int g_vehicleOffset = 0;
static int g_velocityOffset = 0;
static bool g_offsetsFound = false;

/* Pattern to find CPedFactory global pointer in CE 1.2.0.43 */
/* We look for code that accesses the global CPedFactory pointer */
static bool FindGamePointers() {
    if (g_gameBase == 0) return false;
    
    /* 
     * Pattern: Look for references to CPedFactory.
     * In CE, the pattern near player ped access is:
     * A1 ?? ?? ?? ??    mov eax, [CPedFactory]
     * 8B ?? 04          mov reg, [eax+4]  (player ped)
     * 
     * We search for a known CE pattern from FusionFix source:
     * "83 3D ?? ?? ?? ?? 00" - cmp [CPedFactory], 0 (null check)
     */
    
    /* Try pattern: cmp dword ptr [addr], 0 ... followed by accessing player */
    /* CE pattern from known GTA IV CE modding: */
    /* A1 XX XX XX XX 85 C0 74 - mov eax,[g_pPedFactory]; test eax,eax; jz */
    BYTE pat1[] = { 0xA1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74 };
    char msk1[] = "x????xxx";
    
    uintptr_t found = 0;
    /* Scan multiple times to find the right instance */
    uintptr_t scanStart = g_gameBase;
    size_t scanSize = g_gameSize;
    
    for (int attempt = 0; attempt < 50 && scanStart < g_gameBase + g_gameSize - 8; attempt++) {
        found = FindPattern(scanStart, scanSize, pat1, msk1);
        if (found == 0) break;
        
        /* Read the global pointer address from the instruction */
        uintptr_t ptrAddr = *(uintptr_t*)(found + 1);
        
        /* Validate: the pointer should be in a data section (readable, not code) */
        if (ptrAddr > 0x10000 && ptrAddr < 0x7FFFFFFF) {
            __try {
                uintptr_t factoryPtr = *(uintptr_t*)ptrAddr;
                if (factoryPtr > 0x10000 && factoryPtr < 0x7FFFFFFF) {
                    /* Check if offset +4 looks like a valid ped pointer */
                    uintptr_t pedPtr = *(uintptr_t*)(factoryPtr + OFF_PEDFACTORY_PLAYER);
                    if (pedPtr > 0x10000 && pedPtr < 0x7FFFFFFF) {
                        /* Looks promising - try vehicle offsets */
                        for (int v = 0; v < sizeof(VEHICLE_OFFSETS_TO_TRY)/sizeof(int); v++) {
                            int vOff = VEHICLE_OFFSETS_TO_TRY[v];
                            __try {
                                uintptr_t vehPtr = *(uintptr_t*)(pedPtr + vOff);
                                /* Vehicle ptr should be 0 (on foot) or valid address */
                                if (vehPtr == 0 || (vehPtr > 0x10000 && vehPtr < 0x7FFFFFFF)) {
                                    g_pPedFactory = ptrAddr;
                                    g_vehicleOffset = vOff;
                                    /* For velocity, try each offset */
                                    g_velocityOffset = 0x70; /* most common in RAGE */
                                    g_offsetsFound = true;
                                    return true;
                                }
                            } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                        }
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) { /* invalid pointer, continue */ }
        }
        
        scanStart = found + 1;
        scanSize = g_gameBase + g_gameSize - scanStart;
    }
    
    /* Fallback: try a different pattern */
    /* 8B 0D ?? ?? ?? ?? 8B ?? 04 - mov ecx,[g_ptr]; mov reg,[ecx+4] */
    BYTE pat2[] = { 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x41, 0x04 };
    char msk2[] = "xx????xxx";
    
    scanStart = g_gameBase;
    scanSize = g_gameSize;
    
    for (int attempt = 0; attempt < 50 && scanStart < g_gameBase + g_gameSize - 10; attempt++) {
        found = FindPattern(scanStart, scanSize, pat2, msk2);
        if (found == 0) break;
        
        uintptr_t ptrAddr = *(uintptr_t*)(found + 2);
        
        if (ptrAddr > 0x10000 && ptrAddr < 0x7FFFFFFF) {
            __try {
                uintptr_t factoryPtr = *(uintptr_t*)ptrAddr;
                if (factoryPtr > 0x10000 && factoryPtr < 0x7FFFFFFF) {
                    uintptr_t pedPtr = *(uintptr_t*)(factoryPtr + OFF_PEDFACTORY_PLAYER);
                    if (pedPtr > 0x10000 && pedPtr < 0x7FFFFFFF) {
                        for (int v = 0; v < sizeof(VEHICLE_OFFSETS_TO_TRY)/sizeof(int); v++) {
                            int vOff = VEHICLE_OFFSETS_TO_TRY[v];
                            __try {
                                uintptr_t vehPtr = *(uintptr_t*)(pedPtr + vOff);
                                if (vehPtr == 0 || (vehPtr > 0x10000 && vehPtr < 0x7FFFFFFF)) {
                                    g_pPedFactory = ptrAddr;
                                    g_vehicleOffset = vOff;
                                    g_velocityOffset = 0x70;
                                    g_offsetsFound = true;
                                    return true;
                                }
                            } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                        }
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) { /* continue */ }
        }
        
        scanStart = found + 1;
        scanSize = g_gameBase + g_gameSize - scanStart;
    }
    
    return false;
}

static float GetVehicleSpeed() {
    if (!g_offsetsFound || g_pPedFactory == 0) return -1.0f;
    
    __try {
        uintptr_t factoryPtr = *(uintptr_t*)g_pPedFactory;
        if (factoryPtr == 0) return -1.0f;
        
        uintptr_t pedPtr = *(uintptr_t*)(factoryPtr + OFF_PEDFACTORY_PLAYER);
        if (pedPtr == 0) return -1.0f;
        
        uintptr_t vehPtr = *(uintptr_t*)(pedPtr + g_vehicleOffset);
        if (vehPtr == 0) return -1.0f; /* on foot */
        
        /* Read velocity vector from vehicle */
        float vx = *(float*)(vehPtr + g_velocityOffset);
        float vy = *(float*)(vehPtr + g_velocityOffset + 4);
        float vz = *(float*)(vehPtr + g_velocityOffset + 8);
        
        /* Calculate magnitude in m/s, convert to km/h */
        float speed_ms = sqrtf(vx*vx + vy*vy + vz*vz);
        float speed_kmh = speed_ms * KMH_MULTIPLIER;
        
        /* Sanity check - GTA IV vehicles shouldn't exceed ~400 km/h */
        if (speed_kmh < 0.0f || speed_kmh > 500.0f) {
            /* Velocity offset might be wrong, try alternates */
            for (int i = 0; i < sizeof(VELOCITY_OFFSETS_TO_TRY)/sizeof(int); i++) {
                int off = VELOCITY_OFFSETS_TO_TRY[i];
                if (off == g_velocityOffset) continue;
                
                __try {
                    float vx2 = *(float*)(vehPtr + off);
                    float vy2 = *(float*)(vehPtr + off + 4);
                    float vz2 = *(float*)(vehPtr + off + 8);
                    float s = sqrtf(vx2*vx2 + vy2*vy2 + vz2*vz2) * KMH_MULTIPLIER;
                    if (s >= 0.0f && s <= 500.0f) {
                        g_velocityOffset = off;
                        return s;
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
            }
            return 0.0f;
        }
        
        return speed_kmh;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

/* --------------------------------------------------------------------------
 * D3D9 EndScene Hook - Draw speedometer using GDI on the backbuffer
 * -------------------------------------------------------------------------- */
static void CreateFontIfNeeded() {
    if (g_hFont == NULL) {
        g_hFont = CreateFontA(
            FONT_SIZE, 0, 0, 0,
            FW_BOLD,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            FONT_NAME
        );
    }
}

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice) {
    if (!g_initialized) {
        /* Get game module info */
        HMODULE hGame = GetModuleHandleA("GTAIV.exe");
        if (hGame) {
            g_gameBase = (uintptr_t)hGame;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
            g_gameSize = nt->OptionalHeader.SizeOfImage;
            FindGamePointers();
        }
        g_initialized = true;
    }
    
    /* Toggle speedometer with F5 */
    static bool f5WasDown = false;
    bool f5IsDown = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5IsDown && !f5WasDown) {
        g_showSpeedo = !g_showSpeedo;
    }
    f5WasDown = f5IsDown;
    
    if (g_showSpeedo && g_offsetsFound) {
        float speed = GetVehicleSpeed();
        
        /* Only show when in a vehicle (speed >= 0) */
        if (speed >= 0.0f) {
            CreateFontIfNeeded();
            
            /* Get backbuffer for GDI drawing */
            IDirect3DSurface9* pBackBuffer = NULL;
            if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
                HDC hdc = NULL;
                if (SUCCEEDED(pBackBuffer->GetDC(&hdc))) {
                    /* Get viewport size */
                    D3DVIEWPORT9 vp;
                    pDevice->GetViewport(&vp);
                    
                    int x = vp.Width - SPEEDO_POS_X_OFFSET;
                    int y = vp.Height - SPEEDO_POS_Y_OFFSET;
                    
                    char speedText[32];
                    snprintf(speedText, sizeof(speedText), "%d KM/H", (int)speed);
                    
                    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFont);
                    SetBkMode(hdc, TRANSPARENT);
                    
                    /* Draw shadow */
                    SetTextColor(hdc, SHADOW_COLOR);
                    TextOutA(hdc, x + 2, y + 2, speedText, (int)strlen(speedText));
                    
                    /* Draw text */
                    SetTextColor(hdc, TEXT_COLOR);
                    TextOutA(hdc, x, y, speedText, (int)strlen(speedText));
                    
                    SelectObject(hdc, hOldFont);
                    pBackBuffer->ReleaseDC(hdc);
                }
                pBackBuffer->Release();
            }
        }
    }
    
    return g_origEndScene(pDevice);
}

static HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParams) {
    /* Clean up GDI font on device reset */
    if (g_hFont) {
        DeleteObject(g_hFont);
        g_hFont = NULL;
    }
    return g_origReset(pDevice, pParams);
}

/* --------------------------------------------------------------------------
 * D3D9 vtable hook setup
 * -------------------------------------------------------------------------- */

/* Overwrite vtable entry with our function, store original */
static bool HookVTableEntry(void** vtable, int index, void* hookFn, void** origFn) {
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    
    *origFn = vtable[index];
    vtable[index] = hookFn;
    
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

static bool SetupD3D9Hook() {
    /* Create a temporary window and D3D9 device to get the vtable */
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = g_hModule;
    wc.lpszClassName = "SpeedoIVCE_Dummy";
    RegisterClassExA(&wc);
    
    HWND hWnd = CreateWindowExA(
        0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, NULL, NULL, g_hModule, NULL
    );
    if (!hWnd) return false;
    
    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, g_hModule);
        return false;
    }
    
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hWnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    
    IDirect3DDevice9* pDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp, &pDevice
    );
    
    if (FAILED(hr) || !pDevice) {
        pD3D->Release();
        DestroyWindow(hWnd);
        UnregisterClassA(wc.lpszClassName, g_hModule);
        return false;
    }
    
    /* Get vtable from the device */
    void** vtable = *(void***)pDevice;
    
    /* EndScene is at index 42, Reset is at index 16 */
    HookVTableEntry(vtable, 42, (void*)HookedEndScene, (void**)&g_origEndScene);
    HookVTableEntry(vtable, 16, (void*)HookedReset, (void**)&g_origReset);
    
    /* Cleanup dummy device - hooks persist because game uses same vtable */
    pDevice->Release();
    pD3D->Release();
    DestroyWindow(hWnd);
    UnregisterClassA(wc.lpszClassName, g_hModule);
    
    return true;
}

/* --------------------------------------------------------------------------
 * Wait for game to fully load before hooking
 * -------------------------------------------------------------------------- */
static DWORD WINAPI InitThread(LPVOID) {
    /* Wait for the game to initialize D3D9 */
    /* FusionFix replaces d3d9.dll, so the game's device shares the vtable */
    Sleep(5000);
    
    /* Try to hook - retry a few times if the game isn't ready */
    for (int i = 0; i < 10; i++) {
        if (SetupD3D9Hook()) {
            break;
        }
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
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_hFont) {
            DeleteObject(g_hFont);
            g_hFont = NULL;
        }
    }
    return TRUE;
}
