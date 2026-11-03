/* The native build's main code (dra2_native.dll): the game's original x86 code runs as it is, loaded from default.xbe at its Xbox
 * addresses, instead of the recompiled C. The original Xbox is a Pentium III, so the code needs no translation; only
 * the Xbox libraries the port replaces (Direct3D, DirectSound, XAPI: tools/gen_native.py lists them) are redirected,
 * each with a jmp at its entry to the port's function.
 *
 *   1. copy the XBE header and sections to their addresses: the range is the launcher's placeholder
 *      (port/native_launcher.c, dra2_native.exe at 0x10000), which this DLL makes writable and executable
 *   2. point each kernel import at a stub that reports it (the libraries the port replaces were its only users)
 *   3. write the jmps to the port's library functions
 *   4. run the startup tables the Xbox runtime ran (_initterm), then the game's main(0, 0, 0)
 */
int printf(const char *format, ...);
typedef struct _iobuf FILE;
FILE *__acrt_iob_func(unsigned index);
FILE *fopen(const char *path, const char *mode);
unsigned fread(void *buffer, unsigned size, unsigned count, FILE *f);
int fseek(FILE *f, long offset, int origin);
long ftell(FILE *f);
int fclose(FILE *f);
int setvbuf(FILE *stream, char *buffer, int mode, unsigned size);
int snprintf(char *out, unsigned size, const char *format, ...);
void *malloc(unsigned size);
void *memcpy(void *dst, const void *src, unsigned n);
void *memset(void *dst, int c, unsigned n);
void exit(int code);
char *getenv(const char *name);

#define WINAPI __stdcall
__declspec(dllimport) int WINAPI VirtualProtect(void *address, unsigned long size, unsigned long protect,
                                               unsigned long *old);
__declspec(dllimport) unsigned long WINAPI VirtualQuery(const void *address, void *info, unsigned long size);
__declspec(dllimport) void WINAPI ExitProcess(unsigned code);
__declspec(dllimport) unsigned long WINAPI GetModuleFileNameA(void *module, char *name, unsigned long size);
#define PAGE_EXECUTE_READWRITE 0x40

void port_install_crash_reporter(void);

/* read by the crash reporter (crash.c) */
volatile int port_init_index = -1;
volatile unsigned port_init_slot;
volatile int port_in_game_main;

/* from tools/gen_native.py (build/native/native_tables.s), each list ending with a zero pair */
struct native_pair { unsigned address; void *target; };
extern struct native_pair native_hooks[], native_kernel[];

/* The port's replacements keep the conventions the decompiled callers used; where the original differs, the jmp
 * goes to a wrapper with the original's. */
void XAPILIB__XPhysicalFree(void *address);
void __stdcall native_XPhysicalFree(void *address) { XAPILIB__XPhysicalFree(address); }
void __cdecl D3D8__D3DDevice_SetVertexShaderConstantNotInline(int reg, void *data, unsigned dwords);
void __fastcall native_SetVertexShaderConstantNotInline(int reg, void *data, unsigned dwords)
{
    D3D8__D3DDevice_SetVertexShaderConstantNotInline(reg, data, dwords);
}

#define XBE_BASE 0x10000u
#define GAME_MAIN 0x000a2250u
/* the startup tables, in the order the runtime's _cinit runs them: the C initializers (.CRT$XI*, among them the
 * atexit table's setup) before the C++ ones (.CRT$XC*: the runtime's, then the game's constructors). The linker sorts
 * the sections by name, so in memory the XC entries come first. */
static const struct { unsigned from, to; } init_tables[] = {
    { 0x2c2fd4, 0x2c2fec }, { 0x2bf860, 0x2c2fd0 },
};
static const struct { unsigned address; const char *name; } startup[] = {
    { 0x128dea, "FUN_00128dea" }, { 0x127b71, "XapiInitProcess" }, { 0x128d56, "_rtinit" }, { 0x128cfe, "_cinit" },
};

static unsigned char *read_xbe(unsigned *size)
{
    char path[520];
    const char *data = getenv("DRA2_DATA");
    if (data) {
        snprintf(path, sizeof path, "%s\\default.xbe", data);
    } else {
        GetModuleFileNameA(0, path, sizeof path);
        char *slash = path;
        for (char *p = path; *p; p++)
            if (*p == '\\')
                slash = p;
        snprintf(slash, sizeof path - (unsigned)(slash - path), "\\data\\default.xbe");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[native] can't open %s\n", path);
        exit(1);
    }
    fseek(f, 0, 2);
    *size = (unsigned)ftell(f);
    fseek(f, 0, 0);
    unsigned char *xbe = malloc(*size);
    fread(xbe, 1, *size, f);
    fclose(f);
    return xbe;
}

