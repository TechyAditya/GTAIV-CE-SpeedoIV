/*
 * SpeedoIV-CE - Speedometer overlay for GTA IV Complete Edition (1.2.0.43)
 *
 * Reads live vehicle velocity, hides during pause/menu/cutscene, supports
 * live-tunable position/size/sweep from Config.ini and a toggle hotkey.
 *
 * Credits:
 *   - Bck.png / Pin.png assets and the original SpeedoIV concept
 *     (c) retarded_chicken (gtaiv-mods.com, 2009). Logic was NOT used,
 *     but the artwork remains.
 *   - FindPlayerPed / FindPlayerVehicle / pause-state patterns
 *     derived from FusionFix (https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
 *     by ThirteenAG, GPL-3.0.
 *
 * Build: see build.ps1 (32-bit MinGW i686 + D3DX9).
 */

#define _USE_MATH_DEFINES
#include "sdk/all.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==========================================================================
 * Config
 * ========================================================================== */
struct Config {
    bool  debug       = false;
    bool  autostart   = true;
    int   toggleKey   = VK_F6;
    float texSizeX    = 1024.f;
    float texSizeY    = 1024.f;
    bool  kmh         = true;
    float maxSpeed    = 280.f;
    char  align[4]    = "BL";
    float posX        = 90.f;
    float posY        = 12.f;
    float sizeX       = 440.f;
    float sizeY       = 440.f;
    float angle       = 0.f;
    float sweepDeg    = 280.f;
    int   alpha       = 220;
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
static bool               g_resOk  = false;

/* ==========================================================================
 * Config Loading
 * ========================================================================== */
static void GetIniPath(char* out, size_t cap) {
    char base[MAX_PATH];
    GetModuleFileNameA(g_hModule, base, MAX_PATH);
    char* s = strrchr(base, '\\'); if (s) *s = 0;   /* drop SpeedoIV-CE.asi */
    s = strrchr(base, '\\'); if (s) *s = 0;          /* drop plugins\ */
    snprintf(out, cap, "%s\\SpeedoIV\\Config.ini", base);
}

static void GetSkinDir(char* out, size_t cap, const char* skin) {
    char base[MAX_PATH];
    GetModuleFileNameA(g_hModule, base, MAX_PATH);
    char* s = strrchr(base, '\\'); if (s) *s = 0;
    s = strrchr(base, '\\'); if (s) *s = 0;
    snprintf(out, cap, "%s\\SpeedoIV\\%s", base, skin);
}

static void LoadConfig() {
    char ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);

    auto getF = [&](const char* key, float def) -> float {
        char buf[32]; char defStr[32];
        snprintf(defStr, 32, "%.2f", def);
        GetPrivateProfileStringA("Config", key, defStr, buf, 32, ini);
        return (float)atof(buf);
    };
    auto getBool = [&](const char* key, bool def) -> bool {
        char buf[16];
        GetPrivateProfileStringA("Config", key, def ? "true" : "false", buf, 16, ini);
        return (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0 ||
                _stricmp(buf, "yes")  == 0 || _stricmp(buf, "on") == 0);
    };

    g_cfg.debug     = getBool("Debug", false);
    g_cfg.autostart = getBool("Autostart", true);
    g_cfg.toggleKey = GetPrivateProfileIntA("Config", "ToggleKey", VK_F6, ini);
    g_cfg.texSizeX  = getF("TexSizeX", 1024.f);
    g_cfg.texSizeY  = getF("TexSizeY", 1024.f);
    g_cfg.kmh       = getBool("EnableKMH", true);
    g_cfg.maxSpeed  = getF("MaxSpeed", 280.f);
    g_cfg.posX      = getF("PositionX", 90.f);
    g_cfg.posY      = getF("PositionY", 12.f);
    g_cfg.sizeX     = getF("SizeX", 440.f);
    g_cfg.sizeY     = getF("SizeY", 440.f);
    g_cfg.angle     = getF("Angle", 0.f);
    g_cfg.sweepDeg  = getF("SweepDeg", 280.f);
    g_cfg.alpha     = GetPrivateProfileIntA("Config", "Alpha", 220, ini);
    GetPrivateProfileStringA("Config", "SkinFolder",  "Default", g_cfg.skin,  64, ini);
    GetPrivateProfileStringA("Config", "ScreenAlign", "BL",      g_cfg.align,  4, ini);

    /* Propagate Debug flag to logger -- responds live so user can flip it
     * mid-session and start seeing logs immediately. */
    sdk::SetDebug(g_cfg.debug);

