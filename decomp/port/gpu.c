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
#include <d3dcommon.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a.h"

static HWND window;
static IDirect3D9 *d3d;
static IDirect3DDevice9 *device;
static int in_scene;

/* Display: DRA2_WINDOW=1..4 opens a window at that multiple of the game's resolution (default 2), and
 * DRA2_WINDOW=fullscreen a borderless window covering the monitor. Alt+Enter and F11 switch between the two. The
 * picture keeps the game's 4:3 shape, with black bars, whatever the window's size. */
static int fullscreen;
static WINDOWPLACEMENT windowed_placement = { sizeof(WINDOWPLACEMENT) };

/* Widescreen: DRA2_ASPECT=16:9 (or 16:10, 21:9...; 4:3 is the game's own) draws a wider picture, not a stretched one.
 * The native build widens the game's camera projections and their culling by gpu_wide (port/native.c), so the game
 * squeezes a wider view into its 640-pixel viewport. Here, 3D drawing is spread over the wider back buffer, and 2D
 * drawing (screen coordinates: HUD, menus, text) keeps its shape in the middle, except what spans the whole width
 * (fades, backgrounds), which is stretched to the edges. */
float gpu_wide = 1.0f;          /* the width over the game's: (aspect) / (4/3) */
int gpu_draw_2d;                /* set by port/d3d8.c around draws in screen coordinates */
static unsigned game_width = 640, buffer_width = 640, buffer_height = 480;
static IDirect3DTexture9 *drawing_into;  /* the target texture being drawn into, if not the back buffer */

static float aspect_setting(void)
{
    char v[16] = "";
    GetEnvironmentVariableA("DRA2_ASPECT", v, sizeof v);
    float w = 0, h = 0;
    char *colon = strchr(v, ':');
    if (colon) {
        *colon = 0;
        w = (float)atof(v), h = (float)atof(colon + 1);
    }
    float wide = w > 0 && h > 0 ? (w / h) / (4.0f / 3.0f) : 1.0f;
    return wide < 1.0f ? 1.0f : wide > 2.5f ? 2.5f : wide;
}

/* the x a game x maps to on the back buffer; full: the draw spans the game's whole width */
static float wide_x(float x, int two_d, int full)
{
    float middle = game_width * 0.5f, centre = buffer_width * 0.5f;
    if (!two_d || full)  /* spread: 3D was squeezed by the wider projection; full-width 2D is stretched */
        return centre + (x - middle) * ((float)buffer_width / (float)game_width);
    return x + (centre - middle);  /* moved to the middle, its shape kept */
}

