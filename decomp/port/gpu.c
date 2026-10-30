/* The rendering backend: a Win32 window and a Direct3D 9 device that the Xbox Direct3D layer (port/d3d8.c) draws
 * through. Direct3D 9 is the closest Windows API to what the game expects: the same fixed-function render and
 * texture-stage states, ARGB colours and vertex formats as Direct3D 8 on the Xbox.
 *
 * This file is its own translation unit with the real Windows headers (the other port files use win32_min.h); d3d8.c
 * only sees the gpu_* functions below, with plain C types.
 */
#undef size_t  /* the decompiler build defines size_t as ghidra_size_t */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a.h"

static HWND window;
static IDirect3D9 *d3d;
static IDirect3DDevice9 *device;
static int in_scene;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY)
        ExitProcess(0);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* the window (shown at twice the game's resolution) and a device rendering at the game's resolution */
int gpu_open(unsigned width, unsigned height)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "DRA2Window";
    RegisterClassA(&wc);
    RECT r = { 0, 0, (LONG)width * 2, (LONG)height * 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExA(0, wc.lpszClassName, "Digimon Rumble Arena 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("[gpu] Direct3DCreate9 failed\n");
        return 0;
    }
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;  /* the game paces itself (frame limiter) */
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr)) {
        printf("[gpu] CreateDevice failed: %08lx\n", (unsigned long)hr);
        return 0;
    }
    printf("[gpu] Direct3D 9 device %ux%u\n", width, height);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    for (DWORD s = 0; s < 4; s++) {
        IDirect3DDevice9_SetSamplerState(device, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(device, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    return 1;
}

static void begin_scene(void)
{
    if (device && !in_scene && SUCCEEDED(IDirect3DDevice9_BeginScene(device)))
        in_scene = 1;
}

/* Clear with Xbox flags: D3DCLEAR_ZBUFFER 1, STENCIL 2, TARGET_R/G/B/A 0x10-0x80 */
void gpu_clear(int count, const long *rects, unsigned flags, unsigned color, float z, unsigned stencil)
{
    if (!device)
        return;
    DWORD d9 = 0;
    if (flags & 0xF0)
        d9 |= D3DCLEAR_TARGET;
    if (flags & 1)
        d9 |= D3DCLEAR_ZBUFFER;
    if (flags & 2)
        d9 |= D3DCLEAR_STENCIL;
    if (!d9)
        return;
    begin_scene();
    IDirect3DDevice9_Clear(device, rects ? (DWORD)count : 0, (const D3DRECT *)(rects ? rects : 0), d9, color, z,
                           stencil);
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ----------------------------------------------------------------------------------------------- textures -- */
/* An Xbox texture header (D3DPixelContainer): Common, Data, Lock, Format, Size. Format holds the colour format
 * (bits 8-15), mip levels (16-19) and, for swizzled textures, log2 width/height (20-23, 24-27); a linear texture has
 * width-1, height-1 and pitch/64-1 in Size. */
typedef struct { DWORD Common, Data, Lock, Format, Size; } XTexture;

typedef struct {
    DWORD data, format, size;
    IDirect3DTexture9 *texture;
    unsigned width, height;
    int linear;  /* linear textures are addressed in texels, swizzled ones 0..1 */
} CachedTexture;

static CachedTexture cache[1024];
static int cache_count;
static struct { IDirect3DTexture9 *texture; unsigned width, height; int linear; } bound[4];

/* swizzled (Morton) order: bits of x and y interleaved, x first, while both dimensions have bits left */
static unsigned swizzle(unsigned x, unsigned y, unsigned w, unsigned h)
{
    unsigned r = 0, bit = 0;
    for (unsigned b = 1; b < w || b < h; b <<= 1) {
        if (b < w)
            r |= (x & b ? 1u : 0u) << bit++;
        if (b < h)
            r |= (y & b ? 1u : 0u) << bit++;
    }
    return r;
}

static DWORD expand(DWORD v, int bits) { return bits == 0 ? 255 : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

/* one texel as ARGB; bytes per texel through *bpp; 0 for formats not decoded */
static int texel(unsigned format, const unsigned char *p, DWORD *out)
{
    WORD w = *(const WORD *)p;
    switch (format) {
    case 0x06: case 0x12: *out = *(const DWORD *)p; return 4;                        /* A8R8G8B8 */
    case 0x07: case 0x1e: *out = *(const DWORD *)p | 0xff000000; return 4;           /* X8R8G8B8 */
    case 0x05: case 0x11:                                                              /* R5G6B5 */
        *out = 0xff000000 | expand(w >> 11, 5) << 16 | expand((w >> 5) & 63, 6) << 8 | expand(w & 31, 5);
        return 2;
    case 0x02: case 0x10:                                                              /* A1R5G5B5 */
        *out = (w & 0x8000 ? 0xff000000 : 0) | expand((w >> 10) & 31, 5) << 16 | expand((w >> 5) & 31, 5) << 8 |
               expand(w & 31, 5);
        return 2;
    case 0x03: case 0x1c:                                                              /* X1R5G5B5 */
        *out = 0xff000000 | expand((w >> 10) & 31, 5) << 16 | expand((w >> 5) & 31, 5) << 8 | expand(w & 31, 5);
        return 2;
    case 0x04: case 0x1d:                                                              /* A4R4G4B4 */
        *out = (DWORD)((w >> 12) * 17) << 24 | (DWORD)(((w >> 8) & 15) * 17) << 16 | (DWORD)(((w >> 4) & 15) * 17) << 8 |
               (DWORD)((w & 15) * 17);
        return 2;
    case 0x00: case 0x13: *out = 0xff000000 | p[0] * 0x010101u; return 1;             /* L8 */
    case 0x19: case 0x1f: *out = (DWORD)p[0] << 24 | 0xffffff; return 1;              /* A8 */
    case 0x1a: case 0x20: *out = (DWORD)p[1] << 24 | p[0] * 0x010101u; return 2;      /* A8L8 */
    }
    return 0;
}

static CachedTexture *texture_for(const XTexture *x)
{
    for (int i = 0; i < cache_count; i++)
        if (cache[i].data == x->Data && cache[i].format == x->Format && cache[i].size == x->Size)
            return &cache[i];
    if (cache_count == (int)(sizeof cache / sizeof *cache) || !x->Data)
        return 0;
    CachedTexture *c = &cache[cache_count++];
    memset(c, 0, sizeof *c);
    c->data = x->Data, c->format = x->Format, c->size = x->Size;
    unsigned format = (x->Format >> 8) & 0xff;
    unsigned w, h, pitch = 0;
    if (x->Size) {
        w = (x->Size & 0xfff) + 1, h = ((x->Size >> 12) & 0xfff) + 1, pitch = (((x->Size >> 24) & 0xff) + 1) * 64;
        c->linear = 1;
    } else {
        w = 1u << ((x->Format >> 20) & 0xf), h = 1u << ((x->Format >> 24) & 0xf);
    }
    c->width = w, c->height = h;
    const unsigned char *src = (const unsigned char *)x->Data;
    if (format == 0x0c || format == 0x0e || format == 0x0f) {  /* DXT1/3/5: the same block layout as on Windows */
        D3DFORMAT f = format == 0x0c ? D3DFMT_DXT1 : format == 0x0e ? D3DFMT_DXT3 : D3DFMT_DXT5;
        unsigned block = format == 0x0c ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, 0, f, D3DPOOL_MANAGED, &c->texture, 0)))
            return c;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(c->texture, 0, &lr, 0, 0))) {
            for (unsigned by = 0; by < bh; by++)
                memcpy((unsigned char *)lr.pBits + by * lr.Pitch, src + by * bw * block, bw * block);
            IDirect3DTexture9_UnlockRect(c->texture, 0);
        }
        return c;
    }
    DWORD probe;
    int bpp = texel(format, src, &probe);
    if (!bpp) {
        static unsigned warned[256];
        if (!warned[format]++)
            printf("[gpu] texture format %#x not decoded yet (%ux%u)\n", format, w, h);
        return c;
    }
    if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c->texture, 0)))
        return c;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(c->texture, 0, &lr, 0, 0))) {
        for (unsigned y = 0; y < h; y++) {
            DWORD *row = (DWORD *)((unsigned char *)lr.pBits + y * lr.Pitch);
            for (unsigned xx = 0; xx < w; xx++) {
                const unsigned char *p = c->linear ? src + y * pitch + xx * bpp : src + swizzle(xx, y, w, h) * bpp;
                texel(format, p, &row[xx]);
            }
        }
        IDirect3DTexture9_UnlockRect(c->texture, 0);
    }
    return c;
}