/* the library sections: their entries in the startup tables set up hardware the port replaces */
static int in_library(unsigned address, const unsigned char *xbe)
{
    unsigned count = *(unsigned *)(xbe + 0x11c), headers = *(unsigned *)(xbe + 0x120) - XBE_BASE;
    for (unsigned k = 0; k < count; k++) {
        const unsigned *s = (const unsigned *)(xbe + headers + 56 * k);
        const char *name = (const char *)(xbe + s[5] - XBE_BASE);
        if (address >= s[1] && address < s[1] + s[2])
            return name[0] != '.';  /* .text, .rdata, .data are the game's; D3D, DSOUND, XGRPH, ... are not */
    }
    return 0;
}

static void load_image(const unsigned char *xbe)
{
    unsigned image_size = *(unsigned *)(xbe + 0x10c), header_size = *(unsigned *)(xbe + 0x108);
    /* the launcher's image: its allocation must start at the Xbox base and reach past the game's image */
    struct { void *base, *allocation_base; unsigned long allocation_protect, size, state, protect, type; } info;
    VirtualQuery((void *)XBE_BASE, &info, sizeof info);
    unsigned long old;
    unsigned char *image = (unsigned char *)XBE_BASE;
    if (info.allocation_base != (void *)XBE_BASE ||
        !VirtualProtect(image, image_size, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[native] the Xbox image range %#x-%#x isn't the launcher's placeholder (allocation at %p); run "
               "dra2_native.exe\n", XBE_BASE, XBE_BASE + image_size, info.allocation_base);
        exit(1);
    }
    /* Windows still reads the process's .exe headers at its base (CreateThread takes the default stack size from
     * them: with the XBE header there it failed with ERROR_BAD_EXE_FORMAT). The XBE header ends at 0xa20, and its
     * bytes 4-0x103 are a signature nothing reads at run time: so "MZ" stays at 0, e_lfanew (at 0x3c) points past the
     * XBE header, and the launcher's PE headers are copied there. */
    unsigned pe_at = *(unsigned *)(image + 0x3c);
    unsigned pe_size = 24 + *(unsigned short *)(image + pe_at + 20) + 40 * *(unsigned short *)(image + pe_at + 6);
    unsigned char pe[0x400];
    if (pe_size > sizeof pe || header_size > 0xc00) {
        printf("[native] no room to keep the launcher's PE headers beside the XBE header\n");
        exit(1);
    }
    memcpy(pe, image + pe_at, pe_size);
    memcpy(image, xbe, header_size);
    image[0] = 'M', image[1] = 'Z';
    *(unsigned *)(image + 0x3c) = 0xc00;
    memcpy(image + 0xc00, pe, pe_size);
    unsigned count = *(unsigned *)(xbe + 0x11c), headers = *(unsigned *)(xbe + 0x120) - XBE_BASE;
    for (unsigned k = 0; k < count; k++) {
        const unsigned *s = (const unsigned *)(xbe + headers + 56 * k);  /* flags, va, vsize, raw, rsize, name */
        memset((void *)s[1], 0, s[2]);
        memcpy((void *)s[1], xbe + s[3], s[4] < s[2] ? s[4] : s[2]);
    }
    printf("[native] loaded %u sections at %#x-%#x\n", count, XBE_BASE, XBE_BASE + image_size);
}

/* XAPI internals the native build replaces itself (no port equivalent: the recompiled build never ran them) */
static long native_disc_authenticated(void) { return 0; }
static long __stdcall native_mount_utility_drive(void *device) { (void)device; return 0; }
static long __stdcall native_map_drives(void *a, void *b) { (void)a, (void)b; return 0; }
static int __stdcall native_XMountUtilityDrive(int format_clean) { (void)format_clean; return 1; }
static void native_skip(void) {}

/* XAPI's heap, on Windows heaps: the port already allocates (RtlAllocateHeap) and frees (HeapFree) with Windows, so
 * every heap must be a Windows heap; XAPI's own RtlCreateHeap built Xbox heap objects (the process heap among them)
 * that Windows' HeapAlloc then corrupted */