static void set_fullscreen(int on)
{
    if (on == fullscreen)
        return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(window, &windowed_placement);
        GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &windowed_placement);
        SetWindowPos(window, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = on;
    InvalidateRect(window, 0, TRUE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY)
        ExitProcess(0);
    if ((msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) || (msg == WM_KEYDOWN && wp == VK_F11)) {
        if (!(lp & (1 << 30)))  /* not a key repeat */
            set_fullscreen(!fullscreen);
        return 0;
    }
    if (msg == WM_SYSCHAR && wp == VK_RETURN)
        return 0;  /* no beep for Alt+Enter */
    if (msg == WM_SIZE)
        InvalidateRect(hwnd, 0, TRUE);  /* repaint the bars around the picture */
    if (msg == WM_SETCURSOR && fullscreen && LOWORD(lp) == HTCLIENT) {
        SetCursor(0);
        return TRUE;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* the window and a device rendering at the game's resolution */
int gpu_open(unsigned width, unsigned height)
{
    char mode[16] = "";
    GetEnvironmentVariableA("DRA2_WINDOW", mode, sizeof mode);
    int scale = mode[0] >= '1' && mode[0] <= '4' && !mode[1] ? mode[0] - '0' : 2;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DRA2Window";
    RegisterClassA(&wc);
    gpu_wide = aspect_setting();
    game_width = width;
    buffer_width = ((unsigned)((float)width * gpu_wide + 1.0f)) & ~1u;  /* 640 * 4/3 = 853.3 -> 854 */
    buffer_height = height;
    if (gpu_wide > 1.0f)
        printf("[gpu] widescreen: %ux%u, the game's %u wide in the middle\n", buffer_width, height, width);
    width = buffer_width;
    RECT r = { 0, 0, (LONG)width * scale, (LONG)height * scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExA(0, wc.lpszClassName, "Digimon Rumble Arena 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    if (!_stricmp(mode, "fullscreen"))
        set_fullscreen(1);

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

static unsigned texture_frame;

/* DRA2_LOGFRAME=<frame>: every clear and draw of that frame in full */
static int log_frame_now(void)
{
    static int log_frame = -1;
    if (log_frame < 0) {
        char v[16] = "";
        log_frame = GetEnvironmentVariableA("DRA2_LOGFRAME", v, sizeof v) > 0 ? atoi(v) : 0;
    }
    return log_frame && texture_frame + 1 == (unsigned)log_frame;
}

static int has_depth = 1;  /* a depth buffer is bound (gpu_set_render_target) */

/* Clear with Xbox flags: D3DCLEAR_ZBUFFER 1, STENCIL 2, TARGET_R/G/B/A 0x10-0x80 */
void gpu_clear(int count, const long *rects, unsigned flags, unsigned color, float z, unsigned stencil)
{
    if (!device)
        return;
    DWORD d9 = 0;
    if (flags & 0xF0)
        d9 |= D3DCLEAR_TARGET;
    if ((flags & 1) && has_depth)
        d9 |= D3DCLEAR_ZBUFFER;
    if ((flags & 2) && has_depth)
        d9 |= D3DCLEAR_STENCIL;
    if (!d9)
        return;
    if (log_frame_now())
        printf("[gpu] clear flags %x colour %08x z %g (%d rects)\n", flags, color, z, rects ? count : 0);
    begin_scene();
    D3DRECT wide_rects[8];
    if (rects && !drawing_into && buffer_width != game_width && count > 0 && count <= 8) {
        for (int i = 0; i < count; i++) {  /* on the back buffer, as 2D drawing is */
            const long *q = rects + 4 * i;
            int full = q[0] <= 0 && q[2] >= (long)game_width;
            wide_rects[i].x1 = (LONG)wide_x((float)q[0], 1, full), wide_rects[i].y1 = q[1];
            wide_rects[i].x2 = (LONG)wide_x((float)q[2], 1, full), wide_rects[i].y2 = q[3];
        }
        rects = (const long *)wide_rects;
    }
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

/* Decoded textures, found again by the Xbox header's Data, Format and Size. The game builds textures every frame
 * (text, per-frame effects) in memory it frees and reuses, so an entry is re-decoded when a sample of its source
 * changes, and the least recently used one gives way when the cache is full: it used to fill up, and every texture
 * after that drew as none. */
typedef struct {
    DWORD data, format, size;
    IDirect3DTexture9 *texture;
    unsigned width, height;
    int linear;          /* linear textures are addressed in texels, swizzled ones 0..1 */
    unsigned bytes;      /* the source's size */
    DWORD sum;           /* a sample of the source, to notice it was rewritten */
    unsigned long long full;  /* a checksum of all of it, checked once a frame (full_sum) */
    unsigned checked;    /* the frame full was last checked in */
    unsigned last_used;  /* the frame it was last bound in */
} CachedTexture;

static CachedTexture cache[1024];
static int cache_count;
static struct { IDirect3DTexture9 *texture; unsigned width, height; int linear; const void *xbox; } bound[4];

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

/* 32 dwords spread over the source */
static DWORD source_sum(const CachedTexture *c)
{
    const DWORD *p = (const DWORD *)c->data;
    unsigned words = c->bytes / 4, step = words > 32 ? words / 32 : 1;
    DWORD s = 0;
    for (unsigned i = 0; i < words && i < 32 * step; i += step)
        s = s * 31 + p[i];
    return s;
}

/* All of the source (Fletcher-style, so moved data counts too). The sample above can miss a rewrite: the 1-player
 * "VS" screen loads its logo where a fighter's portrait was, both 256x256 DXT3 and mostly transparent, and the 32
 * sampled words all fell on empty blocks, so the logo drew as the portrait. */
static unsigned long long full_sum(const CachedTexture *c)
{
    const DWORD *p = (const DWORD *)c->data;
    unsigned long long a = 0, b = 0;
    for (unsigned i = 0, words = c->bytes / 4; i < words; i++) {
        a += p[i];
        b += a;
    }
    return a ^ (b << 32 | b >> 32);
}

static void decode(CachedTexture *c, const XTexture *x);
static void dxt_block(D3DFORMAT f, const unsigned char *b, DWORD out[16]);

static CachedTexture *texture_for(const XTexture *x)
{
    if (!x->Data)
        return 0;
    CachedTexture *c = 0;
    for (int i = 0; i < cache_count && !c; i++)
        if (cache[i].data == x->Data && cache[i].format == x->Format && cache[i].size == x->Size)
            c = &cache[i];
    if (c && c->bytes && source_sum(c) == c->sum) {
        int same = 1;
        if (c->checked != texture_frame) {  /* the whole source, once a frame: the sample is only a quick check */
            c->checked = texture_frame;
            same = full_sum(c) == c->full;
        }
        if (same) {
            c->last_used = texture_frame;
            return c;
        }
    }
    if (!c && cache_count < (int)(sizeof cache / sizeof *cache))
        c = &cache[cache_count++];
    if (!c) {  /* full: the least recently used entry goes */
        c = &cache[0];
        for (int i = 1; i < cache_count; i++)
            if (cache[i].last_used < c->last_used)
                c = &cache[i];
    }
    if (c->texture) {
        for (int s = 0; s < 4; s++)
            if (bound[s].texture == c->texture)
                bound[s].texture = 0, IDirect3DDevice9_SetTexture(device, (DWORD)s, 0);
        IDirect3DTexture9_Release(c->texture);
    }
    memset(c, 0, sizeof *c);
    c->data = x->Data, c->format = x->Format, c->size = x->Size;
    c->last_used = texture_frame;
    decode(c, x);
    if (c->bytes) {
        c->sum = source_sum(c);
        c->full = full_sum(c);
        c->checked = texture_frame;
    }
    return c;
}

/* How many bytes from p on are committed and readable. A texture whose header asks for more than its data block
 * holds (or whose block is already freed) is logged and skipped instead of read past the end. */
static unsigned readable_from(const void *p)
{
    MEMORY_BASIC_INFORMATION m;
    unsigned total = 0;
    for (const unsigned char *q = p; total < 0x4000000u; q = (const unsigned char *)m.BaseAddress + m.RegionSize) {
        if (!VirtualQuery(q, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            break;
        total += (unsigned)((const unsigned char *)m.BaseAddress + m.RegionSize - q);
    }
    return total;
}

static int texture_fits(const XTexture *x, unsigned bytes)
{
    unsigned have = readable_from(x->Data);
    if (have >= bytes)
        return 1;
    static int warned;
    if (warned++ < 8)
        printf("[gpu] texture %p (format %08lx, size %08lx) needs %u bytes at %p, only %u readable: skipped\n",
               (void *)x, (unsigned long)x->Format, (unsigned long)x->Size, bytes, x->Data, have);
    return 0;
}

int linear_format(unsigned format);  /* port/d3d8.c */

static void decode(CachedTexture *c, const XTexture *x)
{
    unsigned format = (x->Format >> 8) & 0xff;
    unsigned w, h, pitch = 0;
    if (x->Size) {
        /* the format says linear or swizzled, as on the NV2A: the game gives XGSetSurfaceHeader a swizzled format
         * (A8R8G8B8) with pitch 0, which leaves 0xff (16 KB) in Size's pitch field */
        w = (x->Size & 0xfff) + 1, h = ((x->Size >> 12) & 0xfff) + 1, pitch = (((x->Size >> 24) & 0xff) + 1) * 64;
        c->linear = linear_format(format);
    } else {
        w = 1u << ((x->Format >> 20) & 0xf), h = 1u << ((x->Format >> 24) & 0xf);
    }
    c->width = w, c->height = h;
    const unsigned char *src = (const unsigned char *)x->Data;
    if (format == 0x0c || format == 0x0e || format == 0x0f) {  /* DXT1/3/5: the same block layout as on Windows */
        D3DFORMAT f = format == 0x0c ? D3DFMT_DXT1 : format == 0x0e ? D3DFMT_DXT3 : D3DFMT_DXT5;
        unsigned block = format == 0x0c ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (!texture_fits(x, bw * bh * block))
            return;
        c->bytes = bw * bh * block;
        D3DLOCKED_RECT lr;
        if (w % 4 || h % 4) {
            /* Direct3D 9 wants DXT sizes in whole blocks; the fighters' health and evolution gauges are 2x16, so
             * those are decompressed here */
            if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c->texture,
                                                      0)))
                return;
            if (SUCCEEDED(IDirect3DTexture9_LockRect(c->texture, 0, &lr, 0, 0))) {
                for (unsigned by = 0; by < bh; by++)
                    for (unsigned bx = 0; bx < bw; bx++) {
                        DWORD texels[16];
                        dxt_block(f, src + (by * bw + bx) * block, texels);
                        for (unsigned y = 0; y < 4 && by * 4 + y < h; y++)
                            for (unsigned xx = 0; xx < 4 && bx * 4 + xx < w; xx++)
                                ((DWORD *)((unsigned char *)lr.pBits + (by * 4 + y) * lr.Pitch))[bx * 4 + xx] =
                                    texels[y * 4 + xx];
                    }
                IDirect3DTexture9_UnlockRect(c->texture, 0);
            }
            return;
        }
        if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, 0, f, D3DPOOL_MANAGED, &c->texture, 0)))
            return;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(c->texture, 0, &lr, 0, 0))) {
            for (unsigned by = 0; by < bh; by++)
                memcpy((unsigned char *)lr.pBits + by * lr.Pitch, src + by * bw * block, bw * block);
            IDirect3DTexture9_UnlockRect(c->texture, 0);
        }
        return;
    }
    DWORD probe;
    int bpp = texel(format, src, &probe);
    if (!bpp) {
        static unsigned warned[256];
        if (!warned[format]++)
            printf("[gpu] texture format %#x not decoded yet (%ux%u)\n", format, w, h);
        return;
    }
    if (!texture_fits(x, c->linear ? pitch * h : w * h * (unsigned)bpp))
        return;
    if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c->texture, 0)))
        return;
    c->bytes = c->linear ? pitch * h : w * h * (unsigned)bpp;
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
}

