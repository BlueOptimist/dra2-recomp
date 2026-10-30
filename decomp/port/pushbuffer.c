/* NV2A push buffers and methods (see port/nv2a.c): the Kelvin (3D class) methods the game's push buffers and Direct3D
 * calls use, decoded into draws. Every draw goes vertex by vertex through the vertex program interpreter and reaches
 * Direct3D 9 (port/gpu.c) as pretransformed vertices. */
#include "nv2a.h"

int printf(const char *format, ...);

NvAttribute nv_attributes[16];
NvVec4 nv_attribute_defaults[16];
int nv_use_program = 1;
int nv_log_frames = 2;
unsigned nv_draws;  /* draws so far (the frame counter line reports them) */

/* Kelvin methods */
#define NV_BEGIN_END 0x17fc
#define NV_ARRAY_ELEMENT16 0x1800
#define NV_ARRAY_ELEMENT32 0x1808
#define NV_DRAW_ARRAYS 0x1810
#define NV_INLINE_ARRAY 0x1818
#define NV_VERTEX_DATA_ARRAY_OFFSET 0x1720  /* + 4 * attribute */
#define NV_VERTEX_DATA_ARRAY_FORMAT 0x1760  /* + 4 * attribute */
#define NV_TRANSFORM_PROGRAM 0x0b00         /* 32 words */
#define NV_TRANSFORM_CONSTANT 0x0b80        /* 32 words */
#define NV_TRANSFORM_PROGRAM_LOAD 0x1e9c
#define NV_TRANSFORM_PROGRAM_START 0x1ea0
#define NV_TRANSFORM_CONSTANT_LOAD 0x1ea4

static unsigned primitive;                /* inside BEGIN_END: the primitive type, else 0 */
static unsigned indices[0x10000], index_count;
static unsigned inline_words[0x20000], inline_count;
static unsigned program_load, constant_load;  /* load cursors: instruction slot, constant register (+96) */

/* ------------------------------------------------------------------------------------------ vertex fetch -- */
static float s1(const unsigned char *p) { return (float)*(const short *)p / 32767.0f; }

static void fetch(const NvAttribute *a, const unsigned char *p, NvVec4 *out)
{
    unsigned n = a->count;
    switch (a->type) {
    case 2:  /* F */
        for (unsigned i = 0; i < n && i < 4; i++)
            out->v[i] = ((const float *)p)[i];
        break;
    case 0:  /* UB_D3D: a D3DCOLOR, BGRA in memory -> r, g, b, a */
        if (n >= 4) {
            out->v[0] = p[2] / 255.0f, out->v[1] = p[1] / 255.0f, out->v[2] = p[0] / 255.0f, out->v[3] = p[3] / 255.0f;
            break;
        }
        /* fallthrough */
    case 4:  /* UB_OGL */
        for (unsigned i = 0; i < n && i < 4; i++)
            out->v[i] = p[i] / 255.0f;
        break;
    case 1:  /* S1: normalized shorts */
        for (unsigned i = 0; i < n && i < 4; i++)
            out->v[i] = s1(p + 2 * i);
        break;
    case 5:  /* S32K: shorts as they are */
        for (unsigned i = 0; i < n && i < 4; i++)
            out->v[i] = (float)((const short *)p)[i];
        break;
    case 6: {  /* CMP: 11:11:10 signed normalized */
        unsigned w = *(const unsigned *)p;
        int x = (int)(w << 21) >> 21, y = (int)(w << 10) >> 21, z = (int)w >> 22;
        out->v[0] = x / 1023.0f, out->v[1] = y / 1023.0f, out->v[2] = z / 511.0f;
        break;
    }
    }
}

static unsigned pack_color(const float *c)
{
    unsigned r = 0;
    for (int i = 0; i < 4; i++) {
        float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        r |= (unsigned)(f * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);  /* r g b -> ARGB */
    }
    return r;
}

float nv_zscale = 16777215.0f;  /* 24-bit depth buffer */

static void make_vertex(const NvVec4 in[16], GpuVertex *g)
{
    NvVec4 out[NV_OUTPUTS];
    /* outputs a program doesn't write: colours white (the combiners then see the texture as it is), the rest 0,0,0,1 */
    for (int i = 0; i < NV_OUTPUTS; i++) {
        float c = i == NV_OUT_D0 || i == NV_OUT_D1 ? 1.0f : 0.0f;
        out[i].v[0] = out[i].v[1] = out[i].v[2] = c, out[i].v[3] = 1.0f;
    }
    nv_vsh_run(nv_program_start, (NvVec4 *)in, out);
    float w = out[NV_OUT_POS].v[3];
    g->x = out[NV_OUT_POS].v[0], g->y = out[NV_OUT_POS].v[1];
    g->z = out[NV_OUT_POS].v[2] / nv_zscale;
    g->rhw = w != 0.0f ? 1.0f / w : 1.0f;
    g->diffuse = pack_color(out[NV_OUT_D0].v);
    g->specular = pack_color(out[NV_OUT_D1].v);
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 4; i++)
            g->t[t][i] = out[NV_OUT_T0 + t].v[i];
}

