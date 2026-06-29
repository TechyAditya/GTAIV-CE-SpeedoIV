/*
 * SpeedoIV-CE - Simple Speedometer for GTA IV Complete Edition (1.2.0.43)
 *
 * A lightweight ASI plugin that displays vehicle speed as a text HUD overlay.
 * Compatible with FusionFix (DXVK) and the Complete Edition.
 *
 * Rendering: Uses a transparent Win32 overlay window (WS_EX_LAYERED +
 * WS_EX_TRANSPARENT) drawn on top of the game window. This avoids any
 * D3D9/DXVK hooking conflicts with FusionFix.
 *
 * Game data: Pattern-scans GTAIV.exe for CPedFactory to read vehicle speed.
 *
 * Built with MinGW GCC (i686).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */
static const int   FONT_SIZE           = 30;
static const char  FONT_NAME[]         = "Consolas";
static const int   MARGIN_RIGHT        = 185;   /* px from right edge       */
static const int   MARGIN_BOTTOM       = 55;    /* px from bottom edge      */
static const COLORREF TEXT_COLOR       = RGB(255, 255, 255);
static const COLORREF SHADOW_COLOR     = RGB(0, 0, 0);
static const COLORREF BG_COLOR         = RGB(30, 30, 30);
static const BYTE     BG_ALPHA         = 180;   /* background opacity 0-255 */
static const float KMH_MULTIPLIER      = 3.6f;  /* m/s -> km/h              */
static const int   OVERLAY_FPS         = 30;    /* refresh rate of overlay  */

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */
static HMODULE   g_hModule     = NULL;
static HWND      g_hOverlay    = NULL;
static HWND      g_hGameWnd    = NULL;
static HFONT     g_hFont       = NULL;
static bool      g_showSpeedo  = true;
static bool      g_running     = true;

/* Game memory */
static uintptr_t g_gameBase    = 0;
static uintptr_t g_gameSize    = 0;

/* --------------------------------------------------------------------------
 * Safe memory reading (GCC-compatible, no SEH)
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
static uintptr_t FindPattern(uintptr_t base, size_t size,
                             const BYTE* pattern, const char* mask) {
    size_t patLen = strlen(mask);
    if (size < patLen) return 0;
    for (size_t i = 0; i <= size - patLen; i++) {
        bool match = true;
        for (size_t j = 0; j < patLen; j++) {
            if (mask[j] == 'x' && ((const BYTE*)(base + i))[j] != pattern[j]) {
                match = false;
                break;
            }
        }
        if (match) return base + i;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Vehicle Speed Reading
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

static inline bool IsHeapPtr(uintptr_t p) {
    /* Heap pointers in GTA IV CE are well above the image base (~0x00FE0000)
     * and well above the end of static data. Real heap/pool objects live
     * above ~0x04000000 typically. Filter out small values and static data. */
    return p >= 0x04000000 && p < 0x7FFFFFFF;
}

