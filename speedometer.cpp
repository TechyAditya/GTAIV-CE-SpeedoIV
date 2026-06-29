/*
 * SpeedoIV-CE - Speedometer for GTA IV Complete Edition (1.2.0.43)
 *
 * Port of the original SpeedoIV with custom skins.
 * Compatible with FusionFix (DXVK).
 *
 * Uses: gtaiv_sdk.h (game data) + d3d9_hook.h (rendering hook)
 */

#define _USE_MATH_DEFINES
#include "gtaiv_sdk.h"
#include "d3d9_hook.h"
#include <d3d9.h>
#include <d3dx9.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==========================================================================
 * Config
 * ========================================================================== */
struct Config {
    bool  autostart   = true;
    int   toggleKey   = VK_F5;
    float texSizeX    = 300.f;
    float texSizeY    = 300.f;
    bool  kmh         = true;
    float maxSpeed    = 300.f;
    char  align[4]    = "BL";
    float posX        = 26.5f;
    float posY        = -3.f;
    float sizeX       = 242.f;
    float sizeY       = 234.f;
    float angle       = 0.f;
    int   alpha       = 240;
    char  skin[64]    = "Default";
};

static Config   g_cfg;
static HMODULE  g_hModule = NULL;
static bool     g_visible = true;

/* ==========================================================================
 * D3D9 Resources
 * ========================================================================== */
static ID3DXSprite*       g_sprite = NULL;
static IDirect3DTexture9* g_texBck = NULL;
static IDirect3DTexture9* g_texPin = NULL;
static ID3DXFont*         g_font   = NULL;
static bool               g_resOk  = false;

/* ==========================================================================
 * Config Loading
 * ========================================================================== */
static void LoadConfig() {
    char base[MAX_PATH];
    GetModuleFileNameA(g_hModule, base, MAX_PATH);
    /* plugins\SpeedoIV-CE.asi -> go up to game root */
    char* s = strrchr(base, '\\'); if (s) *s = 0;
    s = strrchr(base, '\\'); if (s) *s = 0;

    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\SpeedoIV\\Config.ini", base);

    auto getF = [&](const char* key, float def) -> float {
        char buf[32];
        char defStr[32]; snprintf(defStr, 32, "%.2f", def);
        GetPrivateProfileStringA("Config", key, defStr, buf, 32, ini);
        return (float)atof(buf);
    };

    auto getBool = [&](const char* key, bool def) -> bool {
        char buf[16];
        GetPrivateProfileStringA("Config", key, def ? "true" : "false", buf, 16, ini);
        return (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0 ||
                _stricmp(buf, "yes") == 0);
    };

    g_cfg.autostart = getBool("Autostart", true);
    g_cfg.toggleKey = GetPrivateProfileIntA("Config", "ToggleKey", VK_F5, ini);
    g_cfg.texSizeX  = getF("TexSizeX", 300.f);
    g_cfg.texSizeY  = getF("TexSizeY", 300.f);
    g_cfg.kmh       = getBool("EnableKMH", true);
    g_cfg.maxSpeed  = getF("MaxSpeed", 300.f);
    g_cfg.posX      = getF("PositionX", 26.5f);
    g_cfg.posY      = getF("PositionY", -3.f);
    g_cfg.sizeX     = getF("SizeX", 242.f);
    g_cfg.sizeY     = getF("SizeY", 234.f);
    g_cfg.angle     = getF("Angle", 0.f);
    g_cfg.alpha     = GetPrivateProfileIntA("Config", "Alpha", 240, ini);
    GetPrivateProfileStringA("Config", "SkinFolder", "Default", g_cfg.skin, 64, ini);
    GetPrivateProfileStringA("Config", "ScreenAlign", "BL", g_cfg.align, 4, ini);

    g_visible = g_cfg.autostart;
    sdk::Log("Config loaded: autostart=%d kmh=%d maxSpeed=%.0f skin=%s align=%s",
             g_cfg.autostart, g_cfg.kmh, g_cfg.maxSpeed, g_cfg.skin, g_cfg.align);
}

/* ==========================================================================
 * D3D9 Resource Init
 * ========================================================================== */
static bool InitResources(IDirect3DDevice9* dev) {
    if (g_resOk) return true;

    char base[MAX_PATH];
    GetModuleFileNameA(g_hModule, base, MAX_PATH);
    char* s = strrchr(base, '\\'); if (s) *s = 0;
    s = strrchr(base, '\\'); if (s) *s = 0;

    char bck[MAX_PATH], pin[MAX_PATH];
    snprintf(bck, MAX_PATH, "%s\\SpeedoIV\\%s\\Bck.png", base, g_cfg.skin);
    snprintf(pin, MAX_PATH, "%s\\SpeedoIV\\%s\\Pin.png", base, g_cfg.skin);

    if (FAILED(D3DXCreateSprite(dev, &g_sprite))) {
        sdk::Log("Failed: D3DXCreateSprite"); return false;
    }
    if (FAILED(D3DXCreateTextureFromFileExA(dev, bck,
            (UINT)g_cfg.texSizeX, (UINT)g_cfg.texSizeY, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &g_texBck))) {
        sdk::Log("Failed: load %s", bck); return false;
    }
    if (FAILED(D3DXCreateTextureFromFileExA(dev, pin,
            (UINT)g_cfg.texSizeX, (UINT)g_cfg.texSizeY, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &g_texPin))) {
        sdk::Log("Failed: load %s", pin); return false;
    }
    D3DXCreateFontA(dev, 22, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, "Arial", &g_font);

    g_resOk = true;
    sdk::Log("D3D resources loaded OK");
    return true;
}

