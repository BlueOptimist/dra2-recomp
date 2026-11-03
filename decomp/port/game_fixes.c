/* Game functions rewritten by hand, where the decompiled C can't say what the machine code does. Each one carries
 * a "replaces <address>" marker: build_objects.py then turns the decompiled body into a weak stub, and this
 * definition takes over at link time. Prototypes match build/protos.h, so callers link to these unchanged. */

void __3CNoTrackObject__SAXPAX_Z(int p);  /* operator delete */

/* replaces 00156710 Class_2a4884::vf00: a one-argument callback holding a C++ pointer to member,
 * { vtable, target, method, this adjustment }. It calls target->*method(arg) and deletes itself:
 *     mov ecx, [esi+0Ch] ; mov edx, [esi+4] ; push eax ; add ecx, edx ; call [esi+8]
 * The decompiler shows only the call: the method ran with no "this", so the stream reader's completion
 * (FUN_00158ba0) wrote through null. */
typedef struct {
    void *vftable;
    char *target;
    void (__thiscall *method)(void *self, unsigned int arg);
    int adjust;
} MemberCallback;

void __thiscall Class_2a4884__vf00(MemberCallback *this, unsigned int param_1)
{
    this->method(this->target + this->adjust, param_1);
    __3CNoTrackObject__SAXPAX_Z((int)this);
}

/* replaces 00157220 Class_2a4888::vf00: the stream reader's read-done callback, the same pointer to member
 * (mov ecx, [esi+0Ch] ; add ecx, [esi+4] ; call [esi+8]). When the reader was told to restart (the byte at
 * 0x2fa684), it rebuilds the reader object instead of calling. */
extern unsigned char DAT_002fa684;
void __thiscall Class_2a4884__ctor(void *this);

void __thiscall Class_2a4888__vf00(MemberCallback *this, unsigned int param_1)
{
    if (DAT_002fa684) {
        DAT_002fa684 = 0;
        Class_2a4884__ctor(this->target);
    } else {
        this->method(this->target + this->adjust, param_1);
    }
    __3CNoTrackObject__SAXPAX_Z((int)this);
}

/* replaces 0013e8f0: std::sort of the resource-type registry, 0x10c-byte records { name[0x104], ..., create }
 * ordered by name (unsigned bytes, as strcmp). The introsort's helpers take a record by value, built on the stack
 * with "sub esp, 10Ch" and a string copy rather than pushed, and the decompiled callers don't pass it: the unguarded
 * insert (FUN_0013c460, variadic, reading &stack0x00000008) and make_heap's call to FUN_0013d210 dropped the record.
 * The sort copied garbage over 48 of the 51 types, so no scene node could be instantiated (every type lookup missed)
 * and nothing was ever drawn. */
typedef unsigned int size_t_;
void qsort(void *base, size_t_ count, size_t_ size, int (*compare)(const void *, const void *));
int strcmp(const char *a, const char *b);

static int compare_type_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

void FUN_0013e8f0(unsigned char *begin, unsigned char *end)
{
    if (begin != end)
        qsort(begin, (size_t_)(end - begin) / 0x10c, 0x10c, compare_type_names);
}

/* replaces 00130831: 4x4 matrix product out = a * b (row-major, Direct3D order), written with SSE in the original
 * (movaps/shufps/mulps: each row of a broadcast against the rows of b). out may not alias a or b. */
float *FUN_00130831(float *out, float *a, float *b)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    for (int i = 0; i < 16; i++)
        out[i] = r[i];
    return out;
}

/* replaces 00189400 Class_2a6648::vf03: a particle emitter attached to a parent, spawning this frame's particles
 * along the parent's motion. world = own * parent's (+0xb0 keeps last frame's, +0x124 the parent it belongs to);
 * this frame spawns floor(rate * dt + carry) particles (rate +0xf0, carry +0xf4), and particle i of n is placed at
 * the matrix interpolated i/n of the way from last frame's to this one, with a random position, velocity and life.
 * The decompiled body indexed the per-axis loops through stack pointers biased by -0x110 (lea [esp+x]; sub 110h),
 * which became separate arrays, and lost most of the particle stores, so it wrote through garbage. */