static void vertex_inputs(unsigned index, NvVec4 in[16])
{
    for (int a = 0; a < 16; a++) {
        in[a] = nv_attribute_defaults[a];
        if (nv_attributes[a].data && nv_attributes[a].count)
            fetch(&nv_attributes[a], nv_attributes[a].data + index * nv_attributes[a].stride, &in[a]);
    }
}

static GpuVertex vertices[0x10000];
static int draws_logged;

static void log_draw(const char *kind, unsigned prim, unsigned count)
{
    nv_draws++;
    static int nan_logged;
    if (count && vertices[0].x != vertices[0].x && nan_logged++ < 3) {
        printf("[nv2a] NaN draw: %s primitive %u, %u vertices, program start %d\n", kind, prim, count, nv_program_start);
        for (int a = 0; a < 16; a++)
            if (nv_attributes[a].data && nv_attributes[a].count) {
                const float *f = (const float *)nv_attributes[a].data;
                printf("[nv2a]   v%d: stride %u type %u x%u: %g %g %g %g (%08x at %p)\n", a, nv_attributes[a].stride,
                       nv_attributes[a].type, nv_attributes[a].count, f[0], f[1], f[2], f[3], *(const unsigned *)f,
                       (const void *)f);
            }
        for (int r = -38; r <= 3; r += (r == -37 ? 37 : 1)) {
            const float *c = nv_constants[r + 96];
            printf("[nv2a]   c%d: %g %g %g %g\n", r, c[0], c[1], c[2], c[3]);
        }
        nv_vsh_dump(nv_program_start, 20);
    }
    if (draws_logged >= 3)
        return;
    draws_logged++;
    printf("[nv2a] %s primitive %u, %u vertices, program start %d\n", kind, prim, count, nv_program_start);
    for (int a = 0; a < 16; a++)
        if (nv_attributes[a].data && nv_attributes[a].count) {
            const float *f = (const float *)nv_attributes[a].data;
            printf("[nv2a]   v%d: %p stride %u type %u x%u: %g %g %g %g (%08x)\n", a, (const void *)nv_attributes[a].data,
                   nv_attributes[a].stride, nv_attributes[a].type, nv_attributes[a].count, f[0], f[1], f[2], f[3],
                   *(const unsigned *)f);
        }
    for (unsigned i = 0; i < count && i < 3; i++)
        printf("[nv2a]   out %u: %.1f %.1f %.4f rhw %.4f d0 %08x t0 %.3f %.3f\n", i, vertices[i].x, vertices[i].y,
               vertices[i].z, vertices[i].rhw, vertices[i].diffuse, vertices[i].t[0][0], vertices[i].t[0][1]);
    if (draws_logged == 1) {
        static const int regs[] = { 0, 1, 2, 3, 12, 13, 14, 33, 34, 35, 36, -92, -38, -37 };
        for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++) {
            const float *c = nv_constants[regs[i] + 96];
            printf("[nv2a]   c%d: %g %g %g %g\n", regs[i], c[0], c[1], c[2], c[3]);
        }
    }
}

void nv_draw_indexed(unsigned prim, const unsigned *list, unsigned count)
{
    if (count > 0x10000)
        count = 0x10000;
    NvVec4 in[16];
    for (unsigned i = 0; i < count; i++) {
        vertex_inputs(list[i], in);
        make_vertex(in, &vertices[i]);
    }
    log_draw("indexed", prim, count);
    gpu_draw((int)prim, vertices, count);
}

void nv_draw_arrays(unsigned prim, unsigned first, unsigned count)
{
    if (count > 0x10000)
        count = 0x10000;
    NvVec4 in[16];
    for (unsigned i = 0; i < count; i++) {
        vertex_inputs(first + i, in);
        make_vertex(in, &vertices[i]);
    }
    log_draw("arrays", prim, count);
    gpu_draw((int)prim, vertices, count);
}

static unsigned attribute_bytes(const NvAttribute *a)
{
    return a->type == 2 ? 4 * a->count : a->type == 1 || a->type == 5 ? 2 * a->count : a->type == 6 ? 4 : a->count;
}

/* inline vertex data: each vertex is the enabled attributes in order, each padded to whole dwords */
static void draw_inline(unsigned prim)
{
    unsigned stride_words = 0;
    for (int a = 0; a < 16; a++)
        if (nv_attributes[a].count)
            stride_words += (attribute_bytes(&nv_attributes[a]) + 3) / 4;
    if (!stride_words)
        return;
    unsigned count = inline_count / stride_words;
    if (count > 0x10000)
        count = 0x10000;
    NvVec4 in[16];
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *p = (const unsigned char *)&inline_words[i * stride_words];
        for (int a = 0; a < 16; a++) {
            in[a] = nv_attribute_defaults[a];
            if (nv_attributes[a].count) {
                fetch(&nv_attributes[a], p, &in[a]);
                p += (attribute_bytes(&nv_attributes[a]) + 3) & ~3u;
            }
        }
        make_vertex(in, &vertices[i]);
    }
    log_draw("inline", prim, count);
    gpu_draw((int)prim, vertices, count);
}

