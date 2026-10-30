/* Types and helper macros used by Ghidra's raw decompiler output, so src/*.c can be fed to a 32-bit C compiler
 * while functions are cleaned up one by one. Target: x86 (Pentium III), MSVC 7.1 / XDK 5788 conventions. */
#ifndef GHIDRA_TYPES_H
#define GHIDRA_TYPES_H

typedef unsigned char undefined;
typedef unsigned char undefined1;
typedef unsigned short undefined2;
typedef unsigned int undefined3;
/* Every decompiled function declares "char ghidra_keep_esp[ghidra_dynamic_one];" (compile_check.py). A
 * variable-length local makes clang restore ESP from EBP on return instead of adding a constant. The game's
 * callees don't always pop the argument bytes their decompiled prototypes promise (a virtual method that ends in
 * "ret 4" called through a pointer typed with no arguments), and with a constant the caller then popped the wrong
 * saved EBP and returned into garbage. ghidra_dynamic_one is 1 (port/platform.c). */
extern int ghidra_dynamic_one;
/* Stores of 3-, 5-, 6- and 7-byte values: the C types above are wider, so compile_check.py rewrites
 * "*(undefined3 *)p = v;" into ghidra_store(p, v, 3), which writes exactly that many bytes (little-endian).
 * A 4-byte store of a 3-byte field overwrote the next byte: the saved "this" right after a frame. */
static inline void ghidra_store(void *p, unsigned long long v, int n)
{
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < n; i++, v >>= 8)
        b[i] = (unsigned char)v;
}
/* "x = 0x3f800000;": the decompiler writes float constants as decimals, so a hex constant stored into a float is the
 * float's bits (1.0f), not the integer 1065353216. compile_check.py routes such stores through ghidra_set_bits, which
 * picks by the target's type (_Generic doesn't evaluate it): floats get the bits, everything else the integer. */
static inline float ghidra_bits_float(unsigned b)
{
    union { unsigned u; float f; } x = { b };
    return x.f;
}
/* and the other way, for a float stored where the decompiler keeps raw 32 bits (tools/implicit_bits.py) */
static inline unsigned ghidra_float_bits(float f)
{
    union { float f; unsigned u; } x = { f };
    return x.u;
}
#define ghidra_set_bits(lvalue, bits) \
    ((lvalue) = _Generic((lvalue), float: ghidra_bits_float(bits), double: (double)ghidra_bits_float(bits), \
                         default: (bits)))
typedef unsigned int undefined4;
typedef unsigned long long undefined6;
typedef unsigned long long undefined8;

typedef int int3;          /* Ghidra's 3-byte integers, widened */
typedef unsigned int uint3;
typedef unsigned char byte;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong; /* matches Ghidra's typedef in build/program.h (32-bit on this target) */
typedef long long longlong;
typedef unsigned long long ulonglong;
typedef unsigned char bool8;
typedef unsigned short wchar16;
typedef float float10; /* x87 80-bit temporaries are approximated as float */
typedef struct { unsigned char b[10]; } unkbyte10; /* raw 10-byte x87 value */
typedef struct { unsigned char b[10]; } unkuint10;

typedef unsigned char bool; /* matches Ghidra's own typedef; Ghidra emits bool / true / false */
#define true 1
#define false 0

/* Ghidra's "code": a function of unknown signature. Parameters are unspecified, and the result is a register-sized
 * value (EAX): the decompiled code assigns it to ints, chars and pointers. */
typedef int code();

/* the decompiler's type for a stack pointer it could not track */
typedef unsigned char BADSPACEBASE;

/* the decompiler writes "is NaN" as NAN(x); a bare NaN constant is rewritten by tools/compile_check.py */
#ifndef NAN
#define NAN(x) __builtin_isnan(x)
#endif
typedef void *pointer;
typedef unsigned int dword;
typedef unsigned short word;

