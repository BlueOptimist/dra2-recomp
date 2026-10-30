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

#include "nv2a.h"
void *memcpy(void *dst, const void *src, unsigned n);
extern float nv_zscale;

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
        printf("[d3d] frame %d (%u draws so far)\n", frames, nv_draws);
    if (nv_log_frames > 0)
        nv_log_frames--;
    return 0;
}

/* ------------------------------------------------------------------------------------------- vertex shaders -- */
/* A vertex shader object (the handle is its address + 1): the declaration says which stream and offset feeds each
 * input register; the function is the compiled NV2A program, [type, instruction count] then 4 dwords each. */
typedef struct {
    struct { int stream, offset; unsigned type; } reg[16];
    unsigned used;                     /* bit per declared register */
    const DWORD *program;
    unsigned instructions;
} VertexShader;

static VertexShader *current_shader;  /* 0: a fixed-function FVF */
static DWORD current_fvf;
static struct { const unsigned char *data; unsigned stride; } streams[16];

/* Xbox D3DVSDT_*: low nibble the NV2A array type, high nibble the component count */
static unsigned type_bytes(unsigned type)
{
    unsigned nv = type & 0xf, n = type >> 4;
    return nv == 2 ? 4 * n : nv == 1 || nv == 5 ? 2 * n : nv == 6 ? 4 : n;
}

