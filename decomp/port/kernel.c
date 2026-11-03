/* Xbox kernel exports for the native build, on Windows. The game's original code imports 158 of them; most are
 * only reached through the Xbox libraries the port replaces, so they are added as runs reach them (an import with no
 * xboxkrnl_<name> here goes to a stub that reports it, tools/gen_native.py). Conventions are the kernel's: __stdcall,
 * except where noted. */
typedef unsigned long ULONG;
typedef long NTSTATUS;
typedef unsigned char UCHAR;
#define STATUS_SUCCESS 0
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)

int printf(const char *format, ...);
int vprintf(const char *format, char *args);
void *malloc(unsigned size);
void free(void *p);
void abort(void);

#define WINAPI __stdcall
typedef struct { void *debug; long lock_count, recursion; void *owner, *lock_semaphore; ULONG spin; } CRITICAL_SECTION;
__declspec(dllimport) void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs);
__declspec(dllimport) void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs);
__declspec(dllimport) void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs);
__declspec(dllimport) void *WINAPI VirtualAlloc(void *address, ULONG size, ULONG type, ULONG protect);
__declspec(dllimport) int WINAPI VirtualFree(void *address, ULONG size, ULONG type);
__declspec(dllimport) ULONG WINAPI VirtualQuery(const void *address, void *info, ULONG size);
__declspec(dllimport) void WINAPI Sleep(ULONG ms);
__declspec(dllimport) int WINAPI SwitchToThread(void);
__declspec(dllimport) void WINAPI GetSystemTimeAsFileTime(void *time);
__declspec(dllimport) void WINAPI RtlUnwind(void *frame, void *target, void *record, void *value);
__declspec(dllimport) void WINAPI RaiseException(ULONG code, ULONG flags, ULONG count, const ULONG *args);
__declspec(dllimport) int WINAPI CloseHandle(void *h);
__declspec(dllimport) void WINAPI ExitProcess(unsigned code);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

/* --- critical sections ---
 * The game's are in the Xbox layout, and some are initialised statically in .data, never passed to
 * RtlInitializeCriticalSection (XAPI's thread-notify list, 0x2ed7fc): Windows' EnterCriticalSection read those bytes
 * as its own layout and waited forever. So each Xbox critical section gets its own Windows one, kept in a table by
 * address and created on first use; the Xbox bytes are left alone. */
#define CS_TABLE 4096
static struct { void *xbox; CRITICAL_SECTION cs; } cs_table[CS_TABLE];
static CRITICAL_SECTION cs_table_lock;
static volatile long cs_table_ready;
__declspec(dllimport) long WINAPI InterlockedCompareExchange(volatile long *target, long value, long comparand);

static CRITICAL_SECTION *windows_cs(void *xbox)
{
    if (InterlockedCompareExchange(&cs_table_ready, 1, 0) == 0) {
        InitializeCriticalSection(&cs_table_lock);
        cs_table_ready = 2;
    }
    while (cs_table_ready != 2)
        Sleep(0);
    unsigned h = ((unsigned)xbox >> 2) % CS_TABLE;
    for (unsigned n = 0; n < CS_TABLE; n++, h = (h + 1) % CS_TABLE)
        if (cs_table[h].xbox == xbox)
            return &cs_table[h].cs;
        else if (!cs_table[h].xbox)
            break;
    EnterCriticalSection(&cs_table_lock);  /* not found: add it (looking again, another thread may have) */
    h = ((unsigned)xbox >> 2) % CS_TABLE;
    for (unsigned n = 0; n < CS_TABLE; n++, h = (h + 1) % CS_TABLE) {
        if (cs_table[h].xbox == xbox)
            break;
        if (!cs_table[h].xbox) {
            InitializeCriticalSection(&cs_table[h].cs);
            cs_table[h].xbox = xbox;
            break;
        }
    }
    LeaveCriticalSection(&cs_table_lock);
    if (cs_table[h].xbox != xbox) {
        printf("[native] critical section table full\n");
        abort();
    }
    return &cs_table[h].cs;
}

void WINAPI xboxkrnl_RtlInitializeCriticalSection(void *cs) { windows_cs(cs); }
void WINAPI xboxkrnl_RtlEnterCriticalSection(void *cs) { EnterCriticalSection(windows_cs(cs)); }
void WINAPI xboxkrnl_RtlLeaveCriticalSection(void *cs) { LeaveCriticalSection(windows_cs(cs)); }

