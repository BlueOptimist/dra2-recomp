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

/* the return addresses up the EBP frame chain, starting with eip; returns how many */
static int walk_chain(DWORD eip, DWORD *ebp, DWORD *out, int max)
{
    int n = 0;
    out[n++] = eip;
    for (; n < max && ebp && !IsBadReadPtr(ebp, 8); n++) {
        DWORD ret = ebp[1];
        if (!ret)
            break;
        out[n] = ret;
        DWORD *next = (DWORD *)ebp[0];
        if (next <= ebp) {
            n++;
            break;
        }
        ebp = next;
    }
    return n;
}

static void print_chain(const char *tag, DWORD eip, DWORD *ebp)
{
    DWORD chain[33];
    int n = walk_chain(eip, ebp, chain, 33);
    for (int k = 0; k < n; k++)
        printf("[%s] frame %08lx\n", tag, (unsigned long)chain[k]);
}

static void print_watches(void);
static void parse_watch(void);
static DWORD sampler_thread_id;
static int calls_limit = 80;  /* DRA2_CALLS=<n>: how many functions each "[calls]" report lists */
void port_calls_report(int limit);
void port_calls_recent(void);
WINIMPORT DWORD WINAPI GetCurrentThreadId(void);

/* DRA2_WATCHWRITE=addr[:off...] (same syntax as DRA2_WATCH; the last value read is the address to watch): once the
 * pointer chain resolves, the sampler arms debug register DR0 on the game thread for 4-byte writes there, and each
 * write is reported with the writing code's call chain ("[watch]" lines). For "who overwrote this object's vtable". */
static DWORD watchw_addr, watchw_off[6];
static int watchw_depth = -1, watchw_armed, watchw_hits;
#define CTX_DR0 0x04
#define CTX_DR6 0x14
#define CTX_DR7 0x18
#define CONTEXT_I386_DEBUG_REGISTERS 0x00010010

static long WINAPI on_exception(EXCEPTION_POINTERS_32 *info)
{
    DWORD code = info->record->ExceptionCode;
    if (code == 0x80000004u && watchw_armed) {  /* EXCEPTION_SINGLE_STEP: the data breakpoint fired */
        void *c = info->context;
        if (watchw_hits++ < 8) {
            DWORD addr = CTX_U32(c, CTX_DR0);
            printf("[watch] write to %08lx, now %08lx\n", (unsigned long)addr,
                   IsBadReadPtr((void *)addr, 4) ? 0ul : (unsigned long)*(DWORD *)addr);
            DWORD chain[16];
            int n = walk_chain(CTX_U32(c, CTX_EIP), (DWORD *)CTX_U32(c, CTX_EBP), chain, 16);
            for (int k = 0; k < n; k++)
                printf("[crash] frame %08lx\n", (unsigned long)chain[k]);
        }
        CTX_U32(c, CTX_DR6) = 0;
        return -1; /* EXCEPTION_CONTINUE_EXECUTION */
    }
    if (code < 0xC0000000u || code == 0xC0000409u /* abort(): already reported */)
        return 0; /* EXCEPTION_CONTINUE_SEARCH */
    if (sampler_thread_id && GetCurrentThreadId() == sampler_thread_id)
        return 0; /* IsBadReadPtr probing a watch: handled by its own SEH */
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
    /* ECX is usually "this": its first words and the vtable they point to (a null virtual call shows the slot) */
    DWORD *obj = (DWORD *)CTX_U32(c, CTX_ECX);
    if (!IsBadReadPtr(obj, 16)) {
        printf("[crash] [ecx] %08lx %08lx %08lx %08lx\n", obj[0], obj[1], obj[2], obj[3]);
        DWORD *vt = (DWORD *)obj[0];
        if (!IsBadReadPtr(vt, 64))
            for (int k = 0; k < 16; k += 4)
                printf("[crash] vtable+%02x %08lx %08lx %08lx %08lx\n", k * 4, vt[k], vt[k + 1], vt[k + 2], vt[k + 3]);
    }
    /* the top of the stack: after a call through a null pointer, [esp] is the return address into the caller */
    DWORD *esp = (DWORD *)CTX_U32(c, CTX_ESP);
    for (int k = 0; k < 64 && !IsBadReadPtr(esp + k, 4); k++)  /* deep enough to get past C runtime frames */
        printf("[crash] stack %08lx\n", (unsigned long)esp[k]);
    print_chain("crash", CTX_U32(c, CTX_EIP), (DWORD *)CTX_U32(c, CTX_EBP));
    print_watches();
    port_calls_recent();             /* with an -Instrument build: the last function entries */
    port_calls_report(calls_limit);  /* ... and what ran since the last sample */
    return 0;
}