DWORD __thiscall D3D8__D3DDevice_CreateVertexShader(void *self, DWORD *declaration, unsigned short *function,
                                                    DWORD *handle, DWORD usage)
{
    (void)self, (void)usage;
    VertexShader *vs = (VertexShader *)VirtualAlloc(0, sizeof *vs, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int stream = 0, offset = 0;
    for (const DWORD *t = declaration; t && *t != 0xFFFFFFFF; t++) {
        DWORD kind = *t >> 29;
        if (kind == 1) {                      /* D3DVSD_STREAM(n) */
            stream = (int)(*t & 0xf), offset = 0;
        } else if (kind == 2 && (*t & 0x10000000)) {   /* D3DVSD_SKIP(dwords) */
            offset += (int)(((*t >> 16) & 0xf) * 4);
        } else if (kind == 2) {               /* D3DVSD_REG(register, type) */
            unsigned r = *t & 0x1f, type = (*t >> 16) & 0xff;
            if (r < 16) {
                vs->reg[r].stream = stream, vs->reg[r].offset = offset, vs->reg[r].type = type;
                vs->used |= 1u << r;
            }
            offset += (int)type_bytes(type);
        } else if (kind == 4) {               /* D3DVSD_CONST(address, count): count x 4 dwords follow */
            t += ((*t >> 25) & 0xf) * 4;
        }
    }
    if (function) {
        vs->instructions = function[1];
        vs->program = (const DWORD *)function + 1;
    }
    *handle = (DWORD)vs + 1;
    printf("[d3d] CreateVertexShader %p: %u instructions, registers %04x\n", (void *)vs, vs->instructions, vs->used);
    return 0;
}

static void load_program(const DWORD *program, unsigned instructions, unsigned address)
{
    for (unsigned i = 0; i < instructions && address + i < NV_PROGRAM_SLOTS; i++)
        memcpy(nv_program[address + i], program + 4 * i, 16);
}

/* LoadVertexShaderProgram(program, address): [type, count] header, then the instructions */
void __stdcall D3D8__D3DDevice_LoadVertexShaderProgram(DWORD *program, int address)
{
    load_program(program + 1, ((unsigned short *)program)[1], (unsigned)address);
    static int programs_logged;
    if (programs_logged < 3) {
        extern int DAT_003166c8[], DAT_00316748;  /* the material's (feature, stage) list */
        printf("[d3d] vertex program features:");
        for (int i = 0; i < DAT_00316748 && i < 24; i++)
            printf(" %x/%d", DAT_003166c8[2 * i], DAT_003166c8[2 * i + 1]);
        printf("\n");
    }
    if (programs_logged++ < 3) {
        printf("[d3d] LoadVertexShaderProgram %u instructions at %d\n", ((unsigned short *)program)[1], address);
        nv_vsh_dump(address, 40);
    }
}

void __stdcall D3D8__D3DDevice_LoadVertexShader(int handle, unsigned address)
{
    VertexShader *vs = (VertexShader *)(handle - 1);
    if (handle & 1 && vs->program)
        load_program(vs->program, vs->instructions, address);
}

void __stdcall D3D8__D3DDevice_SelectVertexShader(unsigned handle, unsigned address)
{
    static int logged;
    if (logged++ < 6)
        printf("[d3d] SelectVertexShader %#x at %u\n", handle, address);
    if (handle & 1)
        current_shader = (VertexShader *)(handle - 1);
    nv_program_start = (int)address;
    nv_use_program = 1;
}

/* SetVertexShader(handle): a shader object (odd) is loaded at slot 0 and selected; anything else is an FVF */
void __stdcall D3D8__D3DDevice_SetVertexShader(unsigned handle)
{
    if (handle & 1) {
        current_shader = (VertexShader *)(handle - 1);
        load_program(current_shader->program, current_shader->instructions, 0);
        nv_program_start = 0;
        nv_use_program = 1;
    } else {
        current_shader = 0, current_fvf = handle;
        nv_use_program = 0;
    }
}

/* constants: the register number arrives biased (0..191 = c-96..c95) */
void __cdecl D3D8__D3DDevice_SetVertexShaderConstantNotInline(int reg, void *data, unsigned dwords)
{
    unsigned room = (unsigned)(NV_CONSTANTS - reg) * 4;
    if (reg >= 0 && reg < NV_CONSTANTS && data)
        memcpy(nv_constants[reg], data, (dwords < room ? dwords : room) * 4);
}

void __fastcall D3D8__D3DDevice_SetVertexShaderConstant1(int reg, void *data)
{
    if (reg >= 0 && reg < NV_CONSTANTS)
        memcpy(nv_constants[reg], data, 16);
}

void __fastcall D3D8__D3DDevice_SetVertexShaderConstant4(int reg, void *data)
{
    if (reg >= 0 && reg + 4 <= NV_CONSTANTS)
        memcpy(nv_constants[reg], data, 64);
}

/* SetShaderConstantMode: 96 or 192 constants; in 96 mode Direct3D keeps c-38/c-37 as the viewport transform that
 * compiled shaders end with (SetViewport) */
static DWORD constant_mode;
void __stdcall D3D8__D3DDevice_SetShaderConstantMode(unsigned mode) { constant_mode = mode; }

/* D3DVIEWPORT8: X, Y, Width, Height, MinZ, MaxZ */
typedef struct { DWORD x, y, width, height; float min_z, max_z; } XViewport;

void __stdcall D3D8__D3DDevice_SetViewport(unsigned *viewport)
{
    XViewport *v = (XViewport *)viewport;
    float *scale = nv_constants[96 - 38], *offset = nv_constants[96 - 37];
    static XViewport last;
    static int logged;
    if ((v->x != last.x || v->y != last.y || v->width != last.width || v->height != last.height) && logged++ < 8)
        printf("[d3d] SetViewport %lu,%lu %lux%lu z %g..%g\n", (unsigned long)v->x, (unsigned long)v->y,
               (unsigned long)v->width, (unsigned long)v->height, v->min_z, v->max_z);
    last = *v;
    scale[0] = v->width * 0.5f, scale[1] = -(float)v->height * 0.5f;
    scale[2] = nv_zscale * (v->max_z - v->min_z), scale[3] = 0.0f;
    offset[0] = v->x + v->width * 0.5f, offset[1] = v->y + v->height * 0.5f;
    offset[2] = nv_zscale * v->min_z, offset[3] = 0.0f;
}

/* --------------------------------------------------------------------------- streams, buffers and drawing -- */
void __stdcall D3D8__D3DDevice_SetStreamSource(int stream, int *buffer, int stride)
{
    if (stream >= 0 && stream < 16) {
        streams[stream].data = buffer ? (const unsigned char *)((XSurface *)buffer)->Data : 0;
        streams[stream].stride = (unsigned)stride;
    }
}

/* XGSetVertexBufferHeader(length, usage, fvf, pool, buffer, data) */
void __stdcall XGRAPHC__XGSetVertexBufferHeader(DWORD length, DWORD usage, DWORD fvf, DWORD pool, XSurface *buffer,
                                                DWORD data)
{
    (void)length, (void)usage, (void)fvf, (void)pool;
    buffer->Common = 1 | 0x00000000u;  /* D3DCOMMON_TYPE_VERTEXBUFFER */
    buffer->Data = data;
    buffer->Lock = 0;
}

/* D3DPushBuffer: Common, Data, Lock, Size, AllocationSize. XGSetPushBufferHeader(size, cpu copy, buffer, data) */
typedef struct { DWORD Common, Data, Lock, Size, AllocationSize; } XPushBuffer;

void __stdcall XGRAPHICS__XGSetPushBufferHeader(DWORD size, int cpu_copy, XPushBuffer *buffer, DWORD data)
{
    (void)cpu_copy;
    buffer->Common = 1 | 0x00040000u, buffer->Data = data, buffer->Lock = 0;
    buffer->Size = size, buffer->AllocationSize = size;
}

/* the vertex program's inputs from the current shader's declaration and the stream sources */
static void bind_attributes(void)
{
    for (int r = 0; r < 16; r++) {
        nv_attributes[r].data = 0, nv_attributes[r].count = 0;
        nv_attribute_defaults[r].v[0] = nv_attribute_defaults[r].v[1] = nv_attribute_defaults[r].v[2] = 0.0f;
        nv_attribute_defaults[r].v[3] = 1.0f;
        if (!current_shader || !(current_shader->used & (1u << r)))
            continue;
        int s = current_shader->reg[r].stream;
        if (!streams[s].data)
            continue;
        nv_attributes[r].data = streams[s].data + current_shader->reg[r].offset;
        nv_attributes[r].stride = streams[s].stride;
        nv_attributes[r].type = current_shader->reg[r].type & 0xf;
        nv_attributes[r].count = current_shader->reg[r].type >> 4;
    }
}

/* RunPushBuffer(push buffer, fixups): a model's draw commands, on the current shader and streams */
void __stdcall D3D8__D3DDevice_RunPushBuffer(int *buffer, int fixups)
{
    XPushBuffer *pb = (XPushBuffer *)buffer;
    (void)fixups;
    bind_attributes();
    static int logged;
    if (logged++ < 4) {
        printf("[d3d] RunPushBuffer %p: %lu bytes at %p:", (void *)pb, (unsigned long)pb->Size, (void *)pb->Data);
        for (unsigned i = 0; i < 16 && i < pb->Size / 4; i++)
            printf(" %08lx", (unsigned long)((DWORD *)pb->Data)[i]);
        printf("\n");
    }
    nv_run_pushbuffer((const unsigned *)pb->Data, pb->Size / 4);
}

/* Draws from user memory with a fixed-function FVF or the current shader. Pretransformed FVF vertices
 * (D3DFVF_XYZRHW) go straight through; the others run the vertex program on stream 0 = the user data. */
#define FVF_XYZRHW 0x004
#define FVF_DIFFUSE 0x040
#define FVF_SPECULAR 0x080

static GpuVertex up_vertices[0x4000];

static void fvf_vertex(const unsigned char *p, GpuVertex *g)
{
    const float *f = (const float *)p;
    g->x = f[0], g->y = f[1], g->z = f[2], g->rhw = f[3];
    p += 16;
    g->diffuse = 0xffffffff, g->specular = 0;
    if (current_fvf & FVF_DIFFUSE)
        g->diffuse = *(const DWORD *)p, p += 4;
    if (current_fvf & FVF_SPECULAR)
        g->specular = *(const DWORD *)p, p += 4;
    unsigned textures = (current_fvf >> 8) & 0xf;
    for (unsigned t = 0; t < 4; t++) {
        g->t[t][0] = g->t[t][1] = g->t[t][2] = 0.0f, g->t[t][3] = 1.0f;
        if (t < textures)
            g->t[t][0] = ((const float *)p)[0], g->t[t][1] = ((const float *)p)[1], p += 8;
    }
}

static void draw_up(int primitive, unsigned count, const unsigned short *indices, const unsigned char *data,
                    unsigned stride)
{
    if (!nv_use_program) {
        if ((current_fvf & 0xe) != FVF_XYZRHW) {
            static int warned;
            if (!warned++)
                printf("[d3d] fixed-function FVF %#lx (not pretransformed) not drawn yet\n", (unsigned long)current_fvf);
            return;
        }
        if (count > sizeof up_vertices / sizeof *up_vertices)
            count = sizeof up_vertices / sizeof *up_vertices;
        for (unsigned i = 0; i < count; i++)
            fvf_vertex(data + (indices ? indices[i] : i) * stride, &up_vertices[i]);
        static int logged;
        if (logged++ < 4) {
            const unsigned *w = (const unsigned *)data;
            printf("[d3d] FVF %#lx draw: primitive %d, %u vertices, stride %u, data %p:", (unsigned long)current_fvf,
                   primitive, count, stride, (const void *)data);
            for (unsigned i = 0; i < stride / 4 && i < 10; i++)
                printf(" %08x", w[i]);
            printf("\n");
        }
        gpu_draw(primitive, up_vertices, count);
        return;
    }
    streams[0].data = data, streams[0].stride = stride;
    bind_attributes();
    if (indices) {
        static unsigned list[0x4000];
        if (count > 0x4000)
            count = 0x4000;
        for (unsigned i = 0; i < count; i++)
            list[i] = indices[i];
        nv_draw_indexed((unsigned)primitive, list, count);
    } else {
        nv_draw_arrays((unsigned)primitive, 0, count);
    }
}

void __stdcall D3D8__D3DDevice_DrawVerticesUP(int primitive, unsigned count, void *data, unsigned stride)
{
    draw_up(primitive, count, 0, (const unsigned char *)data, stride);
}

void __stdcall D3D8__D3DDevice_DrawIndexedVerticesUP(int primitive, unsigned count, unsigned short *indices,
                                                     void *data, unsigned stride)
{
    draw_up(primitive, count, indices, (const unsigned char *)data, stride);
}

/* -------------------------------------------------------------------------------------- states and textures -- */
/* SetRenderState_Simple(method header, value): one NV2A method, as Direct3D would push it */
void __fastcall D3D8__D3DDevice_SetRenderState_Simple(DWORD header, DWORD value) { nv_method(header & 0x1ffc, value); }

/* CullMode: the vertices reach Direct3D 9 already in screen space, so culling stays off for now */
void __stdcall D3D8__D3DDevice_SetRenderState_CullMode(int mode) { (void)mode; }

void __stdcall D3D8__D3DDevice_SetTexture(int stage, int *texture) { gpu_set_texture(stage, texture); }

/* the fixed-function transforms: kept for FVF draws that need them (none drawn yet) */
static float transforms[32][16];
void __stdcall D3D8__D3DDevice_SetTransform(int state, float *matrix)
{
    if (state >= 0 && state < 32)
        memcpy(transforms[state & 31], matrix, 64);
    static int logged;
    if (logged++ < 6) {
        printf("[d3d] SetTransform %d:", state);
        for (int i = 0; i < 16; i++)
            printf(" %g", matrix[i]);
        printf("\n");
    }
}

/* XGSetTextureHeader(width, height, levels, usage, format, pool, texture, data, pitch): a swizzled texture has its
 * size as log2 fields in Format; a linear one (LIN_ formats) has width, height and pitch in Size */
static int linear_format(unsigned f)
{
    return (f >= 0x10 && f <= 0x18) || (f >= 0x1b && f <= 0x20) || (f >= 0x2e && f <= 0x30) ||
           (f >= 0x35 && f <= 0x37) || f == 0x40 || f == 0x41;
}

static unsigned log2u(unsigned v)
{
    unsigned r = 0;
    while ((1u << r) < v)
        r++;
    return r;
}

void __stdcall XGRAPHC__XGSetTextureHeader(unsigned width, unsigned height, unsigned levels, unsigned usage,
                                           unsigned format, DWORD pool, XSurface *texture, DWORD data,
                                           unsigned pitch)
{
    (void)usage, (void)pool;
    texture->Common = 1 | 0x00040000u;  /* D3DCOMMON_TYPE_TEXTURE */
    texture->Data = data;
    texture->Lock = 0;
    if (linear_format(format)) {
        texture->Format = FORMAT_DMACHANNEL_A | FORMAT_DIMENSION_2D | (format << 8) | (1u << 16);
        texture->Size = ((width - 1) & 0xFFF) | (((height - 1) & 0xFFF) << 12) | ((((pitch / 64) - 1) & 0xFF) << 24);
    } else {
        texture->Format = FORMAT_DMACHANNEL_A | FORMAT_DIMENSION_2D | (format << 8) | ((levels ? levels : 1) << 16) |
                          (log2u(width) << 20) | (log2u(height) << 24);
        texture->Size = 0;
    }
}

/* SetPixelShaderProgram(D3DPIXELSHADERDEF *): the register combiners. Kept for gpu.c, which approximates them. */
void *nv_pixel_shader;

/* D3DPIXELSHADERDEF (Xbox) */
typedef struct {
    DWORD alpha_inputs[8], final_abcd, final_efg, constant0[8], constant1[8], alpha_outputs[8], rgb_inputs[8];
    DWORD compare_mode, final_constant0, final_constant1, rgb_outputs[8], combiner_count, texture_modes, dot_mapping;
    DWORD input_texture, c0_mapping, c1_mapping, final_constants;
} XPixelShaderDef;

void __stdcall D3D8__D3DDevice_SetPixelShaderProgram(int program)
{
    nv_pixel_shader = (void *)program;
    static const void *seen[8];
    static int count;
    const XPixelShaderDef *ps = (const XPixelShaderDef *)program;
    if (!ps || count >= 8)
        return;
    for (int i = 0; i < count; i++)
        if (seen[i] == ps)
            return;
    seen[count++] = ps;
    unsigned stages = ps->combiner_count & 0xff;
    printf("[d3d] pixel shader %p: %u stages, texture modes %08lx, final ABCD %08lx EFG %08lx\n", (void *)ps, stages,
           (unsigned long)ps->texture_modes, (unsigned long)ps->final_abcd, (unsigned long)ps->final_efg);
    for (unsigned i = 0; i < stages && i < 8; i++)
        printf("[d3d]   stage %u: rgb in %08lx out %08lx, alpha in %08lx out %08lx, c0 %08lx c1 %08lx\n", i,
               (unsigned long)ps->rgb_inputs[i], (unsigned long)ps->rgb_outputs[i], (unsigned long)ps->alpha_inputs[i],
               (unsigned long)ps->alpha_outputs[i], (unsigned long)ps->constant0[i], (unsigned long)ps->constant1[i]);
}

/* the push buffer is the CPU's own memory here: nothing to wait for */
void __stdcall D3D8__D3D_KickOffAndWaitForIdle(void) {}
void __stdcall D3D8__D3D_SetPushBufferSize(DWORD size, DWORD kickoff) { (void)size, (void)kickoff; }