    static bool firstLoad = true;
    if (firstLoad) {
        g_visible = g_cfg.autostart;
        sdk::Log("Config loaded: debug=%d autostart=%d kmh=%d maxSpeed=%.0f skin=%s align=%s size=%.0fx%.0f pos=%.1f,%.1f sweep=%.0f toggleKey=%d",
                 g_cfg.debug, g_cfg.autostart, g_cfg.kmh, g_cfg.maxSpeed,
                 g_cfg.skin, g_cfg.align, g_cfg.sizeX, g_cfg.sizeY,
                 g_cfg.posX, g_cfg.posY, g_cfg.sweepDeg, g_cfg.toggleKey);
        firstLoad = false;
    }
}

/* ==========================================================================
 * D3D9 Resource Init
 * ========================================================================== */
static bool InitResources(IDirect3DDevice9* dev) {
    if (g_resOk) return true;

    char skinDir[MAX_PATH];
    GetSkinDir(skinDir, MAX_PATH, g_cfg.skin);
    char bck[MAX_PATH], pin[MAX_PATH];
    snprintf(bck, MAX_PATH, "%s\\Bck.png", skinDir);
    snprintf(pin, MAX_PATH, "%s\\Pin.png", skinDir);
    sdk::Log("Loading textures from %s", skinDir);

    if (FAILED(D3DXCreateSprite(dev, &g_sprite))) {
        sdk::LogError("D3DXCreateSprite failed"); return false;
    }
    if (!sdk::render::LoadTexture(dev, bck, &g_texBck)) return false;
    if (!sdk::render::LoadTexture(dev, pin, &g_texPin)) return false;

    int tw = 0, th = 0;
    if (sdk::render::GetTextureSize(g_texBck, &tw, &th)) {
        sdk::Log("Bck texture: %dx%d", tw, th);
        g_cfg.texSizeX = (float)tw;
        g_cfg.texSizeY = (float)th;
    }

    g_resOk = true;
    sdk::Log("D3D resources loaded OK");
    return true;
}

static void ReleaseResources() {
    if (g_sprite) { g_sprite->Release(); g_sprite = NULL; }
    if (g_texBck) { g_texBck->Release(); g_texBck = NULL; }
    if (g_texPin) { g_texPin->Release(); g_texPin = NULL; }
    g_resOk = false;
}

/* ==========================================================================
 * Render
 * ========================================================================== */
static IDirect3DDevice9* g_lastDev = NULL;

static void Render(IDirect3DDevice9* dev, float speed) {
    HRESULT coop = dev->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) {
        if (g_resOk) { sdk::Log("Device lost - releasing"); ReleaseResources(); }
        return;
    }
    if (coop == D3DERR_DEVICENOTRESET) return;

    IDirect3DSwapChain9* swap = NULL;
    if (SUCCEEDED(dev->GetSwapChain(0, &swap))) {
        D3DPRESENT_PARAMETERS pp = {};
        swap->GetPresentParameters(&pp);
        swap->Release();
        if (pp.hDeviceWindow && (!IsWindowVisible(pp.hDeviceWindow) || IsIconic(pp.hDeviceWindow))) {
            if (g_resOk) { sdk::Log("Window hidden - releasing"); ReleaseResources(); }
            return;
        }
    }

    if (dev != g_lastDev) {
        if (g_lastDev != NULL && g_resOk) ReleaseResources();
        g_lastDev = dev;
    }
    if (!InitResources(dev)) return;

    sdk::render::Reset2DState(dev);

    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    float scrW = (float)vp.Width, scrH = (float)vp.Height;

    float drawX = 0, drawY = 0;
    sdk::render::ResolveAlignment(g_cfg.align, scrW, scrH,
                                  g_cfg.sizeX, g_cfg.sizeY,
                                  g_cfg.posX,  g_cfg.posY,
                                  &drawX, &drawY);

    float ratio = speed / g_cfg.maxSpeed;
    if (ratio < 0.f) ratio = 0.f;
    if (ratio > 1.f) ratio = 1.f;
    float needleRad = ratio * g_cfg.sweepDeg * (float)M_PI / 180.f;
    float userRad   = g_cfg.angle * (float)M_PI / 180.f;

    float scaleX = g_cfg.sizeX / g_cfg.texSizeX;
    float scaleY = g_cfg.sizeY / g_cfg.texSizeY;
    float cx = drawX + g_cfg.sizeX * 0.5f;
    float cy = drawY + g_cfg.sizeY * 0.5f;

    DWORD bgColor  = D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255);
    DWORD pinColor = D3DCOLOR_ARGB(g_cfg.alpha, 255, 255, 255);

    D3DXMATRIX mat;
    g_sprite->Begin(D3DXSPRITE_ALPHABLEND);

    sdk::render::BuildSpriteTransform(&mat, g_cfg.texSizeX, g_cfg.texSizeY,
                                      scaleX, scaleY, cx, cy, userRad);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texBck, NULL, NULL, NULL, bgColor);

    sdk::render::BuildSpriteTransform(&mat, g_cfg.texSizeX, g_cfg.texSizeY,
                                      scaleX, scaleY, cx, cy,
                                      needleRad + userRad);
    g_sprite->SetTransform(&mat);
    g_sprite->Draw(g_texPin, NULL, NULL, NULL, pinColor);

    g_sprite->End();
}