static bool TryFactory(uintptr_t ptrAddr) {
    if (!IsValidPtr(ptrAddr)) return false;
    uintptr_t factoryPtr = 0;
    if (!SafeReadPtr(ptrAddr, &factoryPtr) || !IsHeapPtr(factoryPtr))
        return false;

    /* Factory should have a vtable pointer at offset 0 */
    uintptr_t vtable = 0;
    if (!SafeReadPtr(factoryPtr, &vtable) || !IsValidPtr(vtable))
        return false;

    /* Player ped pointer at +4 must be a heap pointer */
    uintptr_t pedPtr = 0;
    if (!SafeReadPtr(factoryPtr + OFF_PEDFACTORY_PLAYER, &pedPtr) ||
        !IsHeapPtr(pedPtr))
        return false;

    /* The ped itself should have a vtable */
    uintptr_t pedVtable = 0;
    if (!SafeReadPtr(pedPtr, &pedVtable) || !IsValidPtr(pedVtable))
        return false;

    for (int v = 0; v < NUM_VEH_OFFS; v++) {
        uintptr_t vehPtr = 0;
        if (!SafeReadPtr(pedPtr + VEHICLE_OFFSETS[v], &vehPtr))
            continue;
        /* Vehicle: either 0 (on foot) or a valid heap pointer with vtable */
        if (vehPtr == 0) {
            /* Accept on-foot state only if this is the only "clean" match */
            g_pPedFactory  = ptrAddr;
            g_vehicleOff   = VEHICLE_OFFSETS[v];
            g_velocityOff  = 0x70;
            g_offsetsFound = true;
            return true;
        }
        if (IsHeapPtr(vehPtr)) {
            uintptr_t vehVtable = 0;
            if (!SafeReadPtr(vehPtr, &vehVtable) || !IsValidPtr(vehVtable))
                continue;
            /* Final validation: velocity at +0x70 should be small floats */
            float vx = 0, vy = 0, vz = 0;
            if (SafeReadFloat(vehPtr + 0x70, &vx) &&
                SafeReadFloat(vehPtr + 0x74, &vy) &&
                SafeReadFloat(vehPtr + 0x78, &vz)) {
                float spd = sqrtf(vx*vx + vy*vy + vz*vz) * KMH_MULTIPLIER;
                if (spd >= 0.0f && spd <= 500.0f) {
                    g_pPedFactory  = ptrAddr;
                    g_vehicleOff   = VEHICLE_OFFSETS[v];
                    g_velocityOff  = 0x70;
                    g_offsetsFound = true;
                    return true;
                }
            }
            /* Try other velocity offsets */
            for (int vi = 0; vi < NUM_VEL_OFFS; vi++) {
                if (SafeReadFloat(vehPtr + VELOCITY_OFFSETS[vi], &vx) &&
                    SafeReadFloat(vehPtr + VELOCITY_OFFSETS[vi] + 4, &vy) &&
                    SafeReadFloat(vehPtr + VELOCITY_OFFSETS[vi] + 8, &vz)) {
                    float spd = sqrtf(vx*vx + vy*vy + vz*vz) * KMH_MULTIPLIER;
                    if (spd >= 0.0f && spd <= 500.0f) {
                        g_pPedFactory  = ptrAddr;
                        g_vehicleOff   = VEHICLE_OFFSETS[v];
                        g_velocityOff  = VELOCITY_OFFSETS[vi];
                        g_offsetsFound = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool FindGamePointers() {
    if (g_gameBase == 0) return false;

    /* Pattern 1: A1 ?? ?? ?? ?? 85 C0 74 */
    {
        BYTE pat[] = { 0xA1,0x00,0x00,0x00,0x00, 0x85,0xC0, 0x74 };
        char msk[] = "x????xxx";
        uintptr_t cur = g_gameBase;
        size_t rem = g_gameSize;
        for (int n = 0; n < 80 && cur < g_gameBase + g_gameSize - 8; n++) {
            uintptr_t hit = FindPattern(cur, rem, pat, msk);
            if (!hit) break;
            uintptr_t candidate = *(uintptr_t*)(hit + 1);
            if (TryFactory(candidate)) return true;
            cur = hit + 1;
            rem = g_gameBase + g_gameSize - cur;
        }
    }

    /* Pattern 2: 8B 0D ?? ?? ?? ?? 8B 41 04 */
    {
        BYTE pat[] = { 0x8B,0x0D,0x00,0x00,0x00,0x00, 0x8B,0x41,0x04 };
        char msk[] = "xx????xxx";
        uintptr_t cur = g_gameBase;
        size_t rem = g_gameSize;
        for (int n = 0; n < 80 && cur < g_gameBase + g_gameSize - 10; n++) {
            uintptr_t hit = FindPattern(cur, rem, pat, msk);
            if (!hit) break;
            uintptr_t candidate = *(uintptr_t*)(hit + 2);
            if (TryFactory(candidate)) return true;
            cur = hit + 1;
            rem = g_gameBase + g_gameSize - cur;
        }
    }

    /* Pattern 3: 8B 35 ?? ?? ?? ?? 85 F6 74 (mov esi,[addr]; test esi,esi; jz) */
    {
        BYTE pat[] = { 0x8B,0x35,0x00,0x00,0x00,0x00, 0x85,0xF6, 0x74 };
        char msk[] = "xx????xxx";
        uintptr_t cur = g_gameBase;
        size_t rem = g_gameSize;
        for (int n = 0; n < 80 && cur < g_gameBase + g_gameSize - 10; n++) {
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
    if (vehPtr == 0) return -1.0f;
    if (!IsValidPtr(vehPtr)) return -1.0f;

    float vx, vy, vz;
    uintptr_t vBase = vehPtr + g_velocityOff;
    if (!SafeReadFloat(vBase,     &vx) ||
        !SafeReadFloat(vBase + 4, &vy) ||
        !SafeReadFloat(vBase + 8, &vz))
        return -1.0f;

    float speed = sqrtf(vx*vx + vy*vy + vz*vz) * KMH_MULTIPLIER;
    if (speed >= 0.0f && speed <= 500.0f)
        return speed;

    /* Probe alternate velocity offsets */
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
 * Overlay Window
 *
 * A transparent, click-through, topmost window positioned over the game.
 * Redrawn at OVERLAY_FPS using GDI with alpha blending via UpdateLayeredWindow.
 * -------------------------------------------------------------------------- */

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static HWND FindGameWindow() {
    /* Find the main GTA IV window */
    HWND h = FindWindowA("grcWindow", NULL);  /* RAGE game window class */
    if (!h) h = FindWindowA(NULL, "GTAIV");
    if (!h) h = FindWindowA(NULL, "GTA IV");
    return h;
}

static void DrawOverlay(float speed) {
    if (!g_hOverlay || !g_hGameWnd) return;

    /* Get game window position and size */
    RECT rc;
    GetClientRect(g_hGameWnd, &rc);
    POINT pt = { 0, 0 };
    ClientToScreen(g_hGameWnd, &pt);
    int gw = rc.right - rc.left;
    int gh = rc.bottom - rc.top;

    /* Position overlay to match game window */
    SetWindowPos(g_hOverlay, HWND_TOPMOST,
                 pt.x, pt.y, gw, gh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    /* Create off-screen DC and bitmap for UpdateLayeredWindow */
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, gw, gh);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    /* Clear with fully transparent */
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    /* Use GDI+ style: fill bitmap with 0 alpha everywhere */
    /* For simplicity, use a BITMAPINFO to create a 32bpp DIB section */
    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = gw;
    bmi.bmiHeader.biHeight = -gh; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBmp || !pBits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }
    hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    /* Clear to transparent black */
    memset(pBits, 0, gw * gh * 4);

    /* Draw speed text with manual alpha */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d KM/H", (int)speed);
    int len = (int)strlen(buf);

    /* Calculate text position */
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, g_hFont);
    SIZE textSize;
    GetTextExtentPoint32A(hdcMem, buf, len, &textSize);

    int tx = gw - MARGIN_RIGHT - textSize.cx / 2;
    int ty = gh - MARGIN_BOTTOM - textSize.cy;

    /* Draw a rounded-ish background rectangle with alpha */
    int pad = 8;
    RECT bgRect = { tx - pad, ty - pad/2,
                    tx + textSize.cx + pad, ty + textSize.cy + pad/2 };

    /* Fill background pixels with semi-transparent color */
    BYTE bgR = GetRValue(BG_COLOR);
    BYTE bgG = GetGValue(BG_COLOR);
    BYTE bgB = GetBValue(BG_COLOR);
    uint32_t* pixels = (uint32_t*)pBits;
    for (int y = bgRect.top; y < bgRect.bottom && y < gh; y++) {
        if (y < 0) continue;
        for (int x = bgRect.left; x < bgRect.right && x < gw; x++) {
            if (x < 0) continue;
            /* Pre-multiplied alpha for UpdateLayeredWindow */
            BYTE a = BG_ALPHA;
            pixels[y * gw + x] =
                ((DWORD)a << 24) |
                ((DWORD)((bgR * a) / 255) << 16) |
                ((DWORD)((bgG * a) / 255) << 8) |
                ((DWORD)((bgB * a) / 255));
        }
    }

    /* Draw text with shadow using GDI (alpha = 255 for text pixels) */
    SetBkMode(hdcMem, TRANSPARENT);

    /* Shadow */
    SetTextColor(hdcMem, SHADOW_COLOR);
    TextOutA(hdcMem, tx + 1, ty + 1, buf, len);
    /* Main text */
    SetTextColor(hdcMem, TEXT_COLOR);
    TextOutA(hdcMem, tx, ty, buf, len);

    SelectObject(hdcMem, hOldFont);

    /* Fix alpha channel for text pixels (GDI sets alpha to 0) */
    /* Set alpha=255 for any pixel that GDI wrote (non-zero RGB where bg was 0
       or different from bg) */
    for (int y = (ty - 2 > 0 ? ty - 2 : 0);
         y < ty + textSize.cy + 4 && y < gh; y++) {
        for (int x = (tx - 2 > 0 ? tx - 2 : 0);
             x < tx + textSize.cx + 4 && x < gw; x++) {
            uint32_t px = pixels[y * gw + x];
            BYTE r = (px >> 16) & 0xFF;
            BYTE g = (px >> 8) & 0xFF;
            BYTE b = px & 0xFF;
            BYTE a = (px >> 24) & 0xFF;
            /* If pixel has color but no alpha, it was drawn by GDI text */
            if ((r || g || b) && a == 0) {
                /* Set to fully opaque, pre-multiplied */
                pixels[y * gw + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    /* Update the layered window */
    POINT srcPt = { 0, 0 };
    SIZE wndSize = { gw, gh };
    POINT dstPt = { pt.x, pt.y };

    UpdateLayeredWindow(g_hOverlay, hdcScreen,
                        &dstPt, &wndSize,
                        hdcMem, &srcPt,
                        0, &blend, ULW_ALPHA);

    /* Cleanup */
    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static void HideOverlay() {
    if (g_hOverlay) ShowWindow(g_hOverlay, SW_HIDE);
}

/* --------------------------------------------------------------------------
 * Main overlay thread
 * -------------------------------------------------------------------------- */
static DWORD WINAPI OverlayThread(LPVOID) {
    /* Wait for game to initialize */
    Sleep(8000);

    /* Resolve game module */
    HMODULE hGame = GetModuleHandleA("GTAIV.exe");
    if (hGame) {
        g_gameBase = (uintptr_t)hGame;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
        IMAGE_NT_HEADERS* nt =
            (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
        g_gameSize = nt->OptionalHeader.SizeOfImage;
    }

    /* Find game pointers - retry until game is fully loaded */
    for (int attempt = 0; attempt < 30 && !g_offsetsFound; attempt++) {
        FindGamePointers();
        if (!g_offsetsFound) Sleep(2000);
    }

    /* Find game window */
    for (int attempt = 0; attempt < 30 && !g_hGameWnd; attempt++) {
        g_hGameWnd = FindGameWindow();
        if (!g_hGameWnd) Sleep(1000);
    }
    if (!g_hGameWnd) return 0;

    /* Create overlay window */
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance      = g_hModule;
    wc.lpszClassName  = "SpeedoIVCE_Overlay";
    RegisterClassExA(&wc);

    g_hOverlay = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, "SpeedoIV-CE",
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, g_hModule, NULL);

    if (!g_hOverlay) return 0;

    /* Create font */
    g_hFont = CreateFontA(
        FONT_SIZE, 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        FONT_NAME);

    /* Main loop */
    DWORD frameTime = 1000 / OVERLAY_FPS;
    while (g_running) {
        /* Check game window still exists */
        if (!IsWindow(g_hGameWnd)) {
            g_hGameWnd = FindGameWindow();
            if (!g_hGameWnd) { Sleep(1000); continue; }
        }

        /* Toggle with F5 */
        static bool f5Prev = false;
        bool f5Now = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Now && !f5Prev) g_showSpeedo = !g_showSpeedo;
        f5Prev = f5Now;

        /* Check if game window is foreground (don't show over other apps) */
        HWND fg = GetForegroundWindow();
        bool gameActive = (fg == g_hGameWnd);

        if (g_showSpeedo && g_offsetsFound && gameActive) {
            float speed = GetVehicleSpeed();
            if (speed >= 0.0f) {
                DrawOverlay(speed);
            } else {
                HideOverlay();
            }
        } else {
            HideOverlay();
        }

        Sleep(frameTime);
    }

    /* Cleanup */
    if (g_hOverlay) DestroyWindow(g_hOverlay);
    UnregisterClassA("SpeedoIVCE_Overlay", g_hModule);
    if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }

    return 0;
}

/* --------------------------------------------------------------------------
 * DLL Entry Point
 * -------------------------------------------------------------------------- */
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        Sleep(100);
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
    }
    return TRUE;
}