void gpu_set_texture(int stage, const void *xbox_texture)
{
    if (!device || stage < 0 || stage > 3)
        return;
    CachedTexture *c = xbox_texture ? texture_for((const XTexture *)xbox_texture) : 0;
    bound[stage].texture = c ? c->texture : 0;
    bound[stage].width = c ? c->width : 1, bound[stage].height = c ? c->height : 1;
    bound[stage].linear = c ? c->linear : 0;
    IDirect3DDevice9_SetTexture(device, (DWORD)stage, (IDirect3DBaseTexture9 *)bound[stage].texture);
}

/* ------------------------------------------------------------------------------------------ render states -- */
/* NV2A (OpenGL-style) enums to Direct3D 9 */
static DWORD d3d_compare(DWORD gl) { return gl >= 0x200 && gl <= 0x207 ? gl - 0x200 + 1 : D3DCMP_ALWAYS; }

static DWORD d3d_blend(DWORD gl)
{
    if (gl == 0) return D3DBLEND_ZERO;
    if (gl == 1) return D3DBLEND_ONE;
    if (gl >= 0x300 && gl <= 0x308) return gl - 0x300 + D3DBLEND_SRCCOLOR;
    if (gl == 0x8001 || gl == 0x8003) return D3DBLEND_BLENDFACTOR;
    if (gl == 0x8002 || gl == 0x8004) return D3DBLEND_INVBLENDFACTOR;
    return D3DBLEND_ONE;
}