/* the Xbox texture last bound to a stage (D3DDevice_GetTexture2) */
const void *gpu_get_texture(int stage) { return stage >= 0 && stage < 4 ? bound[stage].xbox : 0; }

/* one 4x4 DXT1/3/5 block to ARGB, for dump_texture and for DXT textures not in whole blocks (decode); the others go to
 * Direct3D as they are */
static void dxt_block(D3DFORMAT f, const unsigned char *b, DWORD out[16])
{
    const unsigned char *c = f == D3DFMT_DXT1 ? b : b + 8;
    WORD c0 = (WORD)(c[0] | c[1] << 8), c1 = (WORD)(c[2] | c[3] << 8);
    DWORD pal[4];
    for (int k = 0; k < 2; k++) {
        WORD w = k ? c1 : c0;
        pal[k] = 0xff000000 | expand(w >> 11, 5) << 16 | expand((w >> 5) & 63, 6) << 8 | expand(w & 31, 5);
    }
    for (int ch = 0; ch < 3; ch++) {
        DWORD a = (pal[0] >> (8 * ch)) & 0xff, z = (pal[1] >> (8 * ch)) & 0xff;
        DWORD m2 = c0 > c1 || f != D3DFMT_DXT1 ? (2 * a + z) / 3 : (a + z) / 2;
        DWORD m3 = c0 > c1 || f != D3DFMT_DXT1 ? (a + 2 * z) / 3 : 0;
        pal[2] = (pal[2] & ~(0xffu << (8 * ch))) | m2 << (8 * ch);
        pal[3] = (pal[3] & ~(0xffu << (8 * ch))) | m3 << (8 * ch);
    }
    pal[2] |= 0xff000000, pal[3] = f == D3DFMT_DXT1 && c0 <= c1 ? pal[3] & 0xffffff : pal[3] | 0xff000000;
    DWORD bits = (DWORD)c[4] | c[5] << 8 | c[6] << 16 | (DWORD)c[7] << 24;
    for (int i = 0; i < 16; i++) {
        DWORD p = pal[(bits >> (2 * i)) & 3], a = p >> 24;
        if (f == D3DFMT_DXT3)
            a = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
        else if (f == D3DFMT_DXT5) {
            DWORD a0 = b[0], a1 = b[1], code = 0;
            unsigned long long ab = 0;
            for (int k = 0; k < 6; k++)
                ab |= (unsigned long long)b[2 + k] << (8 * k);
            code = (DWORD)(ab >> (3 * i)) & 7;
            a = code == 0 ? a0 : code == 1 ? a1 : a0 > a1 ? ((8 - code) * a0 + (code - 1) * a1) / 7
                : code == 6 ? 0 : code == 7 ? 255 : ((6 - code) * a0 + (code - 1) * a1) / 5;
        }
        out[i] = (p & 0xffffff) | a << 24;
    }
}

/* with DRA2_LOGFRAME: each texture the frame draws with, as %TEMP%\dra2_tex_<address>.bmp (ARGB, alpha kept) */
static void dump_texture(IDirect3DTexture9 *t, const void *xbox)
{
    static IDirect3DTexture9 *done[64];
    static int ndone;
    for (int i = 0; i < ndone; i++)
        if (done[i] == t)
            return;
    if (ndone < 64)
        done[ndone++] = t;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    int dxt = 0;
    if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(t, 0, &desc)))
        dxt = desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5;
    if ((desc.Format != D3DFMT_A8R8G8B8 && !dxt) || FAILED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, D3DLOCK_READONLY))) {
        printf("[gpu]   texture %p: Xbox format %#lx (not dumped)\n", (void *)t,
               xbox ? (unsigned long)((const XTexture *)xbox)->Format : 0ul);
        return;
    }
    char path[MAX_PATH], dir[MAX_PATH];
    GetEnvironmentVariableA("TEMP", dir, sizeof dir);
    snprintf(path, sizeof path, "%s\\dra2_tex_%p.bmp", dir, (void *)t);
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
        for (UINT y = 0; y < desc.Height; y++) {
            if (!dxt) {
                fwrite((unsigned char *)lr.pBits + y * lr.Pitch, desc.Width * 4, 1, f);
                continue;
            }
            for (UINT x = 0; x < desc.Width; x += 4) {
                DWORD block[16];
                unsigned size = desc.Format == D3DFMT_DXT1 ? 8 : 16;
                dxt_block(desc.Format, (unsigned char *)lr.pBits + (y / 4) * lr.Pitch + (x / 4) * size, block);
                fwrite(block + (y & 3) * 4, 4, desc.Width - x < 4 ? desc.Width - x : 4, f);
            }
        }
        fclose(f);
        printf("[gpu]   texture %p: Xbox format %#lx, saved to %s\n", (void *)t,
               xbox ? (unsigned long)((const XTexture *)xbox)->Format : 0ul, path);
    }
    IDirect3DTexture9_UnlockRect(t, 0);
}

/* Render targets other than the back buffer: surfaces the game draws into and then binds as textures (each
 * fighter's 64x64 shadow). Each gets a Direct3D 9 render-target texture, found again by the Xbox surface's data
 * address when a texture with that address is bound (gpu_set_texture). */