/* Ghidra's concatenation / extraction helpers */
#define CONCAT11(a, b) ((unsigned short)(((unsigned short)(unsigned char)(a) << 8) | (unsigned char)(b)))
#define CONCAT12(a, b) ((unsigned int)(((unsigned int)(unsigned char)(a) << 16) | (unsigned short)(b)))
#define CONCAT13(a, b) ((unsigned int)(((unsigned int)(unsigned char)(a) << 24) | ((unsigned int)(b) & 0xFFFFFF)))
/* wider than 32 bits: a byte or a short above a dword */
#define CONCAT14(a, b) ((unsigned long long)(((unsigned long long)(unsigned char)(a) << 32) | (unsigned int)(b)))
#define CONCAT24(a, b) ((unsigned long long)(((unsigned long long)(unsigned short)(a) << 32) | (unsigned int)(b)))
#define CONCAT22(a, b) ((unsigned int)(((unsigned int)(unsigned short)(a) << 16) | (unsigned short)(b)))
#define CONCAT31(a, b) ((unsigned int)(((unsigned int)(a) << 8) | (unsigned char)(b)))
#define CONCAT44(a, b) ((unsigned long long)(((unsigned long long)(unsigned int)(a) << 32) | (unsigned int)(b)))
#define SUB41(x, n) ((unsigned char)((unsigned int)(x) >> ((n) * 8)))
#define SUB42(x, n) ((unsigned short)((unsigned int)(x) >> ((n) * 8)))
#define SUB81(x, n) ((unsigned char)((unsigned long long)(x) >> ((n) * 8)))
#define SUB84(x, n) ((unsigned int)((unsigned long long)(x) >> ((n) * 8)))
#define ZEXT14(x) ((unsigned int)(unsigned char)(x))
#define ZEXT24(x) ((unsigned int)(unsigned short)(x))
#define ZEXT48(x) ((unsigned long long)(unsigned int)(x))
#define SEXT14(x) ((int)(signed char)(x))
#define SEXT24(x) ((int)(short)(x))
#define SEXT48(x) ((long long)(int)(x))
#define CARRY4(a, b) ((unsigned int)(a) + (unsigned int)(b) < (unsigned int)(a))
#define SCARRY4(a, b) ((((int)(a) + (int)(b)) < (int)(a)) != ((int)(b) < 0))
#define SBORROW4(a, b) ((((int)(a) - (int)(b)) > (int)(a)) != ((int)(b) < 0))
#define POPCOUNT(x) __builtin_popcount(x)
#define CONCAT21(a, b) ((unsigned int)(((unsigned int)(unsigned short)(a) << 8) | (unsigned char)(b)))

/* C runtime functions under the names Ghidra's Function ID gave them (MSVC 7.1 LIBCMT internals), mapped to the
   names the Windows C runtime exports. The Xbox CRT copies themselves are in lib/ and aren't compiled. */
#define _qsort qsort
#define _memmove memmove
#define _sprintf sprintf
#define _strcspn strcspn
#define _strncmp strncmp
#define _strncpy strncpy
#define _strrchr strrchr
#define _rand rand
#define _isdigit isdigit
#define _atol atol
#define _atexit atexit
#define __stricmp _stricmp
#define __strnicmp _strnicmp
#define __vsnprintf _vsnprintf
#define __copysign _copysign
#define __controlfp _controlfp
#define __ceil_default ceil
#define __floor_default floor
#define FID_conflict__fread fread
/* the second of the two "_fread or _fwrite" matches (overloads now carry their address); the game reads into the
   buffer it passes, so it is fread */
#define FID_conflict__fread_001e05a1 fread
#define _malloc malloc

/* Ghidra's floating-point pseudo-operations (x87 instructions it has no C operator for) */
#define ABS(x) __builtin_fabs((double)(x))
#define SQRT(x) __builtin_sqrt((double)(x))
#define ROUND(x) ((long long)__builtin_rint((double)(x)))   /* FRNDINT/FIST: current rounding mode, nearest */
#define TRUNC(x) ((long long)(x))
#define f2xm1(x) (__builtin_exp2((double)(x)) - 1.0)        /* 2^x - 1 */
#define fsin(x) __builtin_sin((double)(x))
#define fcos(x) __builtin_cos((double)(x))
#define fptan(x) __builtin_tan((double)(x))
#define fpatan(y, x) __builtin_atan2((double)(y), (double)(x)) /* atan(ST1 / ST0) with the quadrant */
#define fscale(x, e) __builtin_ldexp((double)(x), (int)(e))   /* x * 2^trunc(e) */

/* Ghidra's "bad instruction / software interrupt" markers: trap like the original INT 3 would */
static int ghidra_trap() { __builtin_trap(); return 0; }
#define halt_baddata() ((void)ghidra_trap())
#define swi(n) ((code *)ghidra_trap)

/* Engine resource structs (also imported into the Ghidra project, so they appear in src/). The generated
 * build/program.h already contains every program type, so it defines DRA2_PROGRAM_H to skip this. */
#ifndef DRA2_PROGRAM_H
#include "dra2_formats.h"
#endif

#endif