void gpu_render_state(unsigned method, unsigned value)
{
    if (!device)
        return;
    IDirect3DDevice9 *d = device;
    switch (method) {
    case 0x300: IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, value != 0); break;
    case 0x304: IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, value != 0); break;
    case 0x30c: IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, value ? D3DZB_TRUE : D3DZB_FALSE); break;
    case 0x33c: IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHAFUNC, d3d_compare(value)); break;
    case 0x340: IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHAREF, value & 0xff); break;
    case 0x344: IDirect3DDevice9_SetRenderState(d, D3DRS_SRCBLEND, d3d_blend(value)); break;
    case 0x348: IDirect3DDevice9_SetRenderState(d, D3DRS_DESTBLEND, d3d_blend(value)); break;
    case 0x34c: IDirect3DDevice9_SetRenderState(d, D3DRS_BLENDFACTOR, value); break;
    case 0x350: {
        DWORD op = value == 0x800a ? D3DBLENDOP_SUBTRACT : value == 0x800b ? D3DBLENDOP_REVSUBTRACT
                 : value == 0x8007 ? D3DBLENDOP_MIN : value == 0x8008 ? D3DBLENDOP_MAX : D3DBLENDOP_ADD;
        IDirect3DDevice9_SetRenderState(d, D3DRS_BLENDOP, op);
        break;
    }
    case 0x354: IDirect3DDevice9_SetRenderState(d, D3DRS_ZFUNC, d3d_compare(value)); break;
    case 0x358:
        IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE,
                                        (value & 0x00010000 ? D3DCOLORWRITEENABLE_RED : 0) |
                                        (value & 0x00000100 ? D3DCOLORWRITEENABLE_GREEN : 0) |
                                        (value & 0x00000001 ? D3DCOLORWRITEENABLE_BLUE : 0) |
                                        (value & 0x01000000 ? D3DCOLORWRITEENABLE_ALPHA : 0));
        break;
    case 0x35c: IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, value != 0); break;
    }
}