static struct { DWORD data; unsigned width, height; IDirect3DTexture9 *texture; } targets[16];
static int target_count;
static IDirect3DSurface9 *back_buffer, *back_depth;
void gpu_set_render_target(DWORD data, unsigned width, unsigned height, int back, int depth)
{
    if (!device)
        return;
    if (!back_buffer) {
        IDirect3DDevice9_GetRenderTarget(device, 0, &back_buffer);
        IDirect3DDevice9_GetDepthStencilSurface(device, &back_depth);
    }
    IDirect3DSurface9 *surface = back_buffer;
    if (!back) {
        int i = 0;
        while (i < target_count && !(targets[i].data == data && targets[i].width == width && targets[i].height == height))
            i++;
        if (i == target_count) {
            if (target_count == (int)(sizeof targets / sizeof *targets) ||
                FAILED(IDirect3DDevice9_CreateTexture(device, width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_DEFAULT, &targets[i].texture, 0)))
                return;
            targets[i].data = data, targets[i].width = width, targets[i].height = height;
            target_count++;
        }
        IDirect3DTexture9_GetSurfaceLevel(targets[i].texture, 0, &surface);
        drawing_into = targets[i].texture;
    } else
        drawing_into = 0;
    /* a texture can't be read while it is being drawn into: its stages are emptied meanwhile, and get it back after
     * (the game binds it once, and its own state still has it bound) */
    for (int s = 0; s < 4; s++)
        IDirect3DDevice9_SetTexture(device, (DWORD)s, (IDirect3DBaseTexture9 *)(bound[s].texture == drawing_into &&
                                                                               drawing_into ? 0 : bound[s].texture));
    IDirect3DDevice9_SetRenderTarget(device, 0, surface);  /* also sets the viewport to the whole target */
    if (surface != back_buffer)
        IDirect3DSurface9_Release(surface);
    /* the game keeps its 640x480 depth buffer for the small targets too, as Direct3D 9 allows */
    IDirect3DDevice9_SetDepthStencilSurface(device, depth ? back_depth : 0);
    has_depth = depth;
    if (log_frame_now())
        printf("[gpu] render target %08lx %ux%u%s\n", (unsigned long)data, width, height, back ? " (back buffer)" : "");
}

static IDirect3DTexture9 *target_texture(const XTexture *x, unsigned *width, unsigned *height)
{
    for (int i = 0; i < target_count; i++)
        if (targets[i].data == x->Data) {
            *width = targets[i].width, *height = targets[i].height;
            return targets[i].texture;
        }
    return 0;
}

void gpu_set_texture(int stage, const void *xbox_texture)
{
    if (!device || stage < 0 || stage > 3)
        return;
    unsigned width, height;
    IDirect3DTexture9 *target = xbox_texture ? target_texture((const XTexture *)xbox_texture, &width, &height) : 0;
    CachedTexture *c = xbox_texture && !target ? texture_for((const XTexture *)xbox_texture) : 0;
    if (target) {
        bound[stage].texture = target, bound[stage].width = width, bound[stage].height = height;
        bound[stage].linear = 0;
    } else {
        bound[stage].texture = c ? c->texture : 0;
        bound[stage].width = c ? c->width : 1, bound[stage].height = c ? c->height : 1;
        bound[stage].linear = c ? c->linear : 0;
    }
    bound[stage].xbox = xbox_texture;
    IDirect3DDevice9_SetTexture(device, (DWORD)stage, (IDirect3DBaseTexture9 *)(drawing_into && bound[stage].texture ==
                                                                       drawing_into ? 0 : bound[stage].texture));
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
/* pretransformed, a diffuse and a specular colour, four 4-component texture coordinates and the fog factor as a
 * fifth (GpuVertex) */
#define VERTEX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX5 | D3DFVF_TEXCOORDSIZE4(0) | \
                    D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2) | D3DFVF_TEXCOORDSIZE4(3) | \
                    D3DFVF_TEXCOORDSIZE1(4))

static GpuVertex scratch[0x20000];

/* -------------------------------------------------------------------------------- register combiners -- */
/* The Xbox's pixel stage is the NV2A's register combiners, set as a D3DPIXELSHADERDEF (SetPixelShaderProgram). Each
 * program is translated once into Direct3D 9 ps_2_0 bytecode, emitted directly (no shader compiler is available):
 *  - NV registers live in temporaries: R0 r0, R1 r1, T0-T3 r2-r5 (loaded with texld), V0/V1 r6/r7 (the diffuse and
 *    specular colours); r8-r11 are scratch. c0-c15 hold each stage's C0/C1, c16/c17 the final combiner's, c18 the fog
 *    colour (FOG.rgb; FOG.a is the vertex's fog factor, texture coordinate 4), c20 = (0, 1, 0.5, 2).
 *  - A stage computes, for its RGB and alpha portions separately, A*B (or dot3), C*D (or dot3) and their sum (or a mux
 *    on r0.a), each after the inputs' mappings (invert, expand, half bias, negate) and before the output's scale/bias;
 *    all reads happen before the stage's writes, as on the hardware.
 *  - The final combiner: rgb = A*B + (1-A)*C + D, alpha = G, with E*F and V1+R0 as extra inputs. */
typedef struct {
    DWORD alpha_inputs[8], final_abcd, final_efg, constant0[8], constant1[8], alpha_outputs[8], rgb_inputs[8];
    DWORD compare_mode, final_constant0, final_constant1, rgb_outputs[8], combiner_count, texture_modes, dot_mapping;
    DWORD input_texture, c0_mapping, c1_mapping, final_constants;
} PixelShaderDef;

extern void *nv_pixel_shader;  /* port/d3d8.c */

/* HLSL for one combiner program, compiled with D3DCompile (d3dcompiler_47.dll, part of Windows 10 and later) */
static char hlsl[16384];
static int hlsl_len;

static void put(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    hlsl_len += vsnprintf(hlsl + hlsl_len, sizeof hlsl - (size_t)hlsl_len, format, ap);
    va_end(ap);
}

/* an NV combiner register as an HLSL float4 expression (final: the final combiner's constants) */
static const char *reg_name(unsigned r, int stage, int final)
{
    static char buf[4][24];
    static int k;
    char *s = buf[k++ & 3];
    switch (r) {
    case 1: snprintf(s, 24, "c[%d]", final ? 16 : 2 * stage); return s;
    case 2: snprintf(s, 24, "c[%d]", final ? 17 : 2 * stage + 1); return s;
    case 3: return "FOG";
    case 4: return "V0";
    case 5: return "V1";
    case 8: return "T0";
    case 9: return "T1";
    case 10: return "T2";
    case 11: return "T3";
    case 12: return "R0";
    case 13: return "R1";
    case 14: return "V1R0";
    case 15: return "EF";
    }
    return "float4(0,0,0,0)";
}

/* one input: its register's rgb (or alpha replicated) for an RGB portion, alpha (or blue) for an alpha portion,
 * through its mapping */
