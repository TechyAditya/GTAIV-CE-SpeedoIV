/*
 * SpeedoIV-CE - Speedometer for GTA IV Complete Edition (1.2.0.43)
 *
 * Port of the original SpeedoIV by o!nko!nk with the "retarded_chicken" skin.
 * Compatible with FusionFix (DXVK) -- uses inline hook on game's Present call
 * to obtain the real D3D9 device, then renders using D3DX9 sprites.
 *
 * No ScriptHook dependency. Vehicle speed read via pattern-scanned memory.
 * Built with MinGW GCC (i686) + d3dx9_40.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --------------------------------------------------------------------------
 * Configuration (matches original SpeedoIV Config.ini format)
 * -------------------------------------------------------------------------- */
struct SpeedoConfig {
    bool  autostart;
    int   toggleKey;     /* VK code (default 116 = F5) */
    float texSizeX, texSizeY;
    bool  enableKMH;     /* false = MPH display, true = KMH */
    float maxSpeed;
    char  screenAlign[4]; /* BL, BR, TL, TR */
    float posX, posY;
    float sizeX, sizeY;
    float angle;
    int   alpha;
    char  skinFolder[64];
};

static SpeedoConfig g_cfg = {
    true, VK_F5,
    300.0f, 300.0f,
    true, 300.0f,
    "BL",
    26.5f, -3.0f,
    242.0f, 234.0f,
    0.0f, 240,
    "Default"
};

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */
static HMODULE g_hModule       = NULL;
static bool    g_showSpeedo    = true;
static bool    g_running       = true;
static bool    g_initialized   = false;

/* D3D9 rendering */
static IDirect3DDevice9*  g_pDevice   = NULL;
static ID3DXSprite*       g_pSprite   = NULL;
static IDirect3DTexture9* g_pTexBck   = NULL;
static IDirect3DTexture9* g_pTexPin   = NULL;
static ID3DXFont*         g_pFont     = NULL;
static bool               g_d3dReady  = false;

/* Game memory */
static uintptr_t g_gameBase    = 0;
static uintptr_t g_gameSize    = 0;

/* Inline hook */
static uintptr_t g_hookAddr    = 0;
static BYTE      g_origBytes[16];
static bool      g_hooked      = false;

/* --------------------------------------------------------------------------
 * Safe memory helpers
 * -------------------------------------------------------------------------- */
static inline bool IsReadable(const void* a, size_t n) { return !IsBadReadPtr(a, n); }
static inline bool SafeReadPtr(uintptr_t a, uintptr_t* o) {
    if (!IsReadable((void*)a, 4)) return false; *o = *(uintptr_t*)a; return true;
}
static inline bool SafeReadFloat(uintptr_t a, float* o) {
    if (!IsReadable((void*)a, 4)) return false; *o = *(float*)a; return true;
}
static inline bool IsValidPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }
static inline bool IsHeapPtr(uintptr_t p) { return p >= 0x04000000 && p < 0x7FFFFFFF; }

/* --------------------------------------------------------------------------
 * Pattern Scanner
 * -------------------------------------------------------------------------- */