/* ------------------------------------------------------------------------------------------------- draws -- */
/* pretransformed, a diffuse and a specular colour, four 4-component texture coordinates (GpuVertex) */
#define VERTEX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX4 | D3DFVF_TEXCOORDSIZE4(0) | \
                    D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2) | D3DFVF_TEXCOORDSIZE4(3))

static GpuVertex scratch[0x20000];

/* the register combiners, approximated for now: texture 0 times the diffuse colour */
static void combiners(void)
{
    IDirect3DDevice9 *d = device;
    DWORD arg = bound[0].texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2;
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, arg);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, arg);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

void gpu_draw(int primitive, const GpuVertex *v, unsigned count)
{
    if (!device || !count)
        return;
    begin_scene();
    /* Xbox pixel centres are at .5; linear textures are addressed in texels */
    unsigned n = 0;
    D3DPRIMITIVETYPE type;
    unsigned prims;
    #define PUT(i) do { scratch[n] = v[i]; scratch[n].x -= 0.5f, scratch[n].y -= 0.5f; n++; } while (0)
    switch (primitive) {
    case 1: type = D3DPT_POINTLIST; for (unsigned i = 0; i < count; i++) PUT(i); prims = n; break;
    case 2: type = D3DPT_LINELIST; for (unsigned i = 0; i < count; i++) PUT(i); prims = n / 2; break;
    case 3: type = D3DPT_LINESTRIP; for (unsigned i = 0; i < count; i++) PUT(i); PUT(0); prims = n - 1; break;
    case 4: type = D3DPT_LINESTRIP; for (unsigned i = 0; i < count; i++) PUT(i); prims = n - 1; break;
    case 5: type = D3DPT_TRIANGLELIST; for (unsigned i = 0; i < count; i++) PUT(i); prims = n / 3; break;
    case 6: case 9: type = D3DPT_TRIANGLESTRIP; for (unsigned i = 0; i < count; i++) PUT(i); prims = n - 2; break;
    case 7: case 10: type = D3DPT_TRIANGLEFAN; for (unsigned i = 0; i < count; i++) PUT(i); prims = n - 2; break;
    case 8:  /* quads: two triangles each */
        type = D3DPT_TRIANGLELIST;
        for (unsigned q = 0; q + 3 < count && n + 6 <= sizeof scratch / sizeof *scratch; q += 4) {
            PUT(q); PUT(q + 1); PUT(q + 2); PUT(q); PUT(q + 2); PUT(q + 3);
        }
        prims = n / 3;
        break;
    default:
        return;
    }
    #undef PUT
    if ((int)prims <= 0)
        return;
    for (int s = 0; s < 4; s++)
        if (bound[s].texture && bound[s].linear)
            for (unsigned i = 0; i < n; i++)
                scratch[i].t[s][0] /= (float)bound[s].width, scratch[i].t[s][1] /= (float)bound[s].height;
    combiners();
    /* DRA2_FLAT=1: every draw in solid magenta with depth, blending, alpha test and textures off (is the geometry
     * reaching the screen at all?) */
    static int flat = -1;
    if (flat < 0) {
        char v[8];
        flat = GetEnvironmentVariableA("DRA2_FLAT", v, sizeof v) > 0;
    }
    if (flat) {
        IDirect3DDevice9 *d = device;
        IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, D3DZB_FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xf);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        for (unsigned i = 0; i < n; i++)
            scratch[i].diffuse = 0xffff00ff;
    }
    IDirect3DDevice9_SetFVF(device, VERTEX_FVF);
    HRESULT hr = IDirect3DDevice9_DrawPrimitiveUP(device, type, prims, scratch, sizeof(GpuVertex));
    static int logged;
    if (logged % 100 == 99)
        printf("[gpu] draw %d: first vertex %.1f %.1f %.3f rhw %.3f\n", logged + 1, scratch[0].x, scratch[0].y,
               scratch[0].z, scratch[0].rhw);
    if (logged++ < 3 || FAILED(hr)) {
        DWORD at, ab, zf, cw, ze, ar, af;
        IDirect3DDevice9_GetRenderState(device, D3DRS_ALPHATESTENABLE, &at);
        IDirect3DDevice9_GetRenderState(device, D3DRS_ALPHAREF, &ar);
        IDirect3DDevice9_GetRenderState(device, D3DRS_ALPHAFUNC, &af);
        IDirect3DDevice9_GetRenderState(device, D3DRS_ALPHABLENDENABLE, &ab);
        IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE, &ze);
        IDirect3DDevice9_GetRenderState(device, D3DRS_ZFUNC, &zf);
        IDirect3DDevice9_GetRenderState(device, D3DRS_COLORWRITEENABLE, &cw);
        printf("[gpu] draw type %d, %u primitives: %08lx; first vertex %.1f %.1f %.3f rhw %.3f %08lx t0 %g %g\n",
               (int)type, prims, (unsigned long)hr, scratch[0].x, scratch[0].y, scratch[0].z, scratch[0].rhw,
               (unsigned long)scratch[0].diffuse, scratch[0].t[0][0], scratch[0].t[0][1]);
        printf("[gpu]   texture0 %p %ux%u linear %d; alpha test %lu (func %lu ref %lu) blend %lu z %lu func %lu "
               "color write %lx\n", (void *)bound[0].texture, bound[0].width, bound[0].height, bound[0].linear,
               at, af, ar, ab, ze, zf, cw);
    }
}