static void put_input(unsigned input, int stage, int alpha_portion, int final)
{
    static const char *maps[8] = { "saturate(%s)", "(1 - saturate(%s))", "(2 * saturate(%s) - 1)",
                                   "(1 - 2 * saturate(%s))", "(saturate(%s) - 0.5)", "(0.5 - saturate(%s))",
                                   "(%s)", "(-%s)" };
    char x[64];
    const char *r = reg_name(input & 0xf, stage, final);
    if (alpha_portion)
        snprintf(x, sizeof x, "%s.%s", r, input & 0x10 ? "a" : "b");
    else
        snprintf(x, sizeof x, "%s.%s", r, input & 0x10 ? "aaa" : "rgb");
    put(maps[(input >> 5) & 7], x);
}

static const char *dest_name(unsigned r)
{
    switch (r) {
    case 4: return "V0";
    case 5: return "V1";
    case 8: return "T0";
    case 9: return "T1";
    case 10: return "T2";
    case 11: return "T3";
    case 12: return "R0";
    case 13: return "R1";
    }
    return 0;
}

static void put_portion(DWORD inputs, DWORD outputs, int stage, int alpha_portion)
{
    const char *t = alpha_portion ? "float" : "float3", *p = alpha_portion ? "a" : "c";
    static const char *mapping[8] = { "%s", "(%s - 0.5)", "(2 * %s)", "(2 * (%s - 0.5))", "(4 * %s)", "%s",
                                      "(%s / 2)", "%s" };
    put("  %s %sA = ", t, p); put_input(inputs >> 24, stage, alpha_portion, 0); put(";\n");
    put("  %s %sB = ", t, p); put_input((inputs >> 16) & 0xff, stage, alpha_portion, 0); put(";\n");
    put("  %s %sC = ", t, p); put_input((inputs >> 8) & 0xff, stage, alpha_portion, 0); put(";\n");
    put("  %s %sD = ", t, p); put_input(inputs & 0xff, stage, alpha_portion, 0); put(";\n");
    if (!alpha_portion && (outputs & 0x2000))
        put("  %s %sAB = dot(%sA, %sB);\n", t, p, p, p);
    else
        put("  %s %sAB = %sA * %sB;\n", t, p, p, p);
    if (!alpha_portion && (outputs & 0x1000))
        put("  %s %sCD = dot(%sC, %sD);\n", t, p, p, p);
    else
        put("  %s %sCD = %sC * %sD;\n", t, p, p, p);
    if (outputs & 0x4000)
        put("  %s %sSUM = R0.a >= 0.5 ? %sCD : %sAB;\n", t, p, p, p);
    else
        put("  %s %sSUM = %sAB + %sCD;\n", t, p, p, p);
    const char *m = mapping[(outputs >> 15) & 7];
    static const char *parts[3] = { "AB", "CD", "SUM" };
    for (int k = 0; k < 3; k++) {
        char v[16];
        snprintf(v, sizeof v, "%s%s", p, parts[k]);
        put("  %s = clamp(", v);
        put(m, v);
        put(", -1, 1);\n");
    }
}

static void put_writes(DWORD outputs, int alpha_portion)
{
    const char *p = alpha_portion ? "a" : "c", *mask = alpha_portion ? "a" : "rgb";
    unsigned dests[3] = { (outputs >> 4) & 0xf, outputs & 0xf, (outputs >> 8) & 0xf };
    static const char *parts[3] = { "AB", "CD", "SUM" };
    for (int k = 0; k < 3; k++) {
        const char *d = dest_name(dests[k]);
        if (d)
            put("  %s.%s = %s%s;\n", d, mask, p, parts[k]);
    }
}

/* final-combiner input: rgb (or alpha replicated), unsigned, maybe inverted */
static void put_final_input(unsigned input)
{
    put("%s(saturate(%s.%s))", input & 0x20 ? "1 - " : "", reg_name(input & 0xf, 0, 1), input & 0x10 ? "aaa" : "rgb");
}