void *memcpy(void *dst, const void *src, size_t_ n);
float floorf(float x);
float __fastcall FUN_00164620(int clock_owner);
float __fastcall FUN_00164650(int clock_owner);
void *__thiscall FUN_001654f0(void *pool, int count, int *begin, int *end);
void __thiscall FUN_00186e00(void *particle, float *matrix);
int __fastcall FUN_00186ef0(int self);
int __fastcall FUN_00186f50(int self);
int __fastcall FUN_00186f60(int self);
float __thiscall FUN_001c8420(unsigned unused, float lo, float hi);  /* random float in [lo, hi] */
float *__thiscall FUN_001ca7b0(void *matrix, float *in, float *out);

void __thiscall Class_2a6648__vf03(unsigned char *this, void *pool, unsigned char *parent)
{
#define F(off) (*(float *)(this + (off)))
    float now[16], last[16], lerp[16];
    FUN_00130831(now, (float *)FUN_00186f50((int)this), (float *)FUN_00186f60((int)parent));
    unsigned char **owner = (unsigned char **)(this + 0x124);
    if (*owner == 0) {
        *owner = parent;
        memcpy(this + 0xb0, now, sizeof now);
        memcpy(last, now, sizeof now);
    } else if (*owner == parent) {
        memcpy(last, this + 0xb0, sizeof last);
        memcpy(this + 0xb0, now, sizeof now);
    } else {
        memcpy(last, now, sizeof now);
    }
    float dt = FUN_00164620((int)parent);
    float due = dt * F(0xf0) + F(0xf4);
    int spawn = (int)floorf(due);
    F(0xf4) = due - (float)spawn;
    int begin = 0, end = 0;
    FUN_001654f0(pool, spawn, &begin, &end);
    if (begin == end)
        return;
    int count = (end - begin) / 0x30;
    float *origin = (float *)FUN_00186ef0((int)this);
    float t_end = FUN_00164650((int)parent);
    float t_start = t_end - dt;
    *(float *)(parent + 0x100) = F(0x11c) > F(0x120) ? F(0x11c) : F(0x120);
    for (int i = 0; i < count; i++) {
        float *p = (float *)(begin + i * 0x30);
        float f = (float)(i + 1) / (float)count;
        float age = (t_end - t_start) * f + t_start;
        float pos[3], vel[3];
        for (int k = 0; k < 3; k++) {
            float x = F(0x110 + 4 * k);
            vel[k] = FUN_001c8420(0, -x, x) + F(0x104 + 4 * k);
            float y = F(0xf8 + 4 * k);
            pos[k] = FUN_001c8420(0, -y, y) + origin[k] - vel[k] * age;
        }
        float life = FUN_001c8420(0, F(0x11c), F(0x120));
        p[8] = -age;
        p[0] = pos[0], p[1] = pos[1], p[2] = pos[2];
        p[9] = life;
        p[4] = vel[0], p[5] = vel[1], p[6] = vel[2];
        for (int k = 0; k < 16; k++)
            lerp[k] = last[k] + (now[k] - last[k]) * f;
        FUN_00186e00(p, lerp);
    }
    if (F(0x128) != 0.0f) {
        float a[3], b[3];
        FUN_001ca7b0(last, origin, a);
        FUN_001ca7b0(now, origin, b);
        float s = F(0x128) / dt;
        for (int i = 0; i < count; i++) {
            float *v = (float *)(begin + i * 0x30 + 0x10);
            for (int k = 0; k < 3; k++)
                v[k] += (b[k] - a[k]) * s;
        }
    }
#undef F
}

/* replaces 000ccb60: places a transform (param_1, a matrix set to a translation by FUN_001ca060) just beside the
 * scene camera: at the camera's position + tan(fov * DAT_002f707c / 2) * -1/6 along x. The decompiled body passed the
 * return address as the matrix (unaff_retaddr) and its stack temporaries as the position; the attract mode's loading
 * screen wrote to address 3. */
extern void *DAT_002f7f50;
extern float _DAT_002f707c;
double tan(double x);
unsigned __fastcall FUN_00149f10(int self);
void __thiscall FUN_001ca060(void *matrix, unsigned *position);

