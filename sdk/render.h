/*
 * sdk/render.h - Reusable D3DX9 sprite-renderer helpers
 *
 * Pulls out the bits any HUD-style overlay mod needs:
 *   - texture loading with DXVK-compatible flags
 *   - render-state reset that survives GTA IV's post-processed device state
 *   - a 2D transform helper that handles the
 *     scale-then-rotate-around-scaled-centre case correctly (D3DXMatrixTransformation2D
 *     is full of footguns; this wraps the correct usage).
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include "logging.h"

namespace sdk {
namespace render {

/* Load a PNG/TGA/etc. texture in a way that works under DXVK and FusionFix.
 * D3DPOOL_DEFAULT is required (D3DPOOL_MANAGED can hang DXVK on Reset). */
inline bool LoadTexture(IDirect3DDevice9* dev, const char* path,
                        IDirect3DTexture9** outTex) {
    if (!dev || !path || !outTex) return false;
    HRESULT hr = D3DXCreateTextureFromFileExA(
        dev, path,
        D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
        D3DFMT_UNKNOWN, D3DPOOL_DEFAULT,
        D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, outTex);
    if (FAILED(hr)) {
        sdk::LogError("LoadTexture: failed %s (hr=0x%08X)", path, (unsigned)hr);
        return false;
    }
    return true;
}

inline bool GetTextureSize(IDirect3DTexture9* tex, int* w, int* h, D3DFORMAT* fmt = nullptr) {
    if (!tex) return false;
    D3DSURFACE_DESC desc;
    if (FAILED(tex->GetLevelDesc(0, &desc))) return false;
    if (w)   *w   = (int)desc.Width;
    if (h)   *h   = (int)desc.Height;
    if (fmt) *fmt = desc.Format;
    return true;
}

/* Reset render state to a known-good 2D blit configuration. GTA IV leaves
 * the device with tonemap pixel shaders, depth-test on, etc., which causes
 * sprite draws to appear black or get culled. Call this before
 * ID3DXSprite::Begin every frame. */
inline void Reset2DState(IDirect3DDevice9* dev) {
    if (!dev) return;
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
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
}

/* Build a 2D transform matrix that:
 *   - scales the texture by (scaleX, scaleY)
 *   - rotates it around the scaled visual centre by `radians`
 *   - places that scaled visual centre at (centreX, centreY) on screen
 *
 * `texW, texH` are the source texture dimensions in pixels.
 *
 * The math is non-obvious because D3DXMatrixTransformation2D interprets the
 * rotation centre in pre-scale texel coords but applies it AFTER scaling.
 * Workaround: pass scaling-centre = (0,0) and rotation-centre in scaled
 * coords, then translate so the scaled centre lands at the requested point.
 */
inline void BuildSpriteTransform(D3DXMATRIX* out,
                                 float texW, float texH,
                                 float scaleX, float scaleY,
                                 float centreX, float centreY,
                                 float radians) {
    D3DXVECTOR2 zero(0.f, 0.f);
    D3DXVECTOR2 scale(scaleX, scaleY);
    D3DXVECTOR2 rotCentre(texW * 0.5f * scaleX, texH * 0.5f * scaleY);
    D3DXVECTOR2 pos(centreX - rotCentre.x, centreY - rotCentre.y);
    D3DXMatrixTransformation2D(out, &zero, 0.f, &scale, &rotCentre, radians, &pos);
}

/* Convert an alignment code like "BL" / "TR" / "BR" / "TL" + per-corner
 * offsets into a top-left screen-pixel destination for a sprite of size
 * (sizeX, sizeY). For B/T alignment, posY is distance from the edge.
 * For B alignment posY moves the sprite UP (toward centre). */
inline void ResolveAlignment(const char* alignCode,
                             float scrW, float scrH,
                             float sizeX, float sizeY,
                             float posX, float posY,
                             float* outX, float* outY) {
    char v = alignCode && alignCode[0] ? alignCode[0] : 'T';
    char h = alignCode && alignCode[0] ? alignCode[1] : 'L';
    if (v == 'B' || v == 'b') *outY = scrH - sizeY - posY;
    else                      *outY = posY;
    if (h == 'R' || h == 'r') *outX = scrW - sizeX - posX;
    else                      *outX = posX;
}

} // namespace render
} // namespace sdk