typedef HRESULT(WINAPI *D3DCompileFn)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                       const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static IDirect3DPixelShader9 *translate(const PixelShaderDef *ps)
{
    static D3DCompileFn compile;
    if (!compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        compile = m ? (D3DCompileFn)(void *)GetProcAddress(m, "D3DCompile") : 0;
        if (!compile) {
            printf("[gpu] d3dcompiler_47.dll not found: no register combiners\n");
            return 0;
        }
    }
    hlsl_len = 0;
    put("float4 c[19] : register(c0);\n");
    for (int t = 0; t < 4; t++)
        put("sampler2D s%d : register(s%d);\n", t, t);
    put("float4 main(float4 v0 : COLOR0, float4 v1 : COLOR1, float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1,\n"
        "            float4 t2 : TEXCOORD2, float4 t3 : TEXCOORD3, float t4 : TEXCOORD4) : COLOR\n{\n");
    put("  float4 V0 = v0, V1 = v1, R1 = 0, FOG = float4(c[18].rgb, saturate(t4));\n");
    for (int t = 0; t < 4; t++) {
        /* PROJECT2D (1), the ordinary mode, divides by q as the NV2A (and xemu) does; the game's coordinates mostly
         * have q = 1, which the attribute defaults and the fixed-function path also give */
        unsigned mode = (ps->texture_modes >> (5 * t)) & 0x1f;
        if (mode == 1)
            put("  float4 T%d = tex2Dproj(s%d, t%d);\n", t, t, t);
        else if (mode)
            put("  float4 T%d = tex2D(s%d, t%d.xy);\n", t, t, t);
        else
            put("  float4 T%d = 0;\n", t);
    }
    put("  float4 R0 = float4(0, 0, 0, T0.a);\n");
    unsigned stages = ps->combiner_count & 0xff;
    for (unsigned s = 0; s < stages && s < 8; s++) {
        put("  {\n");
        put_portion(ps->rgb_inputs[s], ps->rgb_outputs[s], (int)s, 0);
        put_portion(ps->alpha_inputs[s], ps->alpha_outputs[s], (int)s, 1);
        put_writes(ps->rgb_outputs[s], 0);
        put_writes(ps->alpha_outputs[s], 1);
        put("  }\n");
    }
    DWORD abcd = ps->final_abcd, efg = ps->final_efg;
    unsigned g = (efg >> 8) & 0xff;
    put("  float4 V1R0 = float4(saturate(V1.rgb + R0.rgb), 0);\n");
    put("  float4 EF = float4("); put_final_input(efg >> 24); put(" * "); put_final_input((efg >> 16) & 0xff); put(", 0);\n");
    put("  float3 fA = "); put_final_input(abcd >> 24); put(";\n");
    put("  float3 fB = "); put_final_input((abcd >> 16) & 0xff); put(";\n");
    put("  float3 fC = "); put_final_input((abcd >> 8) & 0xff); put(";\n");
    put("  float3 fD = "); put_final_input(abcd & 0xff); put(";\n");
    put("  float alpha = %s(saturate(%s.%s));\n", g & 0x20 ? "1 - " : "", reg_name(g & 0xf, 0, 1), g & 0x10 ? "a" : "b");
    put("  return float4(saturate(fA * fB + (1 - fA) * fC + fD), alpha);\n}\n");

    ID3DBlob *blob = 0, *errors = 0;
    HRESULT hr = compile(hlsl, (SIZE_T)hlsl_len, "combiners", 0, 0, "main", "ps_2_b", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        printf("[gpu] combiner program didn't compile: %s\n%s",
               errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?", hlsl);
        if (errors)
            ID3D10Blob_Release(errors);
        return 0;
    }
    IDirect3DPixelShader9 *shader = 0;
    hr = IDirect3DDevice9_CreatePixelShader(device, (const DWORD *)ID3D10Blob_GetBufferPointer(blob), &shader);
    ID3D10Blob_Release(blob);
    if (errors)
        ID3D10Blob_Release(errors);
    static int logged;
    if (FAILED(hr) || logged++ < 4)
        printf("[gpu] combiner program, %u stages -> ps_2_b: %08lx\n", stages, (unsigned long)hr);
    return shader;
}
/* translated programs, by the definition's contents without its constant colours: the HLSL reads those from c[],
 * and a program whose colours fade (or flash) would otherwise be compiled again every frame (half the time of a
 * fight went to D3DCompile) */
static struct { PixelShaderDef def; IDirect3DPixelShader9 *shader; } shaders[1024];
static int shader_count;

static PixelShaderDef shader_key(const PixelShaderDef *ps)
{
    PixelShaderDef k = *ps;
    memset(k.constant0, 0, sizeof k.constant0);
    memset(k.constant1, 0, sizeof k.constant1);
    k.final_constant0 = k.final_constant1 = 0;
    return k;
}

static void set_constant_color(int reg, DWORD argb)
{
    float c[4] = { ((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f,
                   (argb >> 24) / 255.0f };
    IDirect3DDevice9_SetPixelShaderConstantF(device, (UINT)reg, c, 1);
}

static int pixel_shader(void)
{
    const PixelShaderDef *ps = (const PixelShaderDef *)nv_pixel_shader;
    if (!ps)
        return 0;
    IDirect3DPixelShader9 *shader = 0;
    PixelShaderDef key = shader_key(ps);
    static int last = -1;  /* consecutive draws mostly share one */
    if (last >= 0 && !memcmp(&shaders[last].def, &key, sizeof key))
        shader = shaders[last].shader;
    for (int i = 0; i < shader_count && !shader; i++)
        if (!memcmp(&shaders[i].def, &key, sizeof key)) {
            shader = shaders[i].shader;
            last = i;
        }
    if (!shader) {
        shader = translate(ps);
        if (shader_count < (int)(sizeof shaders / sizeof *shaders)) {
            shaders[shader_count].def = key;
            last = shader_count;
            shaders[shader_count++].shader = shader;
        }
    }
    if (!shader)
        return 0;
    /* constants: each stage's own C0/C1 when the program says so (PS_COMBINERCOUNT_UNIQUE_C0/C1), else stage 0's */
    int unique_c0 = (ps->combiner_count >> 8) & 0x10, unique_c1 = (ps->combiner_count >> 8) & 0x100 ? 1 : 0;
    for (int s = 0; s < 8; s++) {
        set_constant_color(2 * s, ps->constant0[unique_c0 ? s : 0]);
        set_constant_color(2 * s + 1, ps->constant1[unique_c1 ? s : 0]);
    }
    set_constant_color(16, ps->final_constant0);
    set_constant_color(17, ps->final_constant1);
    float fog_start, fog_end;
    unsigned fog_color = 0;
    nv_fog(&fog_start, &fog_end, &fog_color);
    set_constant_color(18, fog_color);  /* the fog colour; the factor comes with each vertex */
    IDirect3DDevice9_SetPixelShader(device, shader);
    return 1;
}

/* with no combiner program: texture 0 times the diffuse colour */
static void combiners(void)
{
    IDirect3DDevice9 *d = device;
    if (pixel_shader())
        return;
    IDirect3DDevice9_SetPixelShader(d, 0);
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

/* Clipping. The NV2A clips triangles in homogeneous space; pretransformed vertices reach Direct3D 9 already divided
 * by w, and it draws them as they are. Two kinds of triangle need clipping here:
 *  - one with a corner behind the camera (w < 0) would be drawn mirrored through infinity, a smear across the screen
 *    (the old west arena's ground, which passes under the camera): clipped at the near plane;
 *  - one with a corner just in front of the camera lands millions of pixels off screen, beyond what the GPU's
 *    rasterizer handles, and is drawn wrongly: the cave arena's outer shell, which surrounds the camera, came out as
 *    a flat grey slab across the nearer walls. Clipped to a guard band around the screen.
 * The vertex program's outputs are x, y, z already divided by w (plus the viewport offset) and w, so x*w, y*w, z*w,
 * w are linear in clip space: each plane's distance is too, and the corners made on an edge get every output
 * interpolated along it. */
static GpuVertex clipped[0x20000];

#define GUARD_BAND 8192.0f  /* pixels either side of the origin, well inside what Direct3D 9 GPUs rasterize */

/* distance to plane p (0 near, 1-4 the guard band's sides), >= 0 inside; meaningful once w > 0 (after plane 0) */
static float clip_distance(const GpuVertex *g, int p)
{
    float w = 1.0f / g->rhw;
    switch (p) {
    case 0: return g->z * w;
    case 1: return (g->x + GUARD_BAND) * w;
    case 2: return (GUARD_BAND - g->x) * w;
    case 3: return (g->y + GUARD_BAND) * w;
    default: return (GUARD_BAND - g->y) * w;
    }
}

static int needs_clipping(const GpuVertex *g)
{
    return !(clip_distance(g, 0) >= 0.0f) || g->x < -GUARD_BAND || g->x > GUARD_BAND || g->y < -GUARD_BAND ||
           g->y > GUARD_BAND;
}

static unsigned lerp_color(unsigned a, unsigned b, float t)
{
    unsigned r = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float ca = (float)((a >> shift) & 0xff), cb = (float)((b >> shift) & 0xff);
        r |= (unsigned)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return r;
}

static GpuVertex clip_point(const GpuVertex *a, const GpuVertex *b, int p)
{
    float da = clip_distance(a, p), db = clip_distance(b, p), t = da / (da - db);
    float wa = 1.0f / a->rhw, wb = 1.0f / b->rhw, w = wa + (wb - wa) * t;
    GpuVertex g;
    g.x = (a->x * wa + (b->x * wb - a->x * wa) * t) / w;
    g.y = (a->y * wa + (b->y * wb - a->y * wa) * t) / w;
    g.z = (a->z * wa + (b->z * wb - a->z * wa) * t) / w;
    g.rhw = 1.0f / w;
    g.diffuse = lerp_color(a->diffuse, b->diffuse, t);
    g.specular = lerp_color(a->specular, b->specular, t);
    for (int s = 0; s < 4; s++)
        for (int i = 0; i < 4; i++)
            g.t[s][i] = a->t[s][i] + (b->t[s][i] - a->t[s][i]) * t;
    g.fog = a->fog + (b->fog - a->fog) * t;
    return g;
}

/* the n vertices in scratch, as triangles of the given type, clipped into a triangle list (back into scratch);
 * returns the number of triangles. The corners keep their order, so the winding (and culling) stays the same. */
static unsigned clip_triangles(D3DPRIMITIVETYPE type, unsigned *n)
{
    unsigned in_tris = type == D3DPT_TRIANGLELIST ? *n / 3 : *n - 2, out = 0;
    for (unsigned k = 0; k < in_tris && out + 18 <= sizeof clipped / sizeof *clipped; k++) {
        GpuVertex poly[2][8];  /* a triangle clipped by 5 planes has at most 8 corners */
        int m = 3, cur = 0;
        if (type == D3DPT_TRIANGLELIST)
            poly[0][0] = scratch[3 * k], poly[0][1] = scratch[3 * k + 1], poly[0][2] = scratch[3 * k + 2];
        else if (type == D3DPT_TRIANGLEFAN)
            poly[0][0] = scratch[0], poly[0][1] = scratch[k + 1], poly[0][2] = scratch[k + 2];
        else if (k & 1)  /* strips alternate their winding */
            poly[0][0] = scratch[k + 1], poly[0][1] = scratch[k], poly[0][2] = scratch[k + 2];
        else
            poly[0][0] = scratch[k], poly[0][1] = scratch[k + 1], poly[0][2] = scratch[k + 2];
        for (int p = 0; p < 5 && m >= 3; p++) {
            const GpuVertex *in = poly[cur];
            GpuVertex *next = poly[cur ^ 1];
            int all_in = 1, made = 0;
            for (int i = 0; i < m && all_in; i++)
                all_in = clip_distance(&in[i], p) >= 0.0f;
            if (all_in)
                continue;
            for (int e = 0; e < m; e++) {
                const GpuVertex *a = &in[e], *b = &in[(e + 1) % m];
                int a_in = clip_distance(a, p) >= 0.0f, b_in = clip_distance(b, p) >= 0.0f;
                if (a_in)
                    next[made++] = *a;
                if (a_in != b_in)
                    next[made++] = clip_point(a, b, p);
            }
            m = made, cur ^= 1;
        }
        for (int i = 1; i + 1 < m; i++)
            clipped[out++] = poly[cur][0], clipped[out++] = poly[cur][i], clipped[out++] = poly[cur][i + 1];
    }
    memcpy(scratch, clipped, out * sizeof *clipped);
    *n = out;
    return out / 3;
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
    /* widescreen, on the back buffer: 3D spread over the width, 2D in the middle unless it spans the whole width */
    int wide = !drawing_into && buffer_width != game_width, full = 0;
    if (wide && gpu_draw_2d) {
        float lo = v[0].x, hi = v[0].x;
        for (unsigned i = 1; i < count; i++)
            lo = v[i].x < lo ? v[i].x : lo, hi = v[i].x > hi ? v[i].x : hi;
        full = lo <= 1.0f && hi >= (float)game_width - 1.0f;
    }
    #define PUT(i) do { scratch[n] = v[i]; if (wide) scratch[n].x = wide_x(scratch[n].x, gpu_draw_2d, full); \
                        scratch[n].x -= 0.5f, scratch[n].y -= 0.5f; n++; } while (0)
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
    if (type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN) {
        unsigned outside = 0;
        for (unsigned i = 0; i < n && !outside; i++)
            outside = needs_clipping(&scratch[i]);
        if (outside) {
            prims = clip_triangles(type, &n);
            type = D3DPT_TRIANGLELIST;
            if (!prims)
                return;
        }
    }
    for (int s = 0; s < 4; s++)
        if (bound[s].texture && bound[s].linear)
            for (unsigned i = 0; i < n; i++)
                scratch[i].t[s][0] /= (float)bound[s].width, scratch[i].t[s][1] /= (float)bound[s].height;
    combiners();
    /* DRA2_FLAT=1: every draw in magenta with depth, blending and alpha test off (is the geometry reaching the
     * screen at all?); 2: only the states off; 3: only the vertex colours white; 4: only the depth test off. The register combiners still run,
     * so textures still show through. */
    static int flat = -1;
    if (flat < 0) {
        char v[8] = "";
        flat = GetEnvironmentVariableA("DRA2_FLAT", v, sizeof v) > 0 ? atoi(v) : 0;
    }
    if (flat == 3)
        for (unsigned i = 0; i < n; i++)
            scratch[i].diffuse = 0xffffffff;
    if (flat == 4)
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    if (flat == 2) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_COLORWRITEENABLE, 0xf);
    }
    if (flat == 1) {
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
    if (logged % 10000 == 9999)
        printf("[gpu] draw %d: first vertex %.1f %.1f %.3f rhw %.3f\n", logged + 1, scratch[0].x, scratch[0].y,
               scratch[0].z, scratch[0].rhw);
    if (logged++ < 3 || FAILED(hr) || log_frame_now()) {
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
        if (log_frame_now()) {
            DWORD sb, db, op;
            IDirect3DDevice9_GetRenderState(device, D3DRS_SRCBLEND, &sb);
            IDirect3DDevice9_GetRenderState(device, D3DRS_DESTBLEND, &db);
            IDirect3DDevice9_GetRenderState(device, D3DRS_BLENDOP, &op);
            const PixelShaderDef *ps = (const PixelShaderDef *)nv_pixel_shader;
            printf("[gpu]   blend %lu %lu op %lu, pixel shader %p c0 %08lx c1 %08lx final %08lx %08lx\n", sb, db, op, (const void *)ps,
                   ps ? (unsigned long)ps->constant0[0] : 0ul, ps ? (unsigned long)ps->constant1[0] : 0ul,
                   ps ? (unsigned long)ps->final_constant0 : 0ul, ps ? (unsigned long)ps->final_constant1 : 0ul);
            if (bound[0].texture)
                dump_texture(bound[0].texture, bound[0].xbox);
        }
    }
}

/* DRA2_SNAPSHOT=<frame>[:<step>],<path.bmp>: save that frame's back buffer (and every step frames after it), to look
 * at the output without a screen capture. A %d in the path is replaced by the frame number. */
static void snapshot(void)
{
    static int frame, target = -1, step;
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
                char *colon = strchr(v, ':');
                step = colon ? atoi(colon + 1) : 0;
                strncpy(path, comma + 1, sizeof path - 1);
            }
        }
    }
    if (++frame != target || !path[0])
        return;
    if (step > 0)
        target += step;
    char name[MAX_PATH + 16];
    snprintf(name, sizeof name, path, frame);
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
            FILE *f = fopen(name, "wb");
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
                printf("[gpu] frame %d saved to %s\n", frame, name);
                fflush(stdout);  /* test runs end by killing the game */
            }
            IDirect3DSurface9_UnlockRect(copy);
        }
    }
    if (copy)
        IDirect3DSurface9_Release(copy);
    IDirect3DSurface9_Release(back);
}

