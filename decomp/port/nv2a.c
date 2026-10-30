/* The NV2A (the Xbox GPU) on the CPU: the parts of its command interface the game's push buffers and Direct3D calls
 * reach, turned into calls on port/gpu.c.
 *
 *  - A push buffer is a stream of method headers and data: (count << 18) | (subchannel << 13) | method, bit 30 set
 *    when every word goes to the same method (non-increasing); 0x20000000 | address is an old-style jump. The game's
 *    models are pre-built push buffers in the resource files (Direct3D's RunPushBuffer), and Direct3D's own inline
 *    calls (SetRenderState_Simple) hand over single method/value pairs.
 *  - Vertex programs are NV2A microcode (136 slots of 4 dwords) run by an interpreter per vertex. Programs compiled by
 *    the XDK end by scaling the position to the viewport (constants -38/-37), so the outputs are screen coordinates
 *    and Direct3D 9 draws them as pretransformed vertices.
 *
 * Field layouts and opcode tables follow the NV2A documentation in the open-source emulators (xemu, Cxbx-Reloaded).
 */
#include "nv2a.h"

int printf(const char *format, ...);
float sqrtf(float x);
float floorf(float x);
float exp2f(float x);
float log2f(float x);
float powf(float x, float y);
float fabsf(float x);

float nv_constants[NV_CONSTANTS][4];  /* index = register + 96 */
unsigned nv_program[NV_PROGRAM_SLOTS][4];
int nv_program_start;

/* --------------------------------------------------------------------------------------- vertex programs -- */
/* (dword, first bit, bit count) of each instruction field */
#define FLD(t, dw, pos, len) (((t)[dw] >> (pos)) & ((1u << (len)) - 1))
#define ILU(t) FLD(t, 1, 25, 3)
#define MAC(t) FLD(t, 1, 21, 4)
#define CONST_INDEX(t) FLD(t, 1, 13, 8)
#define V_INDEX(t) FLD(t, 1, 9, 4)
#define OUT_MAC_MASK(t) FLD(t, 3, 24, 4)
#define OUT_R(t) FLD(t, 3, 20, 4)
#define OUT_ILU_MASK(t) FLD(t, 3, 16, 4)
#define OUT_O_MASK(t) FLD(t, 3, 12, 4)
#define OUT_ORB(t) FLD(t, 3, 11, 1)      /* 0: a constant register, 1: an output register */
#define OUT_ADDRESS(t) FLD(t, 3, 3, 8)
#define OUT_MUX(t) FLD(t, 3, 2, 1)       /* 0: the MAC result, 1: the ILU result */
#define A0X(t) FLD(t, 3, 1, 1)
#define FINAL(t) FLD(t, 3, 0, 1)

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT,
       MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

typedef struct {
    NvVec4 r[13];  /* r12 reads back the position output */
    NvVec4 *in, *out;
    int a0;
} VshState;

/* operand 0 = A, 1 = B, 2 = C */
static NvVec4 operand(VshState *s, const unsigned *t, int which)
{
    unsigned neg, swz, reg, mux;
    if (which == 0) {
        neg = FLD(t, 1, 8, 1), swz = FLD(t, 1, 0, 8), reg = FLD(t, 2, 28, 4), mux = FLD(t, 2, 26, 2);
    } else if (which == 1) {
        neg = FLD(t, 2, 25, 1), swz = FLD(t, 2, 17, 8), reg = FLD(t, 2, 13, 4), mux = FLD(t, 2, 11, 2);
    } else {
        neg = FLD(t, 2, 10, 1), swz = FLD(t, 2, 2, 8), reg = (FLD(t, 2, 0, 2) << 2) | FLD(t, 3, 30, 2);
        mux = FLD(t, 3, 28, 2);
    }
    const float *src;
    if (mux == 1)
        src = reg == 12 ? s->out[NV_OUT_POS].v : s->r[reg < 12 ? reg : 0].v;
    else if (mux == 2)
        src = s->in[V_INDEX(t)].v;
    else {
        int c = (int)CONST_INDEX(t) + (A0X(t) ? s->a0 : 0);
        src = nv_constants[c < 0 ? 0 : c >= NV_CONSTANTS ? NV_CONSTANTS - 1 : c];
    }
    /* swizzle: x in bits 6-7, y 4-5, z 2-3, w 0-1 */
    NvVec4 v = { { src[(swz >> 6) & 3], src[(swz >> 4) & 3], src[(swz >> 2) & 3], src[swz & 3] } };
    if (neg)
        for (int i = 0; i < 4; i++)
            v.v[i] = -v.v[i];
    return v;
}

/* write mask: bit 3 = x, 2 = y, 1 = z, 0 = w */
static void store(float *dst, const NvVec4 *v, unsigned mask)
{
    for (int i = 0; i < 4; i++)
        if (mask & (8 >> i))
            dst[i] = v->v[i];
}

static NvVec4 splat(float f)
{
    NvVec4 v = { { f, f, f, f } };
    return v;
}

