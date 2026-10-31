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

/* -------------------------------------------------------------------------------- register combiners -- */
/* The Xbox's pixel stage is the NV2A's register combiners, set as a D3DPIXELSHADERDEF (SetPixelShaderProgram). Each
 * program is translated once into Direct3D 9 ps_2_0 bytecode, emitted directly (no shader compiler is available):
 *  - NV registers live in temporaries: R0 r0, R1 r1, T0-T3 r2-r5 (loaded with texld), V0/V1 r6/r7 (the diffuse and
 *    specular colours); r8-r11 are scratch. c0-c15 hold each stage's C0/C1, c16/c17 the final combiner's, c18 the fog
 *    colour, c20 = (0, 1, 0.5, 2).
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
    case 3: return "c[18]";
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
        "            float4 t2 : TEXCOORD2, float4 t3 : TEXCOORD3) : COLOR\n{\n");
    put("  float4 V0 = v0, V1 = v1, R1 = 0;\n");
    for (int t = 0; t < 4; t++) {
        unsigned mode = (ps->texture_modes >> (5 * t)) & 0x1f;
        if (mode)
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
/* translated programs, by the definition's contents */
static struct { PixelShaderDef def; IDirect3DPixelShader9 *shader; } shaders[256];
static int shader_count;

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
    for (int i = 0; i < shader_count; i++)
        if (!memcmp(&shaders[i].def, ps, sizeof *ps)) {
            shader = shaders[i].shader;
            break;
        }
    if (!shader) {
        shader = translate(ps);
        if (shader_count < (int)(sizeof shaders / sizeof *shaders)) {
            shaders[shader_count].def = *ps;
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
    set_constant_color(18, 0xff000000);  /* fog: colour black, factor 1 (no fog yet) */
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
    /* DRA2_FLAT=1: every draw in magenta with depth, blending and alpha test off (is the geometry reaching the
     * screen at all?); 2: only the states off; 3: only the vertex colours white. The register combiners still run,
     * so textures still show through. */
    static int flat = -1;
    if (flat < 0) {
        char v[8] = "";
        flat = GetEnvironmentVariableA("DRA2_FLAT", v, sizeof v) > 0 ? atoi(v) : 0;
    }
    if (flat == 3)
        for (unsigned i = 0; i < n; i++)
            scratch[i].diffuse = 0xffffffff;
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
