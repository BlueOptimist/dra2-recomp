/* Direct3D 8 (Xbox) on Windows: the first part of the graphics layer.
 *
 * Every function keeps the name, calling convention and parameter count of its prototype in build/program.h, so it
 * replaces the weak stub in xbox_stubs.c.
 *
 * The game manages its own framebuffers the Xbox way: it builds surface headers (XGSetSurfaceHeader), points them at
 * memory it allocated (D3D_AllocContiguousMemory + D3DResource_Register) and hands them to the device through the
 * present parameters. So this layer keeps the XDK's resource header layout and works on that memory directly:
 *   - Clear fills the render target's memory (software)
 *   - Swap copies the back buffer to a Win32 window with GDI and pumps window messages
 * Drawing (push buffers, vertex shaders, textures) is the next step; until then those calls stay stubs, so the window
 * shows whatever the game clears to.
 */
#include "win32_min.h"

int printf(const char *format, ...);
void *memset(void *p, int c, unsigned n);

/* ---------------------------------------------------------------------------------- resource headers (XDK) -- */
/* D3DResource: Common (reference count in bits 0-15, type in 16-18), Data (memory address once registered), Lock;
 * D3DPixelContainer adds Format and Size; a surface adds its Parent texture. */
typedef struct {
    DWORD Common, Data, Lock, Format, Size;
    void *Parent;
} XSurface;

#define COMMON_REFCOUNT_MASK 0x0000FFFFu
#define COMMON_TYPE_SURFACE 0x00050000u
#define FORMAT_DMACHANNEL_A 0x00000001u
#define FORMAT_DIMENSION_2D 0x00000020u

static unsigned surface_width(const XSurface *s) { return (s->Size & 0xFFF) + 1; }
static unsigned surface_height(const XSurface *s) { return ((s->Size >> 12) & 0xFFF) + 1; }
static unsigned surface_pitch(const XSurface *s) { return (((s->Size >> 24) & 0xFF) + 1) * 64; }
static unsigned surface_format(const XSurface *s) { return (s->Format >> 8) & 0xFF; }

/* XGSetSurfaceHeader(width, height, format, surface, data offset, pitch): a linear surface header */
void __stdcall XGRAPHC__XGSetSurfaceHeader(unsigned width, unsigned height, unsigned format, XSurface *surface,
                                           DWORD data, unsigned pitch)
{
    surface->Common = 1 | COMMON_TYPE_SURFACE;
    surface->Data = data;
    surface->Lock = 0;
    surface->Format = FORMAT_DMACHANNEL_A | FORMAT_DIMENSION_2D | (format << 8);
    surface->Size = ((width - 1) & 0xFFF) | (((height - 1) & 0xFFF) << 12) | ((((pitch / 64) - 1) & 0xFF) << 24);
    surface->Parent = 0;
}

/* D3DResource_Register(resource, base): Data was an offset into the memory at base */
void __stdcall D3D8__D3DResource_Register(XSurface *resource, int base) { resource->Data += (DWORD)base; }

static void add_ref(XSurface *s)
{
    if (s)
        s->Common = (s->Common & ~COMMON_REFCOUNT_MASK) | (((s->Common & COMMON_REFCOUNT_MASK) + 1) & 0xFFFF);
}

DWORD __stdcall D3D8__D3DResource_Release(XSurface *s)
{
    DWORD count = s->Common & COMMON_REFCOUNT_MASK;
    if (count)
        count--;
    s->Common = (s->Common & ~COMMON_REFCOUNT_MASK) | count;
    return count;
}

/* D3D_AllocContiguousMemory(size, alignment): memory the GPU reads directly. On Windows it is ordinary committed
 * memory; VirtualAlloc's 64 KB alignment covers any alignment asked for. */