void nv_vsh_run(int start, NvVec4 in[16], NvVec4 out[NV_OUTPUTS])
{
    VshState s;
    for (int i = 0; i < 13; i++)
        s.r[i] = splat(0.0f);
    s.in = in, s.out = out, s.a0 = 0;
    for (int pc = start; pc < NV_PROGRAM_SLOTS; pc++) {
        const unsigned *t = nv_program[pc];
        unsigned mac = MAC(t), ilu = ILU(t);
        NvVec4 m = splat(0.0f), l = splat(0.0f);
        if (mac != MAC_NOP) {
            NvVec4 a = operand(&s, t, 0), b = operand(&s, t, 1), c = operand(&s, t, 2);
            switch (mac) {
            case MAC_MOV: m = a; break;
            case MAC_MUL: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] * b.v[i]; break;
            case MAC_ADD: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] + c.v[i]; break;
            case MAC_MAD: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
            case MAC_DP3: m = splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]); break;
            case MAC_DPH: m = splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + b.v[3]); break;
            case MAC_DP4: m = splat(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3]); break;
            case MAC_DST: m.v[0] = 1.0f, m.v[1] = a.v[1] * b.v[1], m.v[2] = a.v[2], m.v[3] = b.v[3]; break;
            case MAC_MIN: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
            case MAC_MAX: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; break;
            case MAC_SLT: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
            case MAC_SGE: for (int i = 0; i < 4; i++) m.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
            case MAC_ARL: s.a0 = (int)floorf(a.v[0]); break;
            }
        }
        if (ilu != ILU_NOP) {
            float x = operand(&s, t, 2).v[0];
            NvVec4 c = operand(&s, t, 2);
            switch (ilu) {
            case ILU_MOV: l = c; break;
            case ILU_RCP: l = splat(x == 0.0f ? 3.4e38f : 1.0f / x); break;
            case ILU_RCC: {
                float r = x == 0.0f ? 1.884467e19f : 1.0f / x;
                float ar = fabsf(r);
                if (ar < 5.42101e-20f) r = r < 0 ? -5.42101e-20f : 5.42101e-20f;
                else if (ar > 1.884467e19f) r = r < 0 ? -1.884467e19f : 1.884467e19f;
                l = splat(r);
                break;
            }
            case ILU_RSQ: l = splat(x == 0.0f ? 3.4e38f : 1.0f / sqrtf(fabsf(x))); break;
            case ILU_EXP: l.v[0] = exp2f(floorf(x)), l.v[1] = x - floorf(x), l.v[2] = exp2f(x), l.v[3] = 1.0f; break;
            case ILU_LOG: {
                float ax = fabsf(x), e = ax == 0.0f ? -3.4e38f : floorf(log2f(ax));
                l.v[0] = e, l.v[1] = ax == 0.0f ? 1.0f : ax / exp2f(e), l.v[2] = ax == 0.0f ? -3.4e38f : log2f(ax);
                l.v[3] = 1.0f;
                break;
            }
            case ILU_LIT: {
                float dx = c.v[0] > 0 ? c.v[0] : 0, dy = c.v[1] > 0 ? c.v[1] : 0, p = c.v[3];
                if (p < -127.9961f) p = -127.9961f;
                if (p > 127.9961f) p = 127.9961f;
                l.v[0] = 1.0f, l.v[1] = dx, l.v[2] = dx > 0 ? powf(dy, p) : 0.0f, l.v[3] = 1.0f;
                break;
            }
            }
        }
        /* temporaries: with both units writing, the ILU result goes to r1 */
        unsigned r = OUT_R(t);
        if (mac != MAC_NOP && mac != MAC_ARL && OUT_MAC_MASK(t))
            store(r == 12 ? out[NV_OUT_POS].v : s.r[r < 12 ? r : 0].v, &m, OUT_MAC_MASK(t));
        if (ilu != ILU_NOP && OUT_ILU_MASK(t)) {
            unsigned ri = mac != MAC_NOP ? 1 : r;
            store(ri == 12 ? out[NV_OUT_POS].v : s.r[ri < 12 ? ri : 0].v, &l, OUT_ILU_MASK(t));
        }
        if (OUT_O_MASK(t)) {
            const NvVec4 *v = OUT_MUX(t) ? &l : &m;
            unsigned addr = OUT_ADDRESS(t);
            if (OUT_ORB(t))
                store(out[addr & 0xF].v, v, OUT_O_MASK(t));
            else if (addr < NV_CONSTANTS)
                store(nv_constants[addr], v, OUT_O_MASK(t));
        }
        if (FINAL(t))
            break;
    }
}

/* one line per instruction, for the log */
void nv_vsh_dump(int start, int max)
{
    static const char *macs[] = { "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max",
                                  "slt", "sge", "arl", "?", "?" };
    static const char *ilus[] = { "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
    for (int pc = start; pc < NV_PROGRAM_SLOTS && pc < start + max; pc++) {
        const unsigned *t = nv_program[pc];
        printf("[vsh] %3d %08x %08x %08x  %s/%s c%d v%d r%d macmask %x ilumask %x omask %x %s%u mux %u%s\n", pc,
               t[1], t[2], t[3], macs[MAC(t)], ilus[ILU(t)], (int)CONST_INDEX(t) - 96, (int)V_INDEX(t),
               (int)OUT_R(t), OUT_MAC_MASK(t), OUT_ILU_MASK(t), OUT_O_MASK(t), OUT_ORB(t) ? "o" : "c",
               OUT_ADDRESS(t), OUT_MUX(t), FINAL(t) ? " final" : "");
        if (FINAL(t))
            break;
    }
}