/* DRA2_SNAPSHOT=<frame>,<path.bmp>: save that frame's back buffer, to look at the output without a screen capture */
static void snapshot(void)
{
    static int frame, target = -1;
    static char path[MAX_PATH];
    if (target == -1) {
        char v[MAX_PATH + 16];
        DWORD n = GetEnvironmentVariableA("DRA2_SNAPSHOT", v, sizeof v);
        target = 0;
        if (n && n < sizeof v) {
            char *comma = strchr(v, ',');
            if (comma) {
                *comma = 0;
                target = atoi(v);
                strncpy(path, comma + 1, sizeof path - 1);
            }
        }
    }
    if (++frame != target || !path[0])
        return;
    IDirect3DSurface9 *back = 0, *copy = 0;
    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return;
    IDirect3DSurface9_GetDesc(back, &desc);
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height, desc.Format,
                                                                D3DPOOL_SYSTEMMEM, &copy, 0)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(device, back, copy))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(copy, &lr, 0, D3DLOCK_READONLY))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = { 0 };
                BITMAPINFOHEADER ih = { 0 };
                DWORD size = desc.Width * desc.Height * 4;
                fh.bfType = 0x4d42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = fh.bfOffBits + size;
                ih.biSize = sizeof ih, ih.biWidth = (LONG)desc.Width, ih.biHeight = -(LONG)desc.Height;
                ih.biPlanes = 1, ih.biBitCount = 32, ih.biCompression = BI_RGB;
                fwrite(&fh, sizeof fh, 1, f);
                fwrite(&ih, sizeof ih, 1, f);
                for (UINT y = 0; y < desc.Height; y++)
                    fwrite((unsigned char *)lr.pBits + y * lr.Pitch, desc.Width * 4, 1, f);
                fclose(f);
                printf("[gpu] frame %d saved to %s\n", frame, path);
            }
            IDirect3DSurface9_UnlockRect(copy);
        }
    }
    if (copy)
        IDirect3DSurface9_Release(copy);
    IDirect3DSurface9_Release(back);
}

void gpu_present(void)
{
    if (device) {
        if (in_scene) {
            IDirect3DDevice9_EndScene(device);
            in_scene = 0;
        }
        snapshot();
        IDirect3DDevice9_Present(device, 0, 0, 0, 0);
    }
    pump_messages();
}