void FUN_000ccb60(unsigned param_1)
{
    void **camera = (void **)FUN_00149f10(*(int *)((char *)DAT_002f7f50 + 0xb4));
    if (!camera)
        return;
    void **vtable = *(void ***)camera;
    float fov = ((float(__thiscall *)(void *))vtable[0x54 / 4])(camera);
    float offset = (float)tan(fov * _DAT_002f707c * 0.5f) * -0.16666667f;
    float tmp[3];
    float *at = ((float *(__thiscall *)(void *, float *))vtable[0x1c / 4])(camera, tmp);
    float position[3] = { at[0] + offset, at[1], at[2] };
    FUN_001ca060((void *)param_1, (unsigned *)position);
}

/* replaces 0014a8d0 Class_2a3c90::vf06: a sound voice's pan, clamped to [-1, 1] (fcmovb/fcmovnbe), stored at +0x34
 * with change flag 0x10. The decompiled body stored the float through the class's vtable pointer type, which C
 * can't cast, so it was a stub and the first sound played in 3D trapped. */
void __thiscall Class_2a3c90__vf06(unsigned char *this, float pan)
{
    if (pan < -1.0f)
        pan = -1.0f;
    if (pan > 1.0f)
        pan = 1.0f;
    *(float *)(this + 0x34) = pan;
    *(unsigned *)(this + 0x14) |= 0x10;
}

/* replaces 00173200 Class_2a64e8::vf01: after the base class's load (Class_2a3850::vf01), look up the 10 names
 * listed in the loaded data (+0x18): each a self-relative offset at data+0x1c+4i to a string, empty for none. Each
 * found object (FUN_00186ac0) is stored at +0x20+4i and attached to this one's +0x1c. The decompiled body read the
 * offsets through the vtable type and didn't compile, so the stub trapped when the attract fight loaded its arena. */
unsigned char __thiscall Class_2a3850__vf01(void *this, unsigned param_2);
unsigned *FUN_00186ac0(int name);
void __thiscall FUN_001868e0(void *this, int param_1);

unsigned char __thiscall Class_2a64e8__vf01(unsigned char *this, unsigned param_2)
{
    if (!Class_2a3850__vf01(this, param_2))
        return 0;
    int *offset = (int *)(*(unsigned char **)(this + 0x18) + 0x1c);
    void **found = (void **)(this + 0x20);
    for (int i = 0; i < 10; i++, offset++, found++) {
        char *name = (char *)offset + *offset;
        if (*name) {
            *found = FUN_00186ac0((int)name);
            FUN_001868e0(*found, (int)(this + 0x1c));
        }
    }
    return 1;
}

/* replaces 00172f20 Class_2a7270::vf05: a value that drifts to a random target. Its parameters (+0xc) are refreshed by
 * FUN_0013c070(this, index, value): [0] enabled, [1]/[2] the target's range, [3] the time to reach it, [4] whether to
 * interpolate, [5] the output. +0x24 counts time (the owner's clock at +4 -> +0xc, its +0x1c is the frame time); when
 * it reaches [3], the old target (+0x1c) becomes the start (+0x20) and a new one is drawn. Then the owner (+4) is told
 * its slot (+8) changed: bit set in the dirty mask at owner+0x38, owner+0x34 cleared, and +0x10 flagged. The decompiled
 * body read the fields through the vtable type and didn't compile, so the stub trapped when the attract fight built
 * its level. */
unsigned __thiscall FUN_0013c070(void *this, int index, float value);

void __thiscall Class_2a7270__vf05(unsigned char *this, unsigned param_2)
{
    (void)param_2;
#define P(i) ((*(float **)(this + 0xc))[i])
#define F(off) (*(float *)(this + (off)))
    FUN_0013c070(this, 0, 0.0f);
    if (P(0) == 0.0f)
        return;
    unsigned char *owner = *(unsigned char **)(this + 4);
    F(0x24) += *(float *)(*(unsigned char **)(owner + 0xc) + 0x1c);
    FUN_0013c070(this, 3, 0.0f);
    float duration = P(3);
    if (!(F(0x24) < duration)) {
        F(0x20) = F(0x1c);
        FUN_0013c070(this, 2, 0.0f);
        float hi = P(2);
        FUN_0013c070(this, 1, 0.0f);
        F(0x1c) = FUN_001c8420(0, P(1), hi);
        F(0x24) = 0.0f;
    }
    float out;
    if (duration > 0.0f && (FUN_0013c070(this, 4, 0.0f), P(4) != 0.0f))
        out = (F(0x1c) - F(0x20)) / duration * F(0x24) + F(0x20);
    else
        out = F(0x1c);
    FUN_0013c070(this, 5, out);
    P(5) = out;
    *(unsigned *)(this + 0x10) |= 0x80000000u;
    int slot = *(int *)(this + 8);
    owner = *(unsigned char **)(this + 4);
    (*(unsigned **)(owner + 0x38))[slot >> 5] |= 1u << (slot & 0x1f);
    owner[0x34] = 0;
#undef P
#undef F
}