static uintptr_t FindPattern(uintptr_t base, size_t size,
                             const BYTE* pat, const char* mask) {
    size_t plen = strlen(mask);
    if (size < plen) return 0;
    for (size_t i = 0; i <= size - plen; i++) {
        bool ok = true;
        for (size_t j = 0; j < plen && ok; j++)
            if (mask[j] == 'x' && ((BYTE*)(base+i))[j] != pat[j]) ok = false;
        if (ok) return base + i;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Vehicle Speed (same proven logic from debug_scan)
 * -------------------------------------------------------------------------- */
static const int OFF_PEDFACTORY_PLAYER = 0x4;
static const int VEHICLE_OFFSETS[]  = { 0x32C, 0x330, 0x328, 0x33C, 0x320 };
static const int VELOCITY_OFFSETS[] = { 0x70, 0x80, 0x90, 0x60 };
static const int NUM_VEH_OFFS = 5, NUM_VEL_OFFS = 4;

static uintptr_t g_pPedFactory = 0;
static int g_vehicleOff = 0, g_velocityOff = 0;
static bool g_offsetsFound = false;

static bool TryFactory(uintptr_t ptrAddr) {
    if (!IsValidPtr(ptrAddr)) return false;
    uintptr_t fp = 0;
    if (!SafeReadPtr(ptrAddr, &fp) || !IsHeapPtr(fp)) return false;
    uintptr_t vt = 0;
    if (!SafeReadPtr(fp, &vt) || !IsValidPtr(vt)) return false;
    uintptr_t ped = 0;
    if (!SafeReadPtr(fp + OFF_PEDFACTORY_PLAYER, &ped) || !IsHeapPtr(ped)) return false;
    uintptr_t pedVt = 0;
    if (!SafeReadPtr(ped, &pedVt) || !IsValidPtr(pedVt)) return false;

    for (int v = 0; v < NUM_VEH_OFFS; v++) {
        uintptr_t veh = 0;
        if (!SafeReadPtr(ped + VEHICLE_OFFSETS[v], &veh)) continue;
        if (veh == 0) {
            g_pPedFactory = ptrAddr; g_vehicleOff = VEHICLE_OFFSETS[v];
            g_velocityOff = 0x70; g_offsetsFound = true; return true;
        }
        if (!IsHeapPtr(veh)) continue;
        uintptr_t vehVt = 0;
        if (!SafeReadPtr(veh, &vehVt) || !IsValidPtr(vehVt)) continue;
        float vx, vy, vz;
        for (int vi = 0; vi < NUM_VEL_OFFS; vi++) {
            uintptr_t vb = veh + VELOCITY_OFFSETS[vi];
            if (SafeReadFloat(vb, &vx) && SafeReadFloat(vb+4, &vy) && SafeReadFloat(vb+8, &vz)) {
                float s = sqrtf(vx*vx+vy*vy+vz*vz) * 3.6f;
                if (s >= 0.0f && s <= 500.0f) {
                    g_pPedFactory = ptrAddr; g_vehicleOff = VEHICLE_OFFSETS[v];
                    g_velocityOff = VELOCITY_OFFSETS[vi]; g_offsetsFound = true;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool FindGamePointers() {
    if (!g_gameBase) return false;
    /* Pattern 1: A1 ?? ?? ?? ?? 85 C0 74 */
    BYTE p1[] = {0xA1,0,0,0,0,0x85,0xC0,0x74}; char m1[] = "x????xxx";
    uintptr_t cur = g_gameBase; size_t rem = g_gameSize;
    for (int n = 0; n < 100 && cur < g_gameBase+g_gameSize-8; n++) {
        uintptr_t h = FindPattern(cur, rem, p1, m1);
        if (!h) break;
        if (TryFactory(*(uintptr_t*)(h+1))) return true;
        cur = h+1; rem = g_gameBase+g_gameSize-cur;
    }
    /* Pattern 2: 8B 0D ?? ?? ?? ?? 8B 41 04 */
    BYTE p2[] = {0x8B,0x0D,0,0,0,0,0x8B,0x41,0x04}; char m2[] = "xx????xxx";
    cur = g_gameBase; rem = g_gameSize;
    for (int n = 0; n < 100 && cur < g_gameBase+g_gameSize-9; n++) {
        uintptr_t h = FindPattern(cur, rem, p2, m2);
        if (!h) break;
        if (TryFactory(*(uintptr_t*)(h+2))) return true;
        cur = h+1; rem = g_gameBase+g_gameSize-cur;
    }
    /* Pattern 3: 8B 35 ?? ?? ?? ?? 85 F6 74 */
    BYTE p3[] = {0x8B,0x35,0,0,0,0,0x85,0xF6,0x74}; char m3[] = "xx????xxx";
    cur = g_gameBase; rem = g_gameSize;
    for (int n = 0; n < 100 && cur < g_gameBase+g_gameSize-9; n++) {
        uintptr_t h = FindPattern(cur, rem, p3, m3);
        if (!h) break;
        if (TryFactory(*(uintptr_t*)(h+2))) return true;
        cur = h+1; rem = g_gameBase+g_gameSize-cur;
    }
    return false;
}

static float GetVehicleSpeed() {
    if (!g_offsetsFound) return -1.0f;
    uintptr_t fp = 0, ped = 0, veh = 0;
    if (!SafeReadPtr(g_pPedFactory, &fp) || !IsHeapPtr(fp)) return -1.0f;
    if (!SafeReadPtr(fp + OFF_PEDFACTORY_PLAYER, &ped) || !IsHeapPtr(ped)) return -1.0f;
    if (!SafeReadPtr(ped + g_vehicleOff, &veh)) return -1.0f;
    if (veh == 0) return -1.0f;
    if (!IsHeapPtr(veh)) return -1.0f;
    float vx, vy, vz;
    uintptr_t vb = veh + g_velocityOff;
    if (!SafeReadFloat(vb, &vx) || !SafeReadFloat(vb+4, &vy) || !SafeReadFloat(vb+8, &vz))
        return -1.0f;
    float s = sqrtf(vx*vx+vy*vy+vz*vz) * 3.6f;
    if (s >= 0.0f && s <= 500.0f) return s;
    return 0.0f;
}

/* --------------------------------------------------------------------------
 * D3DX Rendering
 * -------------------------------------------------------------------------- */
static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    /* Go up from plugins/ to game root, then into SpeedoIV/Config.ini */
    char* slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    slash = strrchr(path, '\\');
    if (slash) *slash = 0;

    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\SpeedoIV\\Config.ini", path);

    g_cfg.autostart = GetPrivateProfileIntA("Config", "Autostart", 1, ini) != 0;
    g_cfg.toggleKey = GetPrivateProfileIntA("Config", "ToggleKey", VK_F5, ini);
    g_cfg.texSizeX = (float)GetPrivateProfileIntA("Config", "TexSizeX", 300, ini);
    g_cfg.texSizeY = (float)GetPrivateProfileIntA("Config", "TexSizeY", 300, ini);
    g_cfg.enableKMH = GetPrivateProfileIntA("Config", "EnableKMH", 1, ini) != 0;
    g_cfg.maxSpeed = (float)GetPrivateProfileIntA("Config", "MaxSpeed", 300, ini);
    g_cfg.sizeX = (float)GetPrivateProfileIntA("Config", "SizeX", 242, ini);
    g_cfg.sizeY = (float)GetPrivateProfileIntA("Config", "SizeY", 234, ini);
    g_cfg.alpha = GetPrivateProfileIntA("Config", "Alpha", 240, ini);
    GetPrivateProfileStringA("Config", "SkinFolder", "Default", g_cfg.skinFolder, 64, ini);
    GetPrivateProfileStringA("Config", "ScreenAlign", "BL", g_cfg.screenAlign, 4, ini);

    /* Read float values as strings */
    char buf[32];
    GetPrivateProfileStringA("Config", "PositionX", "26.5", buf, 32, ini);
    g_cfg.posX = (float)atof(buf);
    GetPrivateProfileStringA("Config", "PositionY", "-3.0", buf, 32, ini);
    g_cfg.posY = (float)atof(buf);
    GetPrivateProfileStringA("Config", "Angle", "0.0", buf, 32, ini);
    g_cfg.angle = (float)atof(buf);

    g_showSpeedo = g_cfg.autostart;
}

static bool InitD3DResources(IDirect3DDevice9* pDev) {
    if (g_d3dReady) return true;
    g_pDevice = pDev;

    /* Build texture paths */
    char basePath[MAX_PATH];
    GetModuleFileNameA(g_hModule, basePath, MAX_PATH);
    char* sl = strrchr(basePath, '\\'); if (sl) *sl = 0;
    sl = strrchr(basePath, '\\'); if (sl) *sl = 0;

    char bckPath[MAX_PATH], pinPath[MAX_PATH];
    snprintf(bckPath, MAX_PATH, "%s\\SpeedoIV\\%s\\Bck.png", basePath, g_cfg.skinFolder);
    snprintf(pinPath, MAX_PATH, "%s\\SpeedoIV\\%s\\Pin.png", basePath, g_cfg.skinFolder);

    /* Create sprite */
    if (FAILED(D3DXCreateSprite(pDev, &g_pSprite)))
        return false;

    /* Load textures */
    if (FAILED(D3DXCreateTextureFromFileExA(
            pDev, bckPath,
            (UINT)g_cfg.texSizeX, (UINT)g_cfg.texSizeY,
            1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR,
            0, NULL, NULL, &g_pTexBck)))
        return false;

    if (FAILED(D3DXCreateTextureFromFileExA(
            pDev, pinPath,
            (UINT)g_cfg.texSizeX, (UINT)g_cfg.texSizeY,
            1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR,
            0, NULL, NULL, &g_pTexPin)))
        return false;

    /* Create font for speed text */
    D3DXCreateFontA(pDev, 24, 0, FW_BOLD, 1, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    "Arial", &g_pFont);

    g_d3dReady = true;
    return true;
}

static void ReleaseD3DResources() {
    if (g_pSprite) { g_pSprite->Release(); g_pSprite = NULL; }
    if (g_pTexBck) { g_pTexBck->Release(); g_pTexBck = NULL; }
    if (g_pTexPin) { g_pTexPin->Release(); g_pTexPin = NULL; }
    if (g_pFont)   { g_pFont->Release();   g_pFont = NULL; }
    g_d3dReady = false;
}

static void RenderSpeedometer(IDirect3DDevice9* pDev, float speed) {
    if (!InitD3DResources(pDev)) return;

    /* Get screen size */
    D3DVIEWPORT9 vp;
    pDev->GetViewport(&vp);
    float scrW = (float)vp.Width;
    float scrH = (float)vp.Height;

    /* Calculate position based on screen alignment */
    float drawX, drawY;
    if (g_cfg.screenAlign[0] == 'B' || g_cfg.screenAlign[0] == 'b')
        drawY = scrH - g_cfg.sizeY + g_cfg.posY;
    else
        drawY = g_cfg.posY;

    if (g_cfg.screenAlign[1] == 'R' || g_cfg.screenAlign[1] == 'r')
        drawX = scrW - g_cfg.sizeX + g_cfg.posX;
    else
        drawX = g_cfg.posX;

    /* Needle rotation: map speed to angle
     * Original SpeedoIV: 0 speed = -135 degrees, max speed = +135 degrees
     * Total sweep = 270 degrees */
    float speedRatio = speed / g_cfg.maxSpeed;
    if (speedRatio > 1.0f) speedRatio = 1.0f;
    float needleAngle = (-135.0f + speedRatio * 270.0f) * (float)M_PI / 180.0f;

    /* Center of the speedometer dial */
    float cx = g_cfg.sizeX / 2.0f;
    float cy = g_cfg.sizeY / 2.0f;

    D3DXVECTOR2 center(cx, cy);
    D3DXVECTOR2 pos(drawX, drawY);
    D3DXVECTOR2 scale(g_cfg.sizeX / g_cfg.texSizeX,
                      g_cfg.sizeY / g_cfg.texSizeY);

    DWORD color = D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255);

    g_pSprite->Begin(D3DXSPRITE_ALPHABLEND);

    /* Draw background (no rotation) */
    D3DXMATRIX matBck;
    D3DXMatrixTransformation2D(&matBck, &center, 0.0f, &scale,
                                &center, g_cfg.angle * (float)M_PI / 180.0f,
                                &pos);
    g_pSprite->SetTransform(&matBck);
    g_pSprite->Draw(g_pTexBck, NULL, NULL, NULL, color);

    /* Draw needle (rotated) */
    D3DXMATRIX matPin;
    D3DXMatrixTransformation2D(&matPin, &center, 0.0f, &scale,
                                &center, needleAngle + g_cfg.angle * (float)M_PI / 180.0f,
                                &pos);
    g_pSprite->SetTransform(&matPin);
    g_pSprite->Draw(g_pTexPin, NULL, NULL, NULL, color);

    g_pSprite->End();

    /* Draw digital speed text */
    if (g_pFont) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)speed);
        RECT rc;
        rc.left   = (LONG)(drawX + cx - 40);
        rc.top    = (LONG)(drawY + cy + 30);
        rc.right  = (LONG)(drawX + cx + 40);
        rc.bottom = (LONG)(drawY + cy + 60);
        g_pFont->DrawTextA(NULL, buf, -1, &rc, DT_CENTER | DT_NOCLIP,
                           D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255));
    }
}

/* --------------------------------------------------------------------------
 * EndScene Hook via inline patch
 *
 * We find the game's call to IDirect3DDevice9::EndScene and place a JMP
 * detour before it. In the detour we get the device from ECX and render.
 *
 * Alternative: We hook the game's per-frame render function.
 * Since DXVK shares the vtable for ALL devices created through its d3d9.dll,
 * we CAN hook the vtable -- but we need to get it from the game's device,
 * not from a separate dummy device.
 *
 * Simplest working approach: scan for the game's stored device pointer,
 * then hook EndScene through its vtable directly.
 * -------------------------------------------------------------------------- */

typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
static EndScene_t g_origEndScene = NULL;

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDev) {
    if (!g_initialized) {
        HMODULE hGame = GetModuleHandleA("GTAIV.exe");
        if (hGame) {
            g_gameBase = (uintptr_t)hGame;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
            g_gameSize = nt->OptionalHeader.SizeOfImage;
            FindGamePointers();
        }
        LoadConfig();
        g_initialized = true;
    }

    /* Toggle with configured key */
    static bool keyPrev = false;
    bool keyNow = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (keyNow && !keyPrev) g_showSpeedo = !g_showSpeedo;
    keyPrev = keyNow;

    if (g_showSpeedo && g_offsetsFound) {
        float speed = GetVehicleSpeed();
        if (speed >= 0.0f) {
            RenderSpeedometer(pDev, speed);
        }
    }

    return g_origEndScene(pDev);
}

