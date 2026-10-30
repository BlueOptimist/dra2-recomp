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