/* DRA2_SAMPLE=<seconds> (or <n>ms): every so often, stop the game's thread and print where it is ("[sample] frame" lines, for
 * tools/symbolize.py). For a game that runs but shows nothing: a loop it never leaves shows up as the same chain. */
WINIMPORT HANDLE WINAPI GetCurrentProcess(void);
WINIMPORT HANDLE WINAPI GetCurrentThread(void);
WINIMPORT BOOL WINAPI DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE *dst, DWORD access,
                                      BOOL inherit, DWORD options);
WINIMPORT DWORD WINAPI SuspendThread(HANDLE thread);
WINIMPORT DWORD WINAPI ResumeThread(HANDLE thread);
WINIMPORT BOOL WINAPI GetThreadContext(HANDLE thread, void *context);
WINIMPORT BOOL WINAPI SetThreadContext(HANDLE thread, const void *context);
#define DUPLICATE_SAME_ACCESS 2
#define CONTEXT_I386_CONTROL_INTEGER 0x00010003

static HANDLE sampled_thread;
static DWORD sample_ms;

/* DRA2_WATCH=addr[:offset[:offset...]],...: dwords printed with every sample ("[sample] watch" lines). Each
 * ":offset" reads the pointer at the current address and moves to pointer+offset. Addresses (hex) are the .exe's;
 * tools/run_game.ps1 -Watch takes label names instead. */
#define WATCH_MAX 16
#define WATCH_DEPTH 6
static DWORD watch_addr[WATCH_MAX], watch_off[WATCH_MAX][WATCH_DEPTH];
static int watch_depth[WATCH_MAX], watch_count;

static DWORD parse_hex(const char **s)
{
    DWORD v = 0;
    for (;; (*s)++) {
        char ch = **s;
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
              : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0)
            return v;
        v = v * 16 + (DWORD)d;
    }
}

static void parse_watch(void)
{
    static char spec[512];
    DWORD n = GetEnvironmentVariableA("DRA2_WATCH", spec, sizeof spec);
    if (!n || n >= sizeof spec)
        return;
    for (const char *s = spec; *s && watch_count < WATCH_MAX;) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        watch_addr[watch_count] = parse_hex(&s);
        while (*s == ':' && watch_depth[watch_count] < WATCH_DEPTH) {
            s++;
            watch_off[watch_count][watch_depth[watch_count]++] = parse_hex(&s);
        }
        watch_count++;
        while (*s && *s != ',')
            s++;
        while (*s == ',' || *s == ' ')
            s++;
    }
}

static void print_watches(void)
{
    for (int k = 0; k < watch_count; k++) {
        char path[96];
        int len = 0;
        DWORD *p = (DWORD *)watch_addr[k];
        int ok = 1;
        for (int d = 0; d < watch_depth[k] && ok; d++) {
            len += __builtin_snprintf(path + len, sizeof path - len, ":%lx", watch_off[k][d]);
            ok = !IsBadReadPtr(p, 4);
            if (ok)
                p = (DWORD *)(*p + watch_off[k][d]);
        }
        path[len] = 0;
        if (ok && !IsBadReadPtr(p, 4))
            printf("[sample] watch %08lx%s = %08lx\n", watch_addr[k], path, *p);
        else
            printf("[sample] watch %08lx%s: unreadable (at %08lx)\n", watch_addr[k], path, (DWORD)p);
    }
}

