/* Stand-ins for the MSVC 7.1 C++ runtime helpers the game calls directly (Ghidra names, Xbox CRT originals in
 * lib/crt/). Each keeps the exact calling convention and argument count of the prototype in build/program.h, so the
 * decorated names match what the compiled game code links against.
 *
 * The exception-handling parts (unwinding a half-built array when a constructor throws) are left out: the game is
 * built without exception frames (see tools/compile_check.py), and nothing in it throws.
 */
#include <stddef.h>

void free(void *p);

typedef void(__thiscall *ctor_fn)(void *self);
typedef void(__thiscall *copy_ctor_fn)(void *self, void *source);
typedef void(__thiscall *dtor_fn)(void *self);

/* `eh vector constructor iterator'(array, element size, count, constructor, destructor) */
void __stdcall ___L_YGXPAXIHP6EX0_Z1_Z(void *array, unsigned size, int count, ctor_fn ctor, dtor_fn dtor)
{
    char *p = (char *)array;
    (void)dtor;
    for (int i = 0; i < count; i++, p += size)
        ctor(p);
}

/* `eh vector destructor iterator'(array, element size, count, destructor): last element first */
void __stdcall ___M_YGXPAXIHP6EX0_Z_Z(void *array, unsigned size, int count, dtor_fn dtor)
{
    char *p = (char *)array + (size_t)size * count;
    while (--count >= 0) {
        p -= size;
        dtor(p);
    }
}

/* `eh vector copy constructor iterator'(destination, source, element size, count, copy constructor, destructor) */
void __stdcall ____C_YGXPAX0IHP6EX00_ZP6EX0_Z_Z(void *dst, void *src, unsigned size, int count, copy_ctor_fn copy,
                                                dtor_fn dtor)
{
    char *d = (char *)dst, *s = (char *)src;
    (void)dtor;
    for (int i = 0; i < count; i++, d += size, s += size)
        copy(d, s);
}

/* exception::~exception(): { vftable, what, owns_what } -- frees the message it owns */
void __fastcall __1exception__UAE_XZ(void **self)
{
    if (self[2])
        free(self[1]);
}