__declspec(dllimport) void *WINAPI HeapCreate(unsigned long options, unsigned long initial, unsigned long maximum);
__declspec(dllimport) unsigned long WINAPI HeapSize(void *heap, unsigned long flags, const void *mem);
unsigned char __stdcall XAPILIB__HeapFree(void *heap, unsigned long flags, void *mem);
#define PORT_HEAP_SLACK 16  /* port/xapi.c's allocations carry 16 bytes past the requested size (GUARD) */
static void *__stdcall native_RtlCreateHeap(unsigned long flags, void *base, unsigned long reserve,
                                            unsigned long commit, void *lock, void *parameters)
{
    (void)flags, (void)base, (void)reserve, (void)commit, (void)lock, (void)parameters;
    return HeapCreate(0, 0, 0);
}
/* The Xbox heap returns 16-byte aligned blocks, and the game's SSE code relies on it (FUN_00130831, its matrix
 * product, uses movaps on heap objects); Windows' 32-bit heap aligns to 8. Every block is over-allocated and handed
 * out at the next 16-byte boundary at least 8 bytes in, with a marker and the raw block pointer just below it. The
 * same 16 bytes of slack as the port's allocator follow it (the game writes a little past some blocks). */
__declspec(dllimport) void *WINAPI HeapAlloc(void *heap, unsigned long flags, unsigned long size);
__declspec(dllimport) int WINAPI HeapFree(void *heap, unsigned long flags, void *mem);
__declspec(dllimport) int WINAPI HeapValidate(void *heap, unsigned long flags, const void *mem);
#define ALIGNED_MARK 0xa11e9ed0u
#define ALIGN_OVERHEAD (8 + 15)

static void *__stdcall native_RtlAllocateHeap(void *heap, unsigned long flags, unsigned long size)
{
    unsigned char *raw = HeapAlloc(heap, flags & 0x8 /* HEAP_ZERO_MEMORY */, size + ALIGN_OVERHEAD + PORT_HEAP_SLACK);
    if (!raw)
        return 0;
    unsigned char *p = (unsigned char *)(((unsigned)raw + 8 + 15) & ~15u);
    ((unsigned *)p)[-2] = ALIGNED_MARK;
    ((void **)p)[-1] = raw;
    return p;
}

static void *raw_block(void *p)
{
    return p && ((unsigned *)p)[-2] == ALIGNED_MARK ? ((void **)p)[-1] : 0;
}

static unsigned char __stdcall native_RtlFreeHeap(void *heap, unsigned long flags, void *p)
{
    (void)flags;
    void *raw = raw_block(p);
    if (!p)
        return 1;
    if (!raw || !HeapValidate(heap, 0, raw)) {
        static int reported;
        if (reported++ < 8)
            printf("[native] free of %p: not a block of this heap, skipped\n", p);
        return 0;
    }
    ((unsigned *)p)[-2] = 0;
    return HeapFree(heap, 0, raw) != 0;
}

static unsigned long __stdcall native_RtlSizeHeap(void *heap, unsigned long flags, const void *p)
{
    (void)flags;
    unsigned char *raw = raw_block((void *)p);
    if (!raw)
        return (unsigned long)-1;
    unsigned long size = HeapSize(heap, 0, raw);
    unsigned long used = (unsigned long)((const unsigned char *)p - raw) + PORT_HEAP_SLACK;
    return size == (unsigned long)-1 || size < used ? (unsigned long)-1 : size - used;
}