/* The end of GameMode::ctor (000a7530, called from its source patch): the arena's bounds (+0x494, min and max
 * corners, reset to +/-FLT_MAX) grow to the "upperleft" and "lowerright" markers of the level (their world matrix
 * from virtual slot 0x28, translation at +0x70), x widened to -666 and 666; without both markers, to (30, -500, 500)
 * and (-30, 500, -500). The decompiled code kept the second marker in a stack slot it reused, and called it at -1. */
void __thiscall FUN_001d2fc0(void *bounds, float *point);

void GameMode_marker_bounds(void *bounds, void *upper_left, void *lower_right)
{
    if (!upper_left || !lower_right) {
        float a[3] = { 30.0f, -500.0f, 500.0f }, b[3] = { -30.0f, 500.0f, -500.0f };
        FUN_001d2fc0(bounds, a);
        FUN_001d2fc0(bounds, b);
        return;
    }
    void *marker[2] = { upper_left, lower_right };
    for (int i = 0; i < 2; i++) {
        unsigned char *(__thiscall *matrix)(void *) = (*(unsigned char *(__thiscall ***)(void *))marker[i])[0x28 / 4];
        float *at = (float *)(matrix(marker[i]) + 0x70);
        float point[3] = { i ? 666.0f : -666.0f, at[1], at[2] };
        FUN_001d2fc0(bounds, point);
    }
}

/* replaces 00050c60: binds a list of named slots to a scene (the camera controller's targets). +4 = source->vf28()
 * (the scene), then for each of this->vf0c() entries: *this->vf00(i) = FUN_0013cc10(scene, this->vf08(i)), the slot
 * filled with the scene node of that name. The decompiled loop dropped the index argument of vf00 and passed the
 * previous name instead, so the attract fight's camera wrote its first node through a small integer.
 * Its vf08 (FUN_00050f70) is a __thiscall that ignores "this", recompiled as cdecl: it leaves its argument on the
 * stack, so like the decompiled code this keeps a variable-length local, which makes the epilogue restore ESP. */
int __thiscall FUN_0013cc10(void *scene, unsigned char *name);
extern int ghidra_dynamic_one;

void __thiscall FUN_00050c60(void **this, void **source)
{
    char keep_esp[ghidra_dynamic_one];
    (void)keep_esp;
    if (!source) {
        this[1] = 0;
        return;
    }
    typedef void *(__thiscall *Method)(void *self, int i);
    this[1] = ((void *(__thiscall *)(void *))(*(void ***)source)[0x28 / 4])(source);
    void **vtable = *(void ***)this;
    int count = ((int(__thiscall *)(void *))vtable[0xc / 4])(this);
    for (int i = 0; i < count; i++) {
        int *slot = ((Method)vtable[0])(this, i);
        unsigned char *name = ((Method)vtable[2])(this, i);
        *slot = FUN_0013cc10(this[1], name);
    }
}

/* replaces 0007e520: a model's anchor point, from its interface at +0x128 (this - 0x128 is the model). vf09 (slot
 * 0x24) gives a local point (filling a scratch vector), vf08 (slot 0x20) the matrix to place it with, and
 * FUN_001cbbc0 transforms it; out = { point, 0, 0, 0 }. The original reuses the arguments it pushed for the first two
 * calls (each takes one and pops one) as FUN_001cbbc0's, which the decompiler couldn't follow: it passed ESI. */
void __thiscall FUN_001cbbc0(void *matrix, float *in, float *out);

void __thiscall FUN_0007e520(unsigned char *this, float *out)
{
    void *model = this - 0x128;
    void **vtable = *(void ***)model;
    float point[3], scratch[4], matrix_scratch[4];
    float *local = ((float *(__thiscall *)(void *, float *))vtable[0x24 / 4])(model, scratch);
    void *matrix = ((void *(__thiscall *)(void *, float *))vtable[0x20 / 4])(model, matrix_scratch);
    FUN_001cbbc0(matrix, local, point);
    out[0] = point[0], out[1] = point[1], out[2] = point[2];
    out[3] = out[4] = out[5] = 0.0f;
}