/* --- memory --- */
NTSTATUS WINAPI xboxkrnl_NtAllocateVirtualMemory(void **base, ULONG zero_bits, ULONG *size, ULONG type, ULONG protect)
{
    (void)zero_bits;
    void *p = VirtualAlloc(*base, *size, type, protect);
    if (!p)
        return STATUS_NO_MEMORY;
    *base = p;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI xboxkrnl_NtFreeVirtualMemory(void **base, ULONG *size, ULONG type)
{
    return VirtualFree(*base, type == MEM_RELEASE ? 0 : *size, type) ? STATUS_SUCCESS : (NTSTATUS)0xC0000045;
}

NTSTATUS WINAPI xboxkrnl_NtQueryVirtualMemory(void *base, void *info)
{
    return VirtualQuery(base, info, 28) ? STATUS_SUCCESS : (NTSTATUS)0xC000000D;
}

void *WINAPI xboxkrnl_MmAllocateContiguousMemory(ULONG size)
{
    return VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void *WINAPI xboxkrnl_MmAllocateContiguousMemoryEx(ULONG size, ULONG low, ULONG high, ULONG alignment, ULONG protect)
{
    (void)low, (void)high, (void)alignment, (void)protect;
    return VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void WINAPI xboxkrnl_MmFreeContiguousMemory(void *p) { VirtualFree(p, 0, MEM_RELEASE); }

void *WINAPI xboxkrnl_ExAllocatePoolWithTag(ULONG size, ULONG tag) { (void)tag; return malloc(size); }
void WINAPI xboxkrnl_ExFreePool(void *p) { free(p); }

/* --- memory and strings --- */
ULONG WINAPI xboxkrnl_RtlCompareMemory(const UCHAR *a, const UCHAR *b, ULONG n)
{
    ULONG i = 0;
    while (i < n && a[i] == b[i])
        i++;
    return i;
}

void WINAPI xboxkrnl_RtlFillMemoryUlong(ULONG *p, ULONG bytes, ULONG value)
{
    for (ULONG i = 0; i < bytes / 4; i++)
        p[i] = value;
}

ULONG WINAPI xboxkrnl_RtlCompareMemoryUlong(const ULONG *p, ULONG bytes, ULONG value)
{
    ULONG i = 0;
    while (i < bytes / 4 && p[i] == value)
        i++;
    return i * 4;
}

typedef struct { unsigned short length, maximum; char *buffer; } ANSI_STRING;

void WINAPI xboxkrnl_RtlInitAnsiString(ANSI_STRING *s, const char *source)
{
    unsigned short n = 0;
    if (source)
        while (source[n])
            n++;
    s->length = n;
    s->maximum = source ? n + 1 : 0;
    s->buffer = (char *)source;
}

ULONG WINAPI xboxkrnl_RtlNtStatusToDosError(NTSTATUS status) { return status == STATUS_SUCCESS ? 0 : 31; }

/* --- time, threads --- */
void WINAPI xboxkrnl_KeQuerySystemTime(void *time) { GetSystemTimeAsFileTime(time); }

NTSTATUS WINAPI xboxkrnl_KeDelayExecutionThread(char mode, char alertable, const long long *interval)
{
    (void)mode, (void)alertable;
    long long t = *interval;  /* negative: relative, in 100 ns units */
    Sleep(t < 0 ? (ULONG)(-t / 10000) : 0);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI xboxkrnl_NtYieldExecution(void) { SwitchToThread(); return STATUS_SUCCESS; }
NTSTATUS WINAPI xboxkrnl_NtClose(void *h) { CloseHandle(h); return STATUS_SUCCESS; }

/* --- devices and drives: XapiInitProcess opens the Xbox's devices and links its drive letters (D:, T:, Z:, ...) to
 * them. Files are opened on Windows paths by the port (port/xapi.c), so these just succeed. --- */
#define FAKE_DEVICE_HANDLE ((void *)0x0dead000)
typedef struct { NTSTATUS status; ULONG information; } IO_STATUS_BLOCK;

NTSTATUS WINAPI xboxkrnl_NtOpenFile(void **handle, ULONG access, void *attributes, IO_STATUS_BLOCK *io, ULONG share,
                                    ULONG options)
{
    (void)access, (void)attributes, (void)share, (void)options;
    *handle = FAKE_DEVICE_HANDLE;
    if (io)
        io->status = STATUS_SUCCESS, io->information = 1;  /* FILE_OPENED */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI xboxkrnl_IoCreateSymbolicLink(void *link, void *device) { (void)link, (void)device; return 0; }
NTSTATUS WINAPI xboxkrnl_IoDeleteSymbolicLink(void *link) { (void)link; return 0; }
void WINAPI xboxkrnl_HalRegisterShutdownNotification(void *registration, char add) { (void)registration, (void)add; }

NTSTATUS WINAPI xboxkrnl_NtQueryVolumeInformationFile(void *h, IO_STATUS_BLOCK *io, void *info, ULONG length,
                                                      ULONG info_class)
{
    (void)h, (void)info_class;
    for (ULONG i = 0; i < length; i++)
        ((UCHAR *)info)[i] = 0;
    if (io)
        io->status = STATUS_SUCCESS, io->information = length;
    return STATUS_SUCCESS;
}

/* the console's settings (language, video, audio, ...): all 0, the defaults */
NTSTATUS WINAPI xboxkrnl_ExQueryNonVolatileSetting(ULONG index, ULONG *type, void *value, ULONG length, ULONG *result)
{
    (void)index;
    if (type)
        *type = 4;  /* REG_DWORD */
    for (ULONG i = 0; i < length; i++)
        ((UCHAR *)value)[i] = 0;
    if (result)
        *result = length < 4 ? length : 4;
    return STATUS_SUCCESS;
}

/* --- kernel timers and DPCs: XAPI's background timers (auto power-down) never need to fire here --- */
void WINAPI xboxkrnl_KeInitializeDpc(void *dpc, void *routine, void *context) { (void)dpc, (void)routine, (void)context; }
void WINAPI xboxkrnl_KeInitializeTimerEx(void *timer, ULONG type) { (void)timer, (void)type; }
UCHAR WINAPI xboxkrnl_KeSetTimer(void *timer, long long due, void *dpc) { (void)timer, (void)due, (void)dpc; return 0; }
UCHAR WINAPI xboxkrnl_KeCancelTimer(void *timer) { (void)timer; return 0; }
UCHAR WINAPI xboxkrnl_KeInsertQueueDpc(void *dpc, void *a, void *b) { (void)dpc, (void)a, (void)b; return 1; }
UCHAR WINAPI xboxkrnl_KeRemoveQueueDpc(void *dpc) { (void)dpc; return 1; }

/* --- data the kernel exports: the import slot holds the variable's address --- */
struct { unsigned short major, minor, build, qfe; } xboxkrnl_XboxKrnlVersion = { 1, 0, 5838, 1 };
struct { ULONG flags; UCHAR gpu, mcp, unknown[2]; } xboxkrnl_XboxHardwareInfo;
void *xboxkrnl_LaunchDataPage;
UCHAR xboxkrnl_XboxHDKey[16], xboxkrnl_XboxSignatureKey[16], xboxkrnl_XboxAlternateSignatureKeys[16][16];
UCHAR xboxkrnl_XePublicKeyData[284];
ANSI_STRING xboxkrnl_XeImageFileName = { 26, 27, "\\Device\\CdRom0\\default.xbe" };
volatile ULONG xboxkrnl_KeTickCount;  /* milliseconds since start; XAPI's GetTickCount reads it */
ULONG xboxkrnl_KeTimeIncrement = 10000;  /* 1 ms in 100 ns units */

__declspec(dllimport) ULONG WINAPI GetTickCount(void);
__declspec(dllimport) void *WINAPI CreateThread(void *security, ULONG stack, ULONG(WINAPI *start)(void *), void *param,
                                                ULONG flags, ULONG *id);
static ULONG WINAPI tick_thread(void *param)
{
    (void)param;
    ULONG start = GetTickCount();
    for (;;) {
        xboxkrnl_KeTickCount = GetTickCount() - start;
        Sleep(1);
    }
}

/* started by port/native.c before the game's startup */
void native_kernel_start(void) { CreateThread(0, 0, tick_thread, 0, 0, 0); }

/* --- exceptions, debugging, shutdown --- */
void WINAPI xboxkrnl_RtlUnwind(void *frame, void *target, void *record, void *value)
{
    RtlUnwind(frame, target, record, value);
}

void WINAPI xboxkrnl_RtlRaiseException(const ULONG *record)
{
    /* EXCEPTION_RECORD: code, flags, record, address, parameter count, parameters */
    RaiseException(record[0], record[1], record[4], record + 5);
}

/* cdecl, like the kernel's */
ULONG xboxkrnl_DbgPrint(const char *format, ...)
{
    printf("[xbox] ");
    return (ULONG)vprintf(format, (char *)(&format + 1));
}

void WINAPI xboxkrnl_KeBugCheck(ULONG code)
{
    printf("[native] KeBugCheck(%#lx)\n", code);
    abort();
}

void port_print_chain(const char *tag);
void WINAPI xboxkrnl_HalReturnToFirmware(ULONG how)
{
    printf("[native] HalReturnToFirmware(%lu): the game asked to leave (reboot or dashboard)\n", how);
    port_print_chain("fatal");
    ExitProcess(0);
}