static void ReleaseResources() {
    if (g_sprite) { g_sprite->Release(); g_sprite = NULL; }
    if (g_texBck) { g_texBck->Release(); g_texBck = NULL; }
    if (g_texPin) { g_texPin->Release(); g_texPin = NULL; }
    if (g_font)   { g_font->Release();   g_font = NULL; }
    g_resOk = false;
}

/* ==========================================================================
 * Render
 * ========================================================================== */
static void Render(IDirect3DDevice9* dev, float speed) {
    if (!InitResources(dev)) return;

    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    float scrW = (float)vp.Width, scrH = (float)vp.Height;

    /* Position */
    float drawX, drawY;
    if (g_cfg.align[0] == 'B' || g_cfg.align[0] == 'b')
        drawY = scrH - g_cfg.sizeY + g_cfg.posY;
    else
        drawY = g_cfg.posY;
    if (g_cfg.align[1] == 'R' || g_cfg.align[1] == 'r')
        drawX = scrW - g_cfg.sizeX + g_cfg.posX;
    else
        drawX = g_cfg.posX;

    /* Needle angle: 0 -> -135deg, max -> +135deg (270deg sweep) */
    float ratio = speed / g_cfg.maxSpeed;
    if (ratio > 1.f) ratio = 1.f;
    float needle = (-135.f + ratio * 270.f) * (float)M_PI / 180.f;

    D3DXVECTOR2 center(g_cfg.sizeX / 2.f, g_cfg.sizeY / 2.f);
    D3DXVECTOR2 pos(drawX, drawY);
    D3DXVECTOR2 scale(g_cfg.sizeX / g_cfg.texSizeX, g_cfg.sizeY / g_cfg.texSizeY);
    DWORD color = D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255);

    g_sprite->Begin(D3DXSPRITE_ALPHABLEND);

    /* Background */
    D3DXMATRIX mat;
    D3DXMatrixTransformation2D(&mat, &center, 0.f, &scale, &center,
                                g_cfg.angle * (float)M_PI / 180.f, &pos);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texBck, NULL, NULL, NULL, color);

    /* Needle */
    D3DXMatrixTransformation2D(&mat, &center, 0.f, &scale, &center,
                                needle + g_cfg.angle * (float)M_PI / 180.f, &pos);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texPin, NULL, NULL, NULL, color);

    g_sprite->End();

    /* Digital readout */
    if (g_font) {
        char buf[32];
        snprintf(buf, 32, "%d", (int)speed);
        RECT rc = { (LONG)(drawX + center.x - 30), (LONG)(drawY + center.y + 25),
                    (LONG)(drawX + center.x + 30), (LONG)(drawY + center.y + 55) };
        g_font->DrawTextA(NULL, buf, -1, &rc, DT_CENTER | DT_NOCLIP,
                          D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255));
    }
}

/* ==========================================================================
 * Frame Callback (called by d3d9_hook every EndScene)
 * ========================================================================== */
static bool g_gameInit = false;

static void OnFrame(IDirect3DDevice9* dev) {
    /* One-time game data init */
    if (!g_gameInit) {
        auto mod = sdk::GetGameModule();
        if (mod.valid) sdk::player::Init(mod.base, mod.size);
        LoadConfig();
        g_gameInit = true;
        sdk::Log("Game init: offsets %s (factory=0x%08X)",
                 sdk::player::g_ready ? "OK" : "FAILED",
                 (unsigned)sdk::player::g_pedFactory);
    }

    /* Toggle key */
    static bool keyPrev = false;
    bool keyNow = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (keyNow && !keyPrev) g_visible = !g_visible;
    keyPrev = keyNow;

    /* Render if visible and in vehicle */
    if (g_visible && sdk::player::g_ready) {
        float speed = sdk::player::GetSpeed();
        if (speed >= 0.f) Render(dev, speed);
    }
}

/* ==========================================================================
 * Init Thread
 * ========================================================================== */
static DWORD WINAPI InitThread(LPVOID) {
    sdk::LogOpen("SpeedoIV-CE.log");
    sdk::Log("SpeedoIV-CE v2.0 starting...");

    Sleep(10000); /* Wait for game + DXVK init */

    auto mod = sdk::GetGameModule();
    if (!mod.valid) { sdk::Log("FATAL: game module not found"); return 0; }
    sdk::Log("Game module: base=0x%08X size=0x%08X", (unsigned)mod.base, (unsigned)mod.size);

    for (int i = 0; i < 10; i++) {
        sdk::Log("Hook attempt %d...", i + 1);
        if (d3d9hook::Install(mod.base, mod.size)) break;
        Sleep(3000);
    }

    return 0;
}

/* ==========================================================================
 * Entry Point
 * ========================================================================== */
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        d3d9hook::SetCallback(OnFrame);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        ReleaseResources();
        sdk::LogClose();
    }
    return TRUE;
}