/* replaces 000f3490: a character model's anchor (FUN_0007e520 above) plus its definition's +0x784 at +0x18, this
 * +0x12c at +0x1c, 1.0 at +0x20 and the byte at +0x4cc as a float at +0x24. The decompiled call passed EDI and ESI
 * where the output went. */
void __thiscall FUN_000f3490(unsigned char *this, unsigned char *out)
{
    FUN_0007e520(this, (float *)out);
    *(unsigned *)(out + 0x18) = *(unsigned *)(*(unsigned char **)(this + 8) + 0x784);
    *(unsigned *)(out + 0x1c) = *(unsigned *)(this + 0x12c);
    *(float *)(out + 0x20) = 1.0f;
    *(float *)(out + 0x24) = (float)this[0x4cc];
}

/* replaces 00050bf0: one value of a marker's bound parameters: the slot index for this axis (vf00(axis)), -1 for
 * none; the parameter is refreshed (FUN_0013c070) and read from the owner's value array (+0xc). The decompiled body
 * passed the axis as the slot and read the output pointer from the return address. */
unsigned __thiscall FUN_0013c070(void *this, int index, float value);

unsigned __thiscall FUN_00050bf0(void *marker, float *range, int axis)
{
    void **this = marker;
    unsigned *out = (unsigned *)range;
    unsigned char *owner = this[1];
    if (!owner)
        return 0;
    int index = *((int *(__thiscall *)(void *, int))(*(void ***)this)[0])(this, axis);
    if (index == -1)
        return 0;
    FUN_0013c070(owner, index, 0.0f);
    *out = (*(unsigned **)(owner + 0xc))[index];
    return 1;
}

/* replaces 0004fb60: the attract camera's aim (+0x1b4 x, +0x1b8 y). The camera rig (FUN_000519c0) has two markers,
 * near (+0x38) and far (+0x60): their positions (FUN_00050c40) and the screen ranges each allows on x and y
 * (FUN_00050bf0, axis 0/1). Over the tracked objects (the list at DAT_002f445c), t = how far their average height
 * (+0x7c and +0x84) lies between the markers' heights, u = how far the lowest one's depth (+0x88) lies between their
 * depths, both clamped to [0, 1]; aim = near + (far - near) * sin(t * pi / 2), and the same with u on y. Each call
 * reuses arguments pushed for the one before, which the decompiler lost: it passed one or two of them, and the
 * challenger screen read through -1. */
void *thunk_FUN_000519c0(void);
void __thiscall FUN_00050c40(void *marker, float *position);
unsigned __thiscall FUN_00050bf0(void *marker, float *range, int axis);
extern int *DAT_002f445c;
float sinf(float x);

void __thiscall FUN_0004fb60(unsigned char *this)
{
    *(float *)(this + 0x1b4) = 0.0f;
    *(float *)(this + 0x1b8) = 0.0f;
    unsigned char *rig = thunk_FUN_000519c0();
    if (!*(int *)(rig + 0x3c) || !*(int *)(rig + 0x64))
        return;
    void *near_ = rig + 0x38, *far_ = rig + 0x60;
    float a[3], b[3], lo[2] = { 0, 0 }, hi[2] = { 0, 0 };
    FUN_00050c40(near_, a);
    FUN_00050c40(far_, b);
    FUN_00050bf0(near_, &lo[0], 0);
    FUN_00050bf0(far_, &hi[0], 0);
    FUN_00050bf0(near_, &lo[1], 1);
    FUN_00050bf0(far_, &hi[1], 1);
    float sum = 0.0f, lowest = a[2];
    unsigned count = 0;
    for (int *node = (int *)*DAT_002f445c; node != DAT_002f445c; node = (int *)*node, count++) {
        unsigned char *obj = (unsigned char *)node[2];
        sum += *(float *)(obj + 0x7c) + *(float *)(obj + 0x84);
        if (*(float *)(obj + 0x88) < lowest)
            lowest = *(float *)(obj + 0x88);
    }
    float t = (sum / (2.0f * (float)count) - a[1]) / (b[1] - a[1]);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float u = (lowest - a[2]) / (b[2] - a[2]);
    u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
    *(float *)(this + 0x1b4) = (hi[0] - lo[0]) * sinf(3.1415927f * t * 0.5f) + lo[0];
    *(float *)(this + 0x1b8) = (hi[1] - lo[1]) * sinf(3.1415927f * u * 0.5f) + lo[1];
}

