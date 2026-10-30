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