/* ----------------------------------------------------------------------------------------------- methods -- */
static unsigned unknown_seen[64], unknown_count;

static void note_unknown(unsigned method, unsigned value)
{
    for (unsigned i = 0; i < unknown_count; i++)
        if (unknown_seen[i] == method)
            return;
    if (unknown_count < 64)
        unknown_seen[unknown_count++] = method;
    printf("[nv2a] method %04x (value %08x) not handled\n", method, value);
}

void nv_method(unsigned method, unsigned value)
{
    if (method >= NV_TRANSFORM_PROGRAM && method < NV_TRANSFORM_PROGRAM + 0x80) {
        unsigned word = (method - NV_TRANSFORM_PROGRAM) / 4;
        nv_program[program_load % NV_PROGRAM_SLOTS][word & 3] = value;
        if ((word & 3) == 3)
            program_load++;
        return;
    }
    if (method >= NV_TRANSFORM_CONSTANT && method < NV_TRANSFORM_CONSTANT + 0x80) {
        unsigned word = (method - NV_TRANSFORM_CONSTANT) / 4;
        union { unsigned u; float f; } bits = { value };
        nv_constants[constant_load % NV_CONSTANTS][word & 3] = bits.f;
        if ((word & 3) == 3)
            constant_load++;
        return;
    }
    if (method >= NV_VERTEX_DATA_ARRAY_OFFSET && method < NV_VERTEX_DATA_ARRAY_OFFSET + 0x40) {
        nv_attributes[(method - NV_VERTEX_DATA_ARRAY_OFFSET) / 4].data = (const unsigned char *)(value & 0x7fffffff);
        return;
    }
    if (method >= NV_VERTEX_DATA_ARRAY_FORMAT && method < NV_VERTEX_DATA_ARRAY_FORMAT + 0x40) {
        NvAttribute *a = &nv_attributes[(method - NV_VERTEX_DATA_ARRAY_FORMAT) / 4];
        a->type = value & 0xf, a->count = (value >> 4) & 0xf, a->stride = value >> 8;
        return;
    }
    switch (method) {
    case NV_BEGIN_END:
        if (value) {
            primitive = value, index_count = 0, inline_count = 0;
        } else if (primitive) {
            if (index_count)
                nv_draw_indexed(primitive, indices, index_count);
            else if (inline_count)
                draw_inline(primitive);
            primitive = 0;
        }
        return;
    case NV_ARRAY_ELEMENT16:
        if (index_count + 2 <= sizeof indices / sizeof *indices)
            indices[index_count++] = value & 0xffff, indices[index_count++] = value >> 16;
        return;
    case NV_ARRAY_ELEMENT32:
        if (index_count < sizeof indices / sizeof *indices)
            indices[index_count++] = value;
        return;
    case NV_DRAW_ARRAYS: {
        unsigned first = value & 0xffffff, count = (value >> 24) + 1;
        for (unsigned i = 0; i < count && index_count < sizeof indices / sizeof *indices; i++)
            indices[index_count++] = first + i;
        return;
    }
    case NV_INLINE_ARRAY:
        if (inline_count < sizeof inline_words / sizeof *inline_words)
            inline_words[inline_count++] = value;
        return;
    case NV_TRANSFORM_PROGRAM_LOAD:
        program_load = value;
        return;
    case NV_TRANSFORM_PROGRAM_START:
        nv_program_start = (int)value;
        return;
    case NV_TRANSFORM_CONSTANT_LOAD:
        constant_load = value;
        return;
    }
    if (method >= 0x0300 && method < 0x0400) {  /* render states: blend, alpha/depth test, cull, masks */
        gpu_render_state(method, value);
        return;
    }
    note_unknown(method, value);
}

void nv_run_pushbuffer(const unsigned *words, unsigned count)
{
    unsigned i = 0;
    while (i < count) {
        unsigned header = words[i++];
        if (header == 0)
            continue;  /* NOP */
        if ((header & 0xe0000003) == 0x20000000 || (header & 3) == 1 || (header & 3) == 2) {
            note_unknown(0xffff, header);  /* jumps and calls: not expected inside a model's push buffer */
            return;
        }
        unsigned method = header & 0x1ffc, n = (header >> 18) & 0x7ff, same = header & 0x40000000;
        for (unsigned k = 0; k < n && i < count; k++, i++)
            nv_method(same ? method : method + 4 * k, words[i]);
    }
}
