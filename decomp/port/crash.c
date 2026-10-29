/* Crash reporter: on an access violation, illegal instruction or other fatal exception, print the exception, the
 * registers and the call chain (by walking the EBP frame chain; the objects are built at -O0, so every function
 * keeps a frame pointer). tools/symbolize.py turns the addresses into function names.
 * Installed by port_install_crash_reporter() at the start of main (startup.c). */
#include "win32_min.h"

int printf(const char *format, ...);

typedef struct {
    DWORD ExceptionCode, ExceptionFlags;
    void *ExceptionRecord, *ExceptionAddress;
    DWORD NumberParameters;
    DWORD ExceptionInformation[15];
} EXCEPTION_RECORD_32;

/* the parts of the 32-bit CONTEXT used here, at their documented offsets */
#define CTX_U32(ctx, offset) (*(DWORD *)((unsigned char *)(ctx) + (offset)))
#define CTX_EDI 0x9C
#define CTX_ESI 0xA0
#define CTX_EBX 0xA4
#define CTX_EDX 0xA8
#define CTX_ECX 0xAC
#define CTX_EAX 0xB0
#define CTX_EBP 0xB4
#define CTX_EIP 0xB8
#define CTX_ESP 0xC4

typedef struct { EXCEPTION_RECORD_32 *record; void *context; } EXCEPTION_POINTERS_32;

WINIMPORT void *WINAPI AddVectoredExceptionHandler(DWORD first, long(WINAPI *handler)(EXCEPTION_POINTERS_32 *));
WINIMPORT BOOL WINAPI IsBadReadPtr(const void *p, SIZE_T size);

static long WINAPI on_exception(EXCEPTION_POINTERS_32 *info)
{
    DWORD code = info->record->ExceptionCode;
    if (code < 0xC0000000u || code == 0xC0000409u /* abort(): already reported */)
        return 0; /* EXCEPTION_CONTINUE_SEARCH */
    static int reported;
    if (reported++)
        return 0;
    void *c = info->context;
    extern volatile int port_init_index, port_in_game_main;
    extern volatile unsigned port_init_slot;
    if (port_init_index >= 0)
        printf("[crash] during static initializer %d (table slot %#x)\n", port_init_index, port_init_slot);
    else if (port_in_game_main)
        printf("[crash] during game_main\n");
    printf("[crash] exception %08lx at %08lx", code, (unsigned long)CTX_U32(c, CTX_EIP));
    if (code == 0xC0000005u && info->record->NumberParameters >= 2)
        printf(" (%s %08lx)", info->record->ExceptionInformation[0] ? "writing" : "reading",
               info->record->ExceptionInformation[1]);
    printf("\n[crash] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
           CTX_U32(c, CTX_EAX), CTX_U32(c, CTX_EBX), CTX_U32(c, CTX_ECX), CTX_U32(c, CTX_EDX),
           CTX_U32(c, CTX_ESI), CTX_U32(c, CTX_EDI), CTX_U32(c, CTX_EBP), CTX_U32(c, CTX_ESP));
    /* the top of the stack: after a call through a null pointer, [esp] is the return address into the caller */
    DWORD *esp = (DWORD *)CTX_U32(c, CTX_ESP);
    for (int k = 0; k < 12 && !IsBadReadPtr(esp + k, 4); k++)
        printf("[crash] stack %08lx\n", (unsigned long)esp[k]);
    printf("[crash] frame %08lx\n", (unsigned long)CTX_U32(c, CTX_EIP));
    DWORD *ebp = (DWORD *)CTX_U32(c, CTX_EBP);
    for (int depth = 0; depth < 32 && ebp && !IsBadReadPtr(ebp, 8); depth++) {
        DWORD ret = ebp[1];
        if (!ret)
            break;
        printf("[crash] frame %08lx\n", (unsigned long)ret);
        DWORD *next = (DWORD *)ebp[0];
        if (next <= ebp)
            break;
        ebp = next;
    }
    return 0;
}

void port_install_crash_reporter(void) { AddVectoredExceptionHandler(1, on_exception); }
