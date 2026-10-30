/* Direct3D 8 (Xbox) on Windows: the Xbox side of the graphics layer.
 *
 * Every function keeps the name, calling convention and parameter count of its prototype in build/program.h, so it
 * replaces the weak stub in xbox_stubs.c.
 *
 * The game manages its own framebuffers the Xbox way: it builds surface headers (XGSetSurfaceHeader), points them at
 * memory it allocated (D3D_AllocContiguousMemory + D3DResource_Register) and hands them to the device through the
 * present parameters. This layer keeps the XDK's resource header layout for the game's sake, and renders through a
 * Direct3D 9 device (port/gpu.c):
 *   - Clear clears the Direct3D 9 back buffer and depth buffer
 *   - Swap presents it and pumps window messages
 * Drawing (push buffers, vertex shaders, textures) is the next step; until then those calls stay stubs.
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
static int frames;

/* port/gpu.c */
int gpu_open(unsigned width, unsigned height);
void gpu_clear(int count, const long *rects, unsigned flags, unsigned color, float z, unsigned stencil);
void gpu_present(void);

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
    gpu_open(present.BackBufferWidth ? present.BackBufferWidth : 640,
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

/* ------------------------------------------------------------------------------------- clear and present -- */
/* Clear(count, rects, flags, color, z, stencil): z arrives as the bits of a float. D3DRECT has the same layout on
 * both sides (x1, y1, x2, y2). */
void __stdcall D3D8__D3DDevice_Clear(int count, int *rects, unsigned flags, unsigned color, DWORD z, DWORD stencil)
{
    union { DWORD bits; float f; } depth = { z };
    gpu_clear(count, (const long *)rects, flags, color, depth.f, stencil);
}

/* Swap(flags): present the frame */
DWORD __stdcall D3D8__D3DDevice_Swap(unsigned flags)
{
    (void)flags;
    gpu_present();
    if (++frames == 1 || frames % 300 == 0)
        printf("[d3d] frame %d\n", frames);
    return 0;
}