static void *__stdcall native_RtlReAllocateHeap(void *heap, unsigned long flags, void *p, unsigned long size)
{
    if (!p)
        return native_RtlAllocateHeap(heap, flags, size);
    unsigned long old = native_RtlSizeHeap(heap, 0, p);
    if (old == (unsigned long)-1 || (flags & 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */ && size > old))
        return size <= old && old != (unsigned long)-1 ? p : 0;
    void *q = native_RtlAllocateHeap(heap, flags, size);
    if (q) {
        memcpy(q, p, old < size ? old : size);
        native_RtlFreeHeap(heap, 0, p);
    }
    return q;
}
static const struct { unsigned address; void *target; const char *what; } native_extra_hooks[] = {
    /* XapiInitProcess's disc check: SCSI MODE SENSE (0x5a) through NtDeviceIoControlFile, IOCTL 0x4d014, asking
     * the DVD drive whether the disc is an authenticated Xbox disc; on failure the game boots to the dashboard */
    { 0x127997, (void *)native_disc_authenticated, "DVD authentication check" },
    /* mounting the hard disk's utility partition as Z: (the port maps Z: to a Windows folder, port/xapi.c) */
    { 0x127b60, (void *)native_mount_utility_drive, "utility drive mount" },
    /* linking the drive letters (D: the disc's game folder, and the title and user save drives) to devices */
    { 0x127826, (void *)native_map_drives, "drive letter mapping" },
    /* XMountUtilityDrive: mounts (and if need be formats) the hard disk's cache partition as Z: */
    { 0x125d18, (void *)native_XMountUtilityDrive, "XMountUtilityDrive" },
    /* the end of XapiInitProcess: reads the symbolic links back to find the drive the game was launched from */
    { 0x128977, (void *)native_skip, "launch drive lookup" },
    /* the heap internals XAPI's HeapCreate, HeapFree, LocalFree, LocalSize and the runtime's _msize use */
    { 0x12aa00, (void *)native_RtlCreateHeap, "RtlCreateHeap" },
    { 0x12be19, (void *)native_RtlFreeHeap, "RtlFreeHeap" },
    { 0x12a482, (void *)native_RtlSizeHeap, "RtlSizeHeap" },
    /* allocation and HeapFree too: the port's versions (port/xapi.c) don't align to 16 */
    { 0x12ae24, (void *)native_RtlAllocateHeap, "RtlAllocateHeap" },
    { 0x1294b1, (void *)native_RtlFreeHeap, "HeapFree" },
    /* RtlReAllocateHeap: the engine's realloc (0x1d7410) and the runtime's call it directly; its Xbox heap code
     * resized Windows blocks and corrupted the process heap */
    { 0x12c1d0, (void *)native_RtlReAllocateHeap, "RtlReAllocateHeap" },
};

static void link_image(void)
{
    int kernel = 0, hooks = 0;
    for (struct native_pair *k = native_kernel; k->address; k++, kernel++)
        *(void **)k->address = k->target;
    for (struct native_pair *h = native_hooks; h->address; h++, hooks++) {
        void *target = h->target;
        if (target == (void *)XAPILIB__XPhysicalFree)
            target = (void *)native_XPhysicalFree;
        else if (target == (void *)D3D8__D3DDevice_SetVertexShaderConstantNotInline)
            target = (void *)native_SetVertexShaderConstantNotInline;
        unsigned char *at = (unsigned char *)h->address;
        at[0] = 0xe9;  /* jmp rel32 */
        *(int *)(at + 1) = (int)((unsigned)target - (h->address + 5));
    }
    for (unsigned k = 0; k < sizeof native_extra_hooks / sizeof native_extra_hooks[0]; k++, hooks++) {
        unsigned char *at = (unsigned char *)native_extra_hooks[k].address;
        at[0] = 0xe9;
        *(int *)(at + 1) = (int)((unsigned)native_extra_hooks[k].target - (native_extra_hooks[k].address + 5));
    }
    printf("[native] %d kernel imports routed to reports, %d library functions redirected\n", kernel, hooks);
}

/* --- the Xbox thread block (fs:) ---
 * Xbox code reads the processor control region through fs: fs:[4] is the array of per-thread TLS blocks (indexed by
 * the TLS index at 0x2f7eec), fs:[0x20] the processor block, fs:[0x24] the interrupt level, fs:[0x28] the current
 * thread (its +0x28 the TLS data), fs:[0x58] a DPC flag. On Windows those offsets of the TEB mean other things, and
 * fs:[4] (the stack base) can't change, so each such read in the image is rewritten to read a Windows TLS slot
 * (TEB TlsSlots, fs:[0xe10 + 4 * slot]) holding this thread's value. fs:[0] (the exception chain) is the same. */
__declspec(dllimport) unsigned long WINAPI TlsAlloc(void);
__declspec(dllimport) int WINAPI TlsSetValue(unsigned long slot, void *value);
void *calloc(unsigned count, unsigned size);

#define TLS_INDEX_ADDRESS 0x2f7eec
#define TLS_BLOCK_SIZE 0x90  /* the XBE's TLS directory: no initial data, 0x90 bytes zero-filled */
static unsigned long slot_tls_array, slot_prcb, slot_thread, slot_zero;

struct native_thread {
    void *tls_array[1];          /* fs:[4]: [TLS index] = this thread's TLS block */
    void *prcb[4];               /* fs:[0x20]: the processor block, CurrentThread first (fs:[0x28] is the same) */
    unsigned char thread[0x100]; /* fs:[0x28]: the thread object; +0x28 is its TLS data */
    unsigned char tls_block[TLS_BLOCK_SIZE];
};