unsigned int __stdcall D3D8__D3D_AllocContiguousMemory(unsigned int size, unsigned int alignment)
{
    (void)alignment;
    return (unsigned int)VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

/* ------------------------------------------------------------------------------------------- the device -- */
/* D3DPRESENT_PARAMETERS (Xbox): the game's own buffers come in BufferSurfaces / DepthStencilSurface */
typedef struct {
    unsigned BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount;
    DWORD MultiSampleType, SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags, FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
    XSurface *BufferSurfaces[3];
    XSurface *DepthStencilSurface;
} XPresentParameters;

static unsigned char device_block[0x4000];  /* what the game gets as its device pointer; its contents are ours */
static XPresentParameters present;
static XSurface *render_target, *depth_target;
static HWND window;
static int frames;

static LRESULT WINAPI window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY)
        ExitProcess(0);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void open_window(unsigned width, unsigned height)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "DRA2Window";
    RegisterClassA(&wc);
    RECT r = { 0, 0, (long)width * 2, (long)height * 2 };  /* 640x480 shown at 2x */
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);
    window = CreateWindowExA(0, wc.lpszClassName, "Digimon Rumble Arena 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* Direct3D_CreateDevice(adapter, device type, window, behaviour flags, present parameters, out device) */
int __stdcall D3D8__Direct3D_CreateDevice(DWORD adapter, DWORD type, DWORD hwnd, DWORD flags,
                                          XPresentParameters *params, void **device)
{
    (void)adapter, (void)type, (void)hwnd, (void)flags;
    present = *params;
    render_target = present.BufferSurfaces[0];
    depth_target = present.DepthStencilSurface;
    printf("[d3d] CreateDevice %ux%u format %#x, back buffer %p (%ux%u, pitch %u, format %#x), depth %p\n",
           present.BackBufferWidth, present.BackBufferHeight, present.BackBufferFormat, (void *)render_target,
           render_target ? surface_width(render_target) : 0, render_target ? surface_height(render_target) : 0,
           render_target ? surface_pitch(render_target) : 0, render_target ? surface_format(render_target) : 0,
           (void *)depth_target);
    open_window(present.BackBufferWidth ? present.BackBufferWidth : 640,
                present.BackBufferHeight ? present.BackBufferHeight : 480);
    *device = device_block;
    return 0;  /* D3D_OK */
}

/* GetBackBuffer2(index): the game's own back buffer; -1 is the front buffer */
DWORD __stdcall D3D8__D3DDevice_GetBackBuffer2(int index)
{
    XSurface *s = present.BufferSurfaces[index < 0 ? 0 : index > 2 ? 2 : index];
    if (!s)
        s = present.BufferSurfaces[0];
    add_ref(s);
    return (DWORD)s;
}

int __stdcall D3D8__D3DDevice_GetDepthStencilSurface2(void)
{
    add_ref(depth_target);
    return (int)depth_target;
}

int __stdcall D3D8__D3DDevice_GetRenderTarget2(void)
{
    add_ref(render_target);
    return (int)render_target;
}

void __stdcall D3D8__D3DDevice_SetRenderTarget(int target, int depth)
{
    if (target)
        render_target = (XSurface *)target;
    depth_target = (XSurface *)depth;
}

/* D3DGAMMARAMP: red, green, blue, 256 bytes each */
static unsigned char gamma_ramp[3][256];
static int gamma_set;

void __stdcall D3D8__D3DDevice_GetGammaRamp(unsigned char *ramp)
{
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++)
            ramp[c * 256 + i] = gamma_set ? gamma_ramp[c][i] : (unsigned char)i;
}

void __stdcall D3D8__D3DDevice_SetGammaRamp(unsigned char flags, unsigned char *ramp)
{
    (void)flags;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++)
            gamma_ramp[c][i] = ramp[c * 256 + i];
    gamma_set = 1;
}

/* SetTile: tiled (compressed) memory regions on the NV2A; the CPU sees the memory linear either way */
void __stdcall D3D8__D3DDevice_SetTile(int index, int *tile) { (void)index, (void)tile; }

/* ------------------------------------------------------------------------------- clear and present (CPU) -- */
#define D3DCLEAR_TARGET 0x000000F0u  /* Xbox: R, G, B and A bits */

typedef struct { long x1, y1, x2, y2; } XRect;

static void fill(XSurface *s, long x1, long y1, long x2, long y2, DWORD color)
{
    if (!s || !s->Data)
        return;
    long w = (long)surface_width(s), h = (long)surface_height(s);
    unsigned pitch = surface_pitch(s);
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w) x2 = w;
    if (y2 > h) y2 = h;
    for (long y = y1; y < y2; y++) {
        DWORD *row = (DWORD *)((unsigned char *)s->Data + y * pitch);
        for (long x = x1; x < x2; x++)
            row[x] = color;
    }
}

/* Clear(count, rects, flags, color, z, stencil): colour only for now (32-bit targets) */
void __stdcall D3D8__D3DDevice_Clear(int count, int *rects, unsigned flags, unsigned color, DWORD z, DWORD stencil)
{
    (void)z, (void)stencil;
    if (!(flags & D3DCLEAR_TARGET) || !render_target)
        return;
    if (count <= 0 || !rects) {
        fill(render_target, 0, 0, 0x7fffffff, 0x7fffffff, color);
        return;
    }
    XRect *r = (XRect *)rects;
    for (int i = 0; i < count; i++)
        fill(render_target, r[i].x1, r[i].y1, r[i].x2, r[i].y2, color);
}

/* Swap(flags): show the back buffer. 32-bit A8R8G8B8 / X8R8G8B8 is what GDI calls a 32-bit BGRA DIB. */
DWORD __stdcall D3D8__D3DDevice_Swap(unsigned flags)
{
    (void)flags;
    XSurface *s = present.BufferSurfaces[0];
    if (window && s && s->Data) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = (long)(surface_pitch(s) / 4);
        bmi.bmiHeader.biHeight = -(long)surface_height(s);  /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        RECT client;
        GetClientRect(window, &client);
        HDC dc = GetDC(window);
        StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, (int)surface_width(s), (int)surface_height(s),
                      (void *)s->Data, &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(window, dc);
    }
    pump_messages();
    if (++frames == 1 || frames % 300 == 0)
        printf("[d3d] frame %d\n", frames);
    return 0;
}