/* --------------------------------------------------------------------------
 * Hook Setup: Get game's device and hook its vtable
 *
 * DXVK uses a single vtable shared by all device instances created through
 * its d3d9.dll. So we wait for the game to create its device, then find it
 * by scanning the game's global pointers, and hook EndScene on that vtable.
 * -------------------------------------------------------------------------- */
static bool HookGameDevice() {
    /* Strategy: scan for global pointers that hold a valid IDirect3DDevice9*
     * A valid device pointer will have a vtable where entry[42] (EndScene)
     * points into the d3d9.dll module (DXVK's code). */

    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) return false;
    IMAGE_DOS_HEADER* d3dDos = (IMAGE_DOS_HEADER*)hD3D9;
    IMAGE_NT_HEADERS* d3dNt = (IMAGE_NT_HEADERS*)((uintptr_t)hD3D9 + d3dDos->e_lfanew);
    uintptr_t d3dBase = (uintptr_t)hD3D9;
    uintptr_t d3dEnd = d3dBase + d3dNt->OptionalHeader.SizeOfImage;

    /* Scan the game's .data/.bss sections for a pointer that looks like a device */
    /* The game image has globals from gameBase to gameBase+gameSize */
    uintptr_t scanStart = g_gameBase;
    uintptr_t scanEnd = g_gameBase + g_gameSize;

    for (uintptr_t addr = scanStart; addr < scanEnd - 4; addr += 4) {
        if (!IsReadable((void*)addr, 4)) continue;
        uintptr_t candidate = *(uintptr_t*)addr;
        if (!IsHeapPtr(candidate)) continue;
        if (!IsReadable((void*)candidate, 4)) continue;

        /* Check vtable pointer */
        uintptr_t vtablePtr = *(uintptr_t*)candidate;
        if (vtablePtr < d3dBase || vtablePtr >= d3dEnd) continue;

        /* vtable should be readable and entry[42] should point into d3d9.dll */
        if (!IsReadable((void*)(vtablePtr + 42*4), 4)) continue;
        uintptr_t endSceneAddr = *(uintptr_t*)(vtablePtr + 42*4);
        if (endSceneAddr < d3dBase || endSceneAddr >= d3dEnd) continue;

        /* This looks like a real IDirect3DDevice9*! Hook its EndScene. */
        void** vt = (void**)vtablePtr;
        DWORD oldProt;
        if (VirtualProtect(&vt[42], 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
            g_origEndScene = (EndScene_t)vt[42];
            vt[42] = (void*)HookedEndScene;
            VirtualProtect(&vt[42], 4, oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Init thread
 * -------------------------------------------------------------------------- */
static DWORD WINAPI InitThread(LPVOID) {
    /* Wait for game to initialize its D3D device */
    Sleep(10000);

    HMODULE hGame = GetModuleHandleA("GTAIV.exe");
    if (hGame) {
        g_gameBase = (uintptr_t)hGame;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
        g_gameSize = nt->OptionalHeader.SizeOfImage;
    }

    /* Try to hook -- retry if game device isn't ready yet */
    for (int i = 0; i < 20; i++) {
        if (HookGameDevice()) break;
        Sleep(2000);
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * DLL Entry
 * -------------------------------------------------------------------------- */
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        ReleaseD3DResources();
    }
    return TRUE;
}