static void setup_thread(void)
{
    struct native_thread *t = calloc(1, sizeof *t);
    t->tls_array[0] = t->tls_block;
    t->prcb[0] = t->thread;
    *(void **)(t->thread + 0x28) = t->tls_block;
    TlsSetValue(slot_tls_array, t->tls_array);
    TlsSetValue(slot_prcb, t->prcb);
    TlsSetValue(slot_thread, t->thread);
}

static unsigned long slot_for(unsigned offset)
{
    switch (offset) {
    case 0x04: return slot_tls_array;
    case 0x20: return slot_prcb;
    case 0x28: return slot_thread;
    case 0x24: case 0x58: return slot_zero;  /* interrupt level, DPC active: 0 */
    default: return ~0ul;
    }
}

/* rewrite "mov eax, fs:[x]" (64 a1 x), "mov r32, fs:[x]" (64 8b modrm x) and "movzx r32, byte fs:[x]"
 * (64 0f b6 modrm x), modrm being a bare disp32, in the code sections */
static void patch_fs_reads(const unsigned char *xbe)
{
    int patched = 0;
    unsigned count = *(unsigned *)(xbe + 0x11c), headers = *(unsigned *)(xbe + 0x120) - XBE_BASE;
    for (unsigned k = 0; k < count; k++) {
        const unsigned *s = (const unsigned *)(xbe + headers + 56 * k);
        const char *name = (const char *)(xbe + s[5] - XBE_BASE);
        if (name[0] == '$' || !(name[0] == '.' ? name[1] == 't' : 1) || !__builtin_strcmp(name, "DOLBY"))
            continue;  /* .text and the library code sections; not .rdata, .data, DOLBY (DSP code) or $$ */
        unsigned char *p = (unsigned char *)s[1], *end = p + s[4];
        for (; p + 8 <= end; p++) {
            if (p[0] != 0x64)
                continue;
            unsigned *disp = 0;
            if (p[1] == 0xa1)
                disp = (unsigned *)(p + 2);
            else if (p[1] == 0x8b && (p[2] & 0xc7) == 0x05)
                disp = (unsigned *)(p + 3);
            else if (p[1] == 0x0f && p[2] == 0xb6 && (p[3] & 0xc7) == 0x05)
                disp = (unsigned *)(p + 4);
            if (!disp)
                continue;
            unsigned long slot = slot_for(*disp);
            if (slot == ~0ul)
                continue;
            *disp = 0xe10 + 4 * slot;
            patched++;
        }
    }
    printf("[native] %d reads of the Xbox thread block (fs:) rewritten\n", patched);
}

static void setup_fs(const unsigned char *xbe)
{
    slot_tls_array = TlsAlloc(), slot_prcb = TlsAlloc(), slot_thread = TlsAlloc(), slot_zero = TlsAlloc();
    if (slot_zero >= 64) {  /* TlsSlots holds 64; later ones live elsewhere */
        printf("[native] no TLS slot below 64 left\n");
        exit(1);
    }
    *(unsigned *)TLS_INDEX_ADDRESS = 0;
    patch_fs_reads(xbe);
    setup_thread();
}

/* each thread the game starts (XAPI CreateThread, port/xapi.c) gets its blocks when it attaches */
int WINAPI DllMain(void *module, unsigned long reason, void *reserved)
{
    (void)module, (void)reserved;
    if (reason == 2 /* DLL_THREAD_ATTACH */ && slot_tls_array)
        setup_thread();
    return 1;
}

static void run_startup_tables(const unsigned char *xbe)
{
    int ran = 0, skipped = 0, trace = getenv("DRA2_TRACE_INIT") != 0;
    for (unsigned t = 0; t < sizeof init_tables / sizeof init_tables[0]; t++)
        for (unsigned slot = init_tables[t].from; slot < init_tables[t].to; slot += 4) {
            unsigned address = *(unsigned *)slot;
            if (address == 0 || address == 0xffffffffu)
                continue;
            if (in_library(address, xbe)) {
                skipped++;
                continue;
            }
            port_init_index = ran;
            port_init_slot = slot;
            if (trace)
                printf("[native] init %d: slot %#x -> %#x\n", ran, slot, address);
            ((void (*)(void))address)();
            ran++;
        }
    port_init_index = -1;
    printf("[native] startup tables: %d run, %d library entries skipped\n", ran, skipped);
}