/* ==========================================================================
 * Frame Callback (called by d3d9_hook every EndScene)
 * ========================================================================== */
static bool g_gameInit = false;

static void OnFrame(IDirect3DDevice9* dev) {
    if (!g_gameInit) {
        auto mod = sdk::GetGameModule();
        if (mod.valid) {
            sdk::player::Init(mod.base, mod.size);
            sdk::ui::Init(mod.base, mod.size);
        }
        LoadConfig();
        g_gameInit = true;
        sdk::Log("Game init: player=%s ui=%s (FindPlayerPed=0x%08X FindPlayerVehicle=0x%08X UserPause=0x%08X CodePause=0x%08X MenuActive=0x%08X)",
                 sdk::player::g_ready ? "OK" : "FAILED",
                 sdk::ui::g_ready ? "OK" : "FAILED",
                 (unsigned)(uintptr_t)sdk::player::g_findPlayerPed,
                 (unsigned)(uintptr_t)sdk::player::g_findPlayerVehicle,
                 (unsigned)(uintptr_t)sdk::ui::g_userPause,
                 (unsigned)(uintptr_t)sdk::ui::g_codePause,
                 (unsigned)(uintptr_t)sdk::ui::g_menuActive);
        if (!sdk::player::g_ready) sdk::LogError("Player module init failed");
        if (!sdk::ui::g_ready)     sdk::LogWarn("UI/pause module init failed -- speedometer will not auto-hide on pause");
    }

    /* Live-reload Config.ini every ~1 second so the user can tune without restarting */
    static int reloadFrame = 0;
    if (++reloadFrame % 60 == 0) LoadConfig();

    /* Toggle key (edge-triggered) */
    static bool keyPrev = false;
    bool keyNow = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (keyNow && !keyPrev) {
        g_visible = !g_visible;
        sdk::Log("Toggle: visible=%d", g_visible);
    }
    keyPrev = keyNow;

    /* Heartbeat log (Debug=true only via ring buffer otherwise) */
    static int frame = 0;
    if (++frame % 300 == 1) {
        float spd = sdk::player::g_ready ? sdk::player::GetSpeed() : -99.0f;
        sdk::Log("Frame %d: visible=%d speed=%.1f velOff=0x%X resOk=%d",
                 frame, g_visible, spd, sdk::player::g_velOffset, g_resOk);
    }

    /* Render only when visible, player is loaded, actually in a vehicle,
     * and the game is not paused/menu/cutscene. */
    if (g_visible && sdk::player::g_ready) {
        float speed = sdk::player::GetSpeed();
        if (speed >= 0.f && !sdk::ui::IsGamePaused()) {
            Render(dev, speed);
        }
    }
}

/* ==========================================================================
 * Init Thread
 * ========================================================================== */
static DWORD WINAPI InitThread(LPVOID) {
    /* LogOpen sets target filename, but defers the actual fopen.
     * Debug flag isn't known yet -- we'll get it from Config.ini on first
     * frame and either truncate (Debug=true) or stay in lazy mode. */
    sdk::LogOpen("SpeedoIV-CE.log", /*debug=*/false);
    sdk::Log("SpeedoIV-CE starting...");

    Sleep(10000);  /* wait for game + DXVK init */

    auto mod = sdk::GetGameModule();
    if (!mod.valid) { sdk::LogError("Game module not found"); return 0; }
    sdk::Log("Game module: base=0x%08X size=0x%08X", (unsigned)mod.base, (unsigned)mod.size);

    for (int i = 0; i < 10; i++) {
        sdk::Log("Hook attempt %d...", i + 1);
        if (d3d9hook::Install(mod.base, mod.size)) break;
        Sleep(3000);
    }
    return 0;
}

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