static DWORD WINAPI sampler(void *param)
{
    (void)param;
    static __attribute__((aligned(16))) unsigned char context[0x2CC];
    for (int n = 1;; n++) {
        Sleep(sample_ms);
        if (SuspendThread(sampled_thread) == (DWORD)-1)
            return 0;
        /* arm the write watchpoint once its pointer chain resolves (the thread is stopped, so its context can change) */
        DWORD armed_at = 0;
        if (watchw_depth >= 0 && !watchw_armed) {
            DWORD *p = (DWORD *)watchw_addr;
            int ok = 1;
            for (int d = 0; d < watchw_depth && ok; d++) {
                ok = !IsBadReadPtr(p, 4) && *p;
                if (ok)
                    p = (DWORD *)(*p + watchw_off[d]);
            }
            if (ok && !IsBadReadPtr(p, 4)) {
                static __attribute__((aligned(16))) unsigned char dctx[0x2CC];
                CTX_U32(dctx, 0) = CONTEXT_I386_DEBUG_REGISTERS;
                if (GetThreadContext(sampled_thread, dctx)) {
                    CTX_U32(dctx, CTX_DR0) = (DWORD)p;
                    CTX_U32(dctx, CTX_DR7) = 1u | (1u << 16) | (3u << 18); /* L0, write, 4 bytes */
                    CTX_U32(dctx, 0) = CONTEXT_I386_DEBUG_REGISTERS;
                    if (SetThreadContext(sampled_thread, dctx)) {
                        watchw_armed = 1;
                        armed_at = (DWORD)p;
                    }
                }
            }
        }
        /* copy the chain while the thread is stopped, print after it runs again (it may hold the stdout lock) */
        DWORD chain[33];
        int count = 0;
        CTX_U32(context, 0) = CONTEXT_I386_CONTROL_INTEGER;
        if (GetThreadContext(sampled_thread, context))
            count = walk_chain(CTX_U32(context, CTX_EIP), (DWORD *)CTX_U32(context, CTX_EBP), chain, 33);
        ResumeThread(sampled_thread);
        if (armed_at)
            printf("[watch] armed: writes to %08lx (now %08lx)\n", (unsigned long)armed_at,
                   (unsigned long)*(DWORD *)armed_at);
        printf("[sample] %d after %lu ms\n", n, (unsigned long)(n * sample_ms));
        for (int k = 0; k < count; k++)
            printf("[sample] frame %08lx\n", (unsigned long)chain[k]);
        print_watches();
        port_calls_report(calls_limit);
    }
}

void port_install_crash_reporter(void)
{
    AddVectoredExceptionHandler(1, on_exception);
    parse_watch();  /* printed with each sample and in a crash report */
    static char ww[128];
    DWORD wn = GetEnvironmentVariableA("DRA2_WATCHWRITE", ww, sizeof ww);
    if (wn && wn < sizeof ww) {
        const char *s = ww;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        watchw_addr = parse_hex(&s);
        watchw_depth = 0;
        while (*s == ':' && watchw_depth < 6) {
            s++;
            watchw_off[watchw_depth++] = parse_hex(&s);
        }
    }
    char limit[12];
    DWORD ln = GetEnvironmentVariableA("DRA2_CALLS", limit, sizeof limit);
    if (ln && ln < sizeof limit) {
        calls_limit = 0;
        for (char *q = limit; *q >= '0' && *q <= '9'; q++)
            calls_limit = calls_limit * 10 + (*q - '0');
    }
    char value[16];
    DWORD n = GetEnvironmentVariableA("DRA2_SAMPLE", value, sizeof value);
    if (n && n < sizeof value) {
        int amount = 0;  /* seconds, or milliseconds with an "ms" suffix */
        char *p = value;
        for (; *p >= '0' && *p <= '9'; p++)
            amount = amount * 10 + (*p - '0');
        if (amount > 0) {
            sample_ms = (DWORD)amount * (p[0] == 'm' && p[1] == 's' ? 1 : 1000);
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampled_thread, 0, 0,
                            DUPLICATE_SAME_ACCESS);
            CreateThread(0, 0, sampler, 0, 0, &sampler_thread_id);
        }
    }
}