/* called by the launcher, whose code the image copy overwrites: never returns */
/* Floating-point exceptions: the Xbox runs with them masked (an invalid operation gives a NaN), but the game's threads
 * here sometimes run with invalid and divide-by-zero unmasked (x87 control word 0x0f5a, which nothing in the XBE
 * loads). Report the first, mask them all in the faulting thread and let the instruction run again. */
__declspec(dllimport) void *WINAPI AddVectoredExceptionHandler(unsigned long first, long(WINAPI *handler)(void *));
static long WINAPI float_exception(void *pointers)
{
    unsigned long code = **(unsigned long **)pointers;
    unsigned char *context = ((unsigned char **)pointers)[1];
    unsigned long *control = (unsigned long *)(context + 0x1c), *status = (unsigned long *)(context + 0x20);
    unsigned long *mxcsr = (unsigned long *)(context + 0xcc + 24);  /* in ExtendedRegisters, the FXSAVE area */
    static int reported;
    if (code >= 0xc000008d && code <= 0xc0000093) {  /* x87: STATUS_FLOAT_DENORMAL_OPERAND .. _UNDERFLOW */
        if (!reported++)
            printf("[native] x87 exception %08lx at %08lx (control word %04lx): masked\n", code,
                   *(unsigned long *)(context + 0xb8), *control & 0xffff);
        *control |= 0x3f;
        *status &= ~0x80ffu;  /* the exception flags, ES and B */
        return -1;  /* EXCEPTION_CONTINUE_EXECUTION */
    }
    if (code == 0xc00002b4 || code == 0xc00002b5) {  /* SSE: STATUS_FLOAT_MULTIPLE_FAULTS / _TRAPS */
        if (!reported++)
            printf("[native] SSE exception %08lx at %08lx (MXCSR %04lx): masked\n", code,
                   *(unsigned long *)(context + 0xb8), *mxcsr & 0xffff);
        *mxcsr = (*mxcsr | 0x1f80) & ~0x3fu;  /* mask all, clear the flags */
        return -1;
    }
    return 0;  /* EXCEPTION_CONTINUE_SEARCH */
}

__declspec(dllexport) void native_main(void)
{
    setvbuf(__acrt_iob_func(1), 0, 4 /* _IONBF */, 0);
    port_install_crash_reporter();
    AddVectoredExceptionHandler(1, float_exception);
    unsigned size;
    unsigned char *xbe = read_xbe(&size);
    load_image(xbe);
    link_image();
    setup_fs(xbe);
    void native_kernel_start(void);
    native_kernel_start();
    /* the game asks for gamepads with the Xbox's own device-type descriptor (XDEVICE_TYPE_GAMEPAD), not the port's */
    extern void *port_gamepad_type;
    port_gamepad_type = (void *)0x28ab14;
    /* DRA2_ARENA=levels/<name> (at most 24 characters, e.g. levels/final_cavern): the attract mode's demo fights
     * always take place there. The arena list builder (0x54f90) gets the single-arena branch it takes when the
     * carousel is empty (je at 0x54fd9 -> jmp), and that branch's name (0x29631c, "levels/final_oldwesttown") is
     * replaced. */
    const char *arena = getenv("DRA2_ARENA");
    unsigned arena_length = 0;
    while (arena && arena[arena_length])
        arena_length++;
    if (arena && arena_length <= 24) {
        unsigned char *branch = (unsigned char *)0x54fd9;
        branch[0] = 0x90, branch[1] = 0xe9;
        memcpy((char *)0x29631c, arena, arena_length + 1);
    } else if (arena)
        printf("[native] DRA2_ARENA is longer than 24 characters: ignored\n");
    /* what the Xbox startup thread (0x126fac) runs before main: process setup (heaps), the runtime's _rtinit and
     * _cinit (which runs the startup tables, C initializers first) */
    for (unsigned k = 0; k < sizeof startup / sizeof startup[0]; k++) {
        port_init_index = (int)k;
        port_init_slot = startup[k].address;
        printf("[native] startup: %s\n", startup[k].name);
        ((void (*)(void))startup[k].address)();
    }
    port_init_index = -1;
    (void)run_startup_tables;
    printf("[native] calling the game's main at %#x\n", GAME_MAIN);
    port_in_game_main = 1;
    ExitProcess((unsigned)((int (*)(int, char **, char **))GAME_MAIN)(0, 0, 0));
}
