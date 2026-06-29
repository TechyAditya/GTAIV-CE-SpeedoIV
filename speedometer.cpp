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

    /* Quiet by default - only log once at startup */
    static bool firstLoad = true;
    if (firstLoad) {
        g_visible = g_cfg.autostart;
        sdk::Log("Config loaded: autostart=%d kmh=%d maxSpeed=%.0f skin=%s align=%s size=%.0fx%.0f pos=%.1f,%.1f",
                 g_cfg.autostart, g_cfg.kmh, g_cfg.maxSpeed, g_cfg.skin, g_cfg.align,
                 g_cfg.sizeX, g_cfg.sizeY, g_cfg.posX, g_cfg.posY);
        firstLoad = false;
    }
    /* Don't overwrite g_visible on reload -- user F5 toggle should persist */
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
    /* Use the modern skin textures */
    snprintf(bck, MAX_PATH, "%s\\SpeedoIV\\%s\\Bck.png", base, g_cfg.skin);
    snprintf(pin, MAX_PATH, "%s\\SpeedoIV\\%s\\Pin.png", base, g_cfg.skin);
    sdk::Log("Loading bck: %s", bck);
    sdk::Log("Loading pin: %s", pin);

    if (FAILED(D3DXCreateSprite(dev, &g_sprite))) {
        sdk::Log("Failed: D3DXCreateSprite"); return false;
    }
    /* Load textures preserving original format, with D3DPOOL_DEFAULT for DXVK */
    if (FAILED(D3DXCreateTextureFromFileExA(dev, bck,
            D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &g_texBck))) {
        sdk::Log("Failed: load %s", bck); return false;
    }
    if (FAILED(D3DXCreateTextureFromFileExA(dev, pin,
            D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &g_texPin))) {
        sdk::Log("Failed: load %s", pin); return false;
    }
    /* Get texture dimensions to use as texSize (auto-detect) */
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(g_texBck->GetLevelDesc(0, &desc))) {
        sdk::Log("Bck texture: %dx%d format=%d", desc.Width, desc.Height, desc.Format);
        g_cfg.texSizeX = (float)desc.Width;
        g_cfg.texSizeY = (float)desc.Height;
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
static IDirect3DDevice9* g_lastDev = NULL;

static void Render(IDirect3DDevice9* dev, float speed) {
    /* Check device state - DXVK reports DEVICELOST on alt+tab from fullscreen */
    HRESULT coop = dev->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) {
        if (g_resOk) { sdk::Log("Device lost - releasing"); ReleaseResources(); }
        return;
    }
    if (coop == D3DERR_DEVICENOTRESET) return;

    /* Get the device's focus window from its swapchain - if it's not 
     * visible/active, skip rendering to avoid DXVK crashes */
    IDirect3DSwapChain9* swap = NULL;
    if (SUCCEEDED(dev->GetSwapChain(0, &swap))) {
        D3DPRESENT_PARAMETERS pp = {};
        swap->GetPresentParameters(&pp);
        swap->Release();
        if (pp.hDeviceWindow) {
            if (!IsWindowVisible(pp.hDeviceWindow) || IsIconic(pp.hDeviceWindow)) {
                if (g_resOk) { sdk::Log("Window hidden - releasing"); ReleaseResources(); }
                return;
            }
        }
    }

    /* Device change detection */
    if (dev != g_lastDev) {
        if (g_lastDev != NULL && g_resOk) ReleaseResources();
        g_lastDev = dev;
    }

    if (!InitResources(dev)) return;

    /* Force clean render states - GTA leaves the device in weird state after
     * its rendering passes (tonemap, depth-of-field). Reset to defaults. */
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    /* Disable any pixel shaders that might be active */
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);
    /* Reset texture stage state */
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    float scrW = (float)vp.Width, scrH = (float)vp.Height;

    /* Position with intuitive offsets:
     * - For BL/BR: posY moves UP from bottom (positive = up), posX moves RIGHT for BL or LEFT for BR
     * - For TL/TR: posY moves DOWN from top */
    float drawX, drawY;
    if (g_cfg.align[0] == 'B' || g_cfg.align[0] == 'b')
        drawY = scrH - g_cfg.sizeY - g_cfg.posY;
    else
        drawY = g_cfg.posY;
    if (g_cfg.align[1] == 'R' || g_cfg.align[1] == 'r')
        drawX = scrW - g_cfg.sizeX - g_cfg.posX;
    else
        drawX = g_cfg.posX;

    /* Pin already points at the "0" position in the texture.
     * Rotate it clockwise from 0 (speed=0) to +270deg (speed=maxSpeed). */
    float ratio = speed / g_cfg.maxSpeed;
    if (ratio > 1.f) ratio = 1.f;
    float needle = ratio * 270.f * (float)M_PI / 180.f;

    /* Use texture-pixel center for scaling and rotation pivots so that
     * a (texSize/2, texSize/2) point in the texture is at the screen position
     * (drawX + sizeX/2, drawY + sizeY/2) after scaling. */
    D3DXVECTOR2 scalingCenter(0.f, 0.f);
    D3DXVECTOR2 rotCenter(g_cfg.texSizeX / 2.f, g_cfg.texSizeY / 2.f);
    D3DXVECTOR2 pos(drawX, drawY);
    D3DXVECTOR2 scale(g_cfg.sizeX / g_cfg.texSizeX, g_cfg.sizeY / g_cfg.texSizeY);
    DWORD color = D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255);

    /* Use ALPHABLEND but no SORT_TEXTURE which may interfere with DXVK */
    g_sprite->Begin(D3DXSPRITE_ALPHABLEND);

    /* Background - explicit white color, full alpha to test rendering */
    DWORD bgColor = D3DCOLOR_ARGB(255, 255, 255, 255);
    DWORD pinColor = D3DCOLOR_ARGB(255, 255, 255, 255);

    D3DXMATRIX mat;
    D3DXMatrixTransformation2D(&mat, &scalingCenter, 0.f, &scale, &rotCenter,
                                g_cfg.angle * (float)M_PI / 180.f, &pos);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texBck, NULL, NULL, NULL, bgColor);

    /* Needle */
    D3DXMatrixTransformation2D(&mat, &scalingCenter, 0.f, &scale, &rotCenter,
                                needle + g_cfg.angle * (float)M_PI / 180.f, &pos);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texPin, NULL, NULL, NULL, pinColor);

    g_sprite->End();
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
        sdk::Log("Game init: offsets %s (factory=0x%08X vehOff=0x%X velOff=0x%X)",
                 sdk::player::g_ready ? "OK" : "FAILED",
                 (unsigned)sdk::player::g_pedFactory,
                 sdk::player::g_vehOffset, sdk::player::g_velOffset);
    }

    /* Live-reload Config.ini every ~1 second so you can tune without restarting */
    static int reloadFrame = 0;
    if (++reloadFrame % 60 == 0) {
        LoadConfig();
    }

    /* Re-scan if we're locked on a wrong factory (always reads near-zero in vehicle).
     * If speed stays under 1 km/h for > 600 frames (~10s), re-scan. */
    static int rescanFrame = 0;
    static int lowSpeedFrames = 0;
    static int highSpeedFrames = 0;
    if (sdk::player::g_ready) {
        float s = sdk::player::GetSpeed();
        if (s < 0.0f) {
            lowSpeedFrames = 0; highSpeedFrames = 0;
        } else if (s < 1.0f) {
            lowSpeedFrames++;
            if (lowSpeedFrames > 600 && highSpeedFrames < 5) {
                /* Stuck at near-zero with no real movement detected -- wrong factory */
                if (++rescanFrame % 120 == 0) {
                    sdk::player::g_ready = false;
                    auto mod = sdk::GetGameModule();
                    if (mod.valid) sdk::player::Init(mod.base, mod.size);
                    sdk::Log("Re-scan (stuck low): factory=0x%08X vehOff=0x%X velOff=0x%X",
                             (unsigned)sdk::player::g_pedFactory,
                             sdk::player::g_vehOffset, sdk::player::g_velOffset);
                    lowSpeedFrames = 0;
                }
            }
        } else {
            highSpeedFrames++;
            lowSpeedFrames = 0;
        }
    }

    /* Toggle key */
    static bool keyPrev = false;
    bool keyNow = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (keyNow && !keyPrev) {
        g_visible = !g_visible;
        sdk::Log("Toggle: visible=%d", g_visible);
    }
    keyPrev = keyNow;

    /* Periodic status log */
    static int frame = 0;
    if (++frame % 300 == 1) {
        float spd = sdk::player::g_ready ? sdk::player::GetSpeed() : -99.0f;
        sdk::Log("Frame %d: visible=%d speed=%.1f resOk=%d", frame, g_visible, spd, g_resOk);
    }

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

/* Called by hook when the device is being reset (e.g. alt+tab) */
static void OnDeviceLost() {
    sdk::Log("Device reset detected - releasing resources");
    ReleaseResources();
    g_lastDev = NULL;
}

/* ==========================================================================
 * Entry Point
 * ========================================================================== */
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        d3d9hook::SetCallback(OnFrame);
        d3d9hook::SetLostCallback(OnDeviceLost);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        ReleaseResources();
        sdk::LogClose();
    }
    return TRUE;
}