/* frames presented so far (DRA2_INPUT in port/xapi.c counts them) */
unsigned gpu_frame(void) { return texture_frame; }

/* The Xbox's soft display (D3DRS_SOFTDISPLAY, on unless a game turns it off, and this one doesn't): the video encoder
 * blends each line with its neighbours. Without it the art's fine horizontal detail, such as the stripes in the HUD
 * panels, is much harsher than on the console. This does the same on the finished frame, with weights 1-2-1.
 * DRA2_SOFT=0 turns it off. */
static void soft_display(void)
{
    static int state = -1;  /* -1 not set up, 0 off or failed, 1 on */
    static IDirect3DTexture9 *copy;
    static IDirect3DPixelShader9 *shader;
    static IDirect3DStateBlock9 *saved;
    static float line;  /* one line, in texture coordinates */
    IDirect3DSurface9 *back = 0, *target = 0, *copy_surface = 0;
    D3DSURFACE_DESC desc;
    if (state == 0)
        return;
    if (IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back) != D3D_OK)
        return;
    IDirect3DSurface9_GetDesc(back, &desc);
    if (state == -1) {
        char v[8] = "";
        GetEnvironmentVariableA("DRA2_SOFT", v, sizeof v);
        state = 0;
        typedef HRESULT(WINAPI *D3DCompileFn)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                               const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
        HMODULE m = v[0] == '0' ? 0 : LoadLibraryA("d3dcompiler_47.dll");
        D3DCompileFn compile = m ? (D3DCompileFn)(void *)GetProcAddress(m, "D3DCompile") : 0;
        static const char source[] =
            "sampler2D s : register(s0);\n"
            "float4 down : register(c0);\n"
            "float4 main(float2 t : TEXCOORD0) : COLOR\n"
            "{\n"
            "  return 0.25 * tex2D(s, t - down.xy) + 0.5 * tex2D(s, t) + 0.25 * tex2D(s, t + down.xy);\n"
            "}\n";
        ID3DBlob *blob = 0, *errors = 0;
        if (compile && SUCCEEDED(compile(source, sizeof source - 1, "soft", 0, 0, "main", "ps_2_0", 0, 0, &blob, &errors)) &&
            SUCCEEDED(IDirect3DDevice9_CreatePixelShader(device, (const DWORD *)ID3D10Blob_GetBufferPointer(blob), &shader)) &&
            SUCCEEDED(IDirect3DDevice9_CreateTexture(device, desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                     D3DPOOL_DEFAULT, &copy, 0)) &&
            SUCCEEDED(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &saved)))
            state = 1;
        if (blob)
            ID3D10Blob_Release(blob);
        if (errors)
            ID3D10Blob_Release(errors);
        line = 1.0f / (float)desc.Height;
        printf("[gpu] soft display %s\n", state ? "on" : v[0] == '0' ? "off (DRA2_SOFT=0)" : "unavailable");
        if (!state) {
            IDirect3DSurface9_Release(back);
            return;
        }
    }
    IDirect3DTexture9_GetSurfaceLevel(copy, 0, &copy_surface);
    IDirect3DDevice9_StretchRect(device, back, 0, copy_surface, 0, D3DTEXF_NONE);
    IDirect3DSurface9_Release(copy_surface);

    /* the game's state stays as it was: the state block holds all of it but the render target */
    IDirect3DStateBlock9_Capture(saved);
    IDirect3DDevice9_GetRenderTarget(device, 0, &target);
    IDirect3DDevice9 *d = device;
    IDirect3DDevice9_SetRenderTarget(d, 0, back);
    D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0, 1 };
    IDirect3DDevice9_SetViewport(d, &vp);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetVertexShader(d, 0);
    IDirect3DDevice9_SetPixelShader(d, shader);
    float c0[4] = { 0, line, 0, 0 };
    IDirect3DDevice9_SetPixelShaderConstantF(d, 0, c0, 1);
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)copy);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    float w = (float)desc.Width - 0.5f, h = (float)desc.Height - 0.5f;  /* texel centres on pixel centres */
    float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w, -0.5f, 0, 1, 1, 0 },
                         { -0.5f, h, 0, 1, 0, 1 }, { w, h, 0, 1, 1, 1 } };
    IDirect3DDevice9_BeginScene(d);
    IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
    IDirect3DDevice9_EndScene(d);
    IDirect3DDevice9_SetTexture(d, 0, 0);
    IDirect3DDevice9_SetRenderTarget(d, 0, target);
    IDirect3DStateBlock9_Apply(saved);
    if (target)
        IDirect3DSurface9_Release(target);
    IDirect3DSurface9_Release(back);
}

void gpu_present(void)
{
    texture_frame++;
    if (device) {
        if (in_scene) {
            IDirect3DDevice9_EndScene(device);
            in_scene = 0;
        }
        soft_display();
        snapshot();
        /* the largest rectangle of the picture's shape (4:3, or wider with DRA2_ASPECT) in the middle of the window */
        RECT client, picture;
        GetClientRect(window, &client);
        LONG w = client.right, h = client.bottom;
        if (w * (LONG)buffer_height > h * (LONG)buffer_width)
            w = h * (LONG)buffer_width / (LONG)buffer_height;
        else
            h = w * (LONG)buffer_height / (LONG)buffer_width;
        picture.left = (client.right - w) / 2;
        picture.top = (client.bottom - h) / 2;
        picture.right = picture.left + w;
        picture.bottom = picture.top + h;
        IDirect3DDevice9_Present(device, 0, w > 0 && h > 0 ? &picture : 0, 0, 0);
    }
    pump_messages();
}