/* replaces 0008f4e0: adds the four edge midpoints of a rectangle { x0, y0, x1, y1 } to this (FUN_0008f3c0, a point
 * { 0, x, y } and weight 0): (mid x, y0), (mid x, y1), (x1, mid y), (x0, mid y). ECX passes through untouched to all
 * four calls, and each point is built in the same stack slots; the decompiler showed two calls without arguments. */
void __thiscall FUN_0008f3c0(void *this, float *point, float weight);

void __thiscall FUN_0008f4e0(void *this, float *rect)
{
    float mid_x = (rect[2] - rect[0]) * 0.5f + rect[0], mid_y = (rect[1] - rect[3]) * 0.5f + rect[3];
    float p[3] = { 0.0f, mid_x, rect[1] };
    FUN_0008f3c0(this, p, 0.0f);
    p[2] = rect[3];
    FUN_0008f3c0(this, p, 0.0f);
    p[1] = rect[2], p[2] = mid_y;
    FUN_0008f3c0(this, p, 0.0f);
    p[1] = rect[0];
    FUN_0008f3c0(this, p, 0.0f);
}

/* replaces 0004f7a0: for each tracked object (the list at DAT_002f445c), its screen rectangle (+0x7c..+0x88) goes to
 * FUN_0008f4e0 on this+0xe0. A method (lea edi,[ecx+0E0h]) that Ghidra typed without this. */
void __thiscall FUN_0004f7a0(unsigned char *this)
{
    for (int *node = (int *)*DAT_002f445c; node != DAT_002f445c; node = (int *)*node) {
        float rect[4];
        memcpy(rect, (unsigned char *)node[2] + 0x7c, sizeof rect);
        FUN_0008f4e0(this + 0xe0, rect);
    }
}

/* replaces 0002e760, replaces 0002e7b0: an object's lower and upper bound on the AI's 2D plane: its position's y and z
 * (vf07, slot 0x1c, into a scratch vector) minus or plus its radius (vf18, slot 0x60) on z, and 0 * radius on y.
 * Both take the output as their one stack argument; the decompiled bodies wrote it through the return address. */
static float *ai_extent(void **this, float *out, float sign)
{
    void **object = *(void ***)this;
    void **vtable = *(void ***)object;
    float radius = ((float(__thiscall *)(void *))vtable[0x60 / 4])(object);
    float scratch[4];
    float *at = ((float *(__thiscall *)(void *, float *))vtable[0x1c / 4])(object, scratch);
    out[0] = at[1] + sign * (0.0f * radius);
    out[1] = at[2] + sign * radius;
    return out;
}

float *__thiscall FUN_0002e760(void **this, float *out) { return ai_extent(this, out, -1.0f); }
void __thiscall FUN_0002e7b0(void **this, float *out) { ai_extent(this, out, 1.0f); }

/* replaces 001cccf0: keeps the scheduler's task list (circular, doubly linked: next at +4, prev at +8, the head
 * itself the sentinel) sorted by priority (+0x14). When it's out of order: unlink the ring, merge sort it
 * (FUN_001ccc30, singly linked through +4), store the result as head->next and rebuild the prev links. The decompiled
 * body stored an uninitialised value as head->next before the sort, instead of the sort's result after it. */
int FUN_001ccc30(int list, unsigned count);

void __fastcall FUN_001cccf0(int head)
{
    unsigned count = 0, last = 0;
    int sorted = 1;
    for (int node = *(int *)(head + 4); node != head; node = *(int *)(node + 4)) {
        unsigned priority = *(unsigned *)(node + 0x14);
        if (priority < last)
            sorted = 0;
        last = priority;
        count++;
    }
    if (sorted || count < 2)
        return;
    *(int *)(*(int *)(head + 8) + 4) = 0;
    int node = FUN_001ccc30(*(int *)(head + 4), count), prev = head;
    *(int *)(head + 4) = node;
    for (; node; node = *(int *)(node + 4)) {
        *(int *)(node + 8) = prev;
        prev = node;
    }
    *(int *)(prev + 4) = head;
    *(int *)(head + 8) = prev;
}
