/* XAPI (the Xbox system library) on Win32.
 *
 * Each function keeps the name, calling convention and parameter count of the prototype the game code is compiled
 * against (build/program.h), so it replaces the weak stub in xbox_stubs.c at link time. Parameter types are spelled
 * with plain C types of the same size, so this file builds without the generated headers (and in CI).
 *
 * Paths: the game uses Xbox drive letters. d:\ (the disc) maps to the data folder, u:\ (save games), t:\ and z:\
 * (title and cache scratch) map to folders under the save root:
 *   data  = %DRA2_DATA%  or <exe folder>\data
 *   saves = %DRA2_SAVES% or <exe folder>\saves
 */
#include "win32_min.h"

int printf(const char *format, ...);

void port_todo(const char *name);

/* ------------------------------------------------------------------------------------------------ helpers -- */

static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void str_copy(char *dst, unsigned size, const char *src)
{
    unsigned i = 0;
    for (; src[i] && i + 1 < size; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void str_append(char *dst, unsigned size, const char *src)
{
    unsigned n = str_len(dst);
    if (n < size)
        str_copy(dst + n, size - n, src);
}

static void mem_zero(void *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        ((unsigned char *)p)[i] = 0;
}

static void exe_folder(char *out, unsigned size)
{
    unsigned n = (unsigned)GetModuleFileNameA(0, out, size);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
        n--;
    out[n ? n - 1 : 0] = 0;
}

static void root_folder(const char *env, const char *fallback, char *out, unsigned size)
{
    if (GetEnvironmentVariableA(env, out, size) == 0) {
        exe_folder(out, size);
        str_append(out, size, "\\");
        str_append(out, size, fallback);
    }
}

/* create every folder along a path (errors for existing ones are expected) */
static void make_folders(const char *path)
{
    char tmp[MAX_PATH];
    str_copy(tmp, sizeof tmp, path);
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, 0);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, 0);
}

/* Xbox path -> Windows path */
static const char *map_path(const char *xbox, char *out, unsigned size)
{
    char drive = xbox[0] | 0x20;
    if (xbox[0] && xbox[1] == ':') {
        if (drive == 'd') {
            root_folder("DRA2_DATA", "data", out, size);
        } else if (drive == 'u' || drive == 't' || drive == 'z') {
            char sub[3] = { '\\', (char)drive, 0 };
            root_folder("DRA2_SAVES", "saves", out, size);
            str_append(out, size, sub);
            make_folders(out);
        } else {
            str_copy(out, size, xbox);
            return out;
        }
        str_append(out, size, xbox[2] == '\\' || xbox[2] == '/' ? xbox + 2 : "\\");
        if (!(xbox[2] == '\\' || xbox[2] == '/'))
            str_append(out, size, xbox + 2);
    } else {
        str_copy(out, size, xbox);
    }
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    return out;
}

/* ------------------------------------------------------------------------------------ kernel objects, time -- */

unsigned char __stdcall XAPILIB__CloseHandle(void *h) { return CloseHandle(h) != 0; }

void *__stdcall XAPILIB__CreateEventA(void *security, int manual_reset, int initial, const char *name)
{
    return CreateEventA(security, manual_reset, initial, name);
}

void *__stdcall XAPILIB__CreateMutexA(void *security, int owned, const char *name)
{
    return CreateMutexA(security, owned, name);
}

void *__stdcall XAPILIB__CreateThread(void *security, DWORD stack, LPTHREAD_START_ROUTINE start, void *param,
                                      DWORD flags, DWORD *id)
{
    return CreateThread(security, stack, start, param, flags, id);
}

unsigned char __stdcall XAPILIB__SetEvent(void *h) { return SetEvent(h) != 0; }
unsigned char __stdcall XAPILIB__ReleaseMutex(void *h) { return ReleaseMutex(h) != 0; }
DWORD __stdcall XAPILIB__WaitForSingleObject(void *h, DWORD ms) { return WaitForSingleObject(h, ms); }

DWORD __stdcall XAPILIB__SignalObjectAndWait(void *signal, void *wait, DWORD ms, int alertable)
{
    return SignalObjectAndWait(signal, wait, ms, alertable);
}

void __stdcall XAPILIB__Sleep(DWORD ms)
{
    /* The frame limiter sleeps for the rest of the frame: more than a second means its time math went wrong */
    if (ms > 1000) {
        static int reported;
        if (reported++ < 4)
            printf("[xapi] Sleep(%lu ms) requested from %p: shortened to 16 ms\n", ms, __builtin_return_address(0));
        ms = 16;
    }
    Sleep(ms);
}
DWORD __stdcall XAPILIB__GetLastError(void) { return GetLastError(); }
int __stdcall XAPILIB__QueryPerformanceCounter(LONGLONG *count) { return QueryPerformanceCounter(count); }
int __stdcall XAPILIB__QueryPerformanceFrequency(LONGLONG *freq) { return QueryPerformanceFrequency(freq); }

/* ------------------------------------------------------------------------------------------------- memory -- */

/* MEMORYSTATUS has the same layout on Xbox and 32-bit Windows */
void __stdcall XAPILIB__GlobalMemoryStatus(MEMORYSTATUS *status) { GlobalMemoryStatus(status); }

/* FUN_001294c8: GetProcessHeap (returns XAPI's heap handle) */
void *__stdcall FUN_001294c8(void) { return GetProcessHeap(); }

/* Every block gets 16 bytes of slack after it. With DRA2_HEAPCHECK=1 they are guard bytes, and each block is tracked
 * with the game function that asked for it. Guards are checked when a block is freed and, every 256 heap calls, across all live blocks: the first one
 * found overwritten is reported ("[heap] ..." with the allocating call site, for tools/symbolize.py). Windows only
 * notices heap damage much later ("heap corruption" inside some unrelated call). */
#define GUARD 16
#define GUARD_BYTE 0xFD
#define TRACKED (1 << 18)
typedef struct { unsigned char *p; DWORD size; void *site[3]; void *freed_at[2]; } Tracked;
#define FREED_BYTE 0xDD
static Tracked tracked[TRACKED];
static int heapcheck = -1, heap_ops, guard_reported;

static int heapcheck_on(void)
{
    if (heapcheck < 0) {
        char v[4];
        heapcheck = GetEnvironmentVariableA("DRA2_HEAPCHECK", v, sizeof v) > 0;
    }
    return heapcheck;
}

static Tracked *slot_of(void *p, int insert)
{
    unsigned h = ((unsigned)p >> 3) * 2654435761u >> 14;
    for (unsigned k = 0; k < TRACKED; k++) {
        Tracked *t = &tracked[(h + k) & (TRACKED - 1)];
        if (t->p == p)
            return t;
        if (!t->p)
            return insert ? t : 0;
    }
    return 0;
}

static int guard_ok(const Tracked *t)
{
    for (int i = 0; i < GUARD; i++)
        if (t->p[t->size + i] != GUARD_BYTE)
            return 0;
    return 1;
}

static void report_guard(const Tracked *t, const char *when)
{
    if (guard_reported++ < 4)
        printf("[heap] block %p (%lu bytes) written past its end, found %s\n[crash] frame %08lx\n"
               "[crash] frame %08lx\n[crash] frame %08lx\n", (void *)t->p, (unsigned long)t->size, when,
               (unsigned long)t->site[0], (unsigned long)t->site[1], (unsigned long)t->site[2]);
}

/* a freed block (kept, not returned to Windows, while checking) that something wrote to afterwards */
static void check_freed(Tracked *t)
{
    for (DWORD i = 0; i < t->size; i++)
        if (t->p[i] != FREED_BYTE) {
            if (guard_reported++ < 4)
                printf("[heap] block %p (%lu bytes) written at +%lu after it was freed\n[crash] frame %08lx\n"
                       "[crash] frame %08lx\n[crash] frame %08lx\n[heap] freed from\n[crash] frame %08lx\n"
                       "[crash] frame %08lx\n", (void *)t->p, (unsigned long)t->size, (unsigned long)i,
                       (unsigned long)t->site[0], (unsigned long)t->site[1], (unsigned long)t->site[2],
                       (unsigned long)t->freed_at[0], (unsigned long)t->freed_at[1]);
            for (DWORD j = 0; j < t->size; j++)
                t->p[j] = FREED_BYTE;  /* report each block once per write */
            return;
        }
}

static void check_all(void)
{
    for (unsigned k = 0; k < TRACKED; k++) {
        Tracked *t = &tracked[k];
        if (!t->p || t->p == (unsigned char *)1)
            continue;
        if (!guard_ok(t))
            report_guard(t, "in a sweep");
        if (t->freed_at[0])
            check_freed(t);
    }
}

/* FUN_0012ae24: RtlAllocateHeap(heap, flags, size), reached through LocalAlloc */
void *__stdcall FUN_0012ae24(void *heap, DWORD flags, DWORD size)
{
    /* Always 16 bytes of slack: the game writes a little past some blocks, which the Xbox heap's rounding absorbed
     * and Windows' heap reports as corruption much later (inside CreateDirectoryA). */
    if (!heapcheck_on())
        return HeapAlloc(heap, flags, size + GUARD);
    unsigned char *p = HeapAlloc(heap, flags, size + GUARD);
    if (p) {
        for (int i = 0; i < GUARD; i++)
            p[size + i] = GUARD_BYTE;
        Tracked *t = slot_of(p, 1);
        if (t) {
            t->p = p;
            t->size = size;
            /* the allocating game code, up through LocalAlloc / malloc / operator new (frames exist at -O0) */
            t->site[0] = __builtin_return_address(1);
            t->site[1] = __builtin_return_address(2);
            t->site[2] = __builtin_return_address(3);
        }
    }
    if (++heap_ops % 256 == 0)
        check_all();
    return p;
}

/* A pointer the heap doesn't own (another allocator's memory, a block freed twice) makes Windows stop the process
 * with "heap corruption"; it is reported and skipped instead, so the bad free can be found. */
WINIMPORT BOOL WINAPI HeapValidate(HANDLE heap, DWORD flags, const void *mem);

unsigned char __stdcall XAPILIB__HeapFree(void *heap, DWORD flags, void *mem)
{
    if (mem && !HeapValidate(heap, 0, mem)) {
        static int reported;
        if (reported++ < 8)
            printf("[xapi] HeapFree(%p): not a live block of this heap, skipped\n", mem);
        return 0;
    }
    if (mem && heapcheck_on()) {
        Tracked *t = slot_of(mem, 0);
        if (t && t->freed_at[0]) {
            static int twice;
            if (twice++ < 4)
                printf("[heap] block %p freed twice\n", mem);
            return 1;
        }
        if (t) {
            if (!guard_ok(t))
                report_guard(t, "when it was freed");
            /* kept in quarantine, filled, so a later write to it is caught (never returned to Windows) */
            for (DWORD i = 0; i < t->size; i++)
                t->p[i] = FREED_BYTE;
            t->freed_at[0] = __builtin_return_address(1);
            t->freed_at[1] = __builtin_return_address(2);
        }
        if (++heap_ops % 256 == 0)
            check_all();
        if (t)
            return 1;
    }
    return HeapFree(heap, flags, mem) != 0;
}

/* Contiguous physical memory for the GPU. On Windows plain committed memory does; VirtualAlloc's 64 KB alignment
 * covers every alignment the XDK allows. */
void *__stdcall XAPILIB__XPhysicalAlloc(DWORD size, DWORD max_address, DWORD alignment, DWORD protect)
{
    (void)max_address, (void)alignment, (void)protect;
    return VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

/* cdecl: that's how the game's prototype declares it (the original is __stdcall; either way one argument) */
void XAPILIB__XPhysicalFree(void *address) { VirtualFree(address, 0, MEM_RELEASE); }

/* ------------------------------------------------------------------------------------------------- files -- */

#define FILE_FLAG_NO_BUFFERING 0x20000000u

/* FUN_0012d671: CreateFileA. The streaming reader opens files unbuffered + overlapped; unbuffered I/O on Windows
 * needs buffers and offsets aligned to the drive's sector size (up to 4 KB), stricter than the DVD's 2 KB, so it
 * goes (overlapped stays: the game polls the reads). */
void *__stdcall FUN_0012d671(const char *name, DWORD access, DWORD share, void *security, DWORD disposition,
                             DWORD flags, void *templ)
{
    char path[MAX_PATH];
    HANDLE h = CreateFileA(map_path(name, path, sizeof path), access, share, security, disposition,
                           flags & ~FILE_FLAG_NO_BUFFERING, templ);
    if (h == INVALID_HANDLE_VALUE && disposition == 3 /* OPEN_EXISTING */) {
        DWORD error = GetLastError();
        printf("[xapi] can't open %s (%s): error %lu\n", name, path, error);
        SetLastError(error);
    }
    return h;
}

int __stdcall XAPILIB__ReadFile(void *h, void *buffer, DWORD size, DWORD *read, void *overlapped)
{
    int ok = ReadFile(h, buffer, size, read, overlapped);
    if (!ok) {
        DWORD error = GetLastError();
        static int reported;
        if (error != ERROR_IO_PENDING && error != 38 /* ERROR_HANDLE_EOF */ && reported++ < 5)
            printf("[xapi] ReadFile(%p, %lu bytes) failed: error %lu\n", buffer, (unsigned long)size, error);
        SetLastError(error);
    }
    return ok;
}

int __stdcall XAPILIB__WriteFile(void *h, const void *buffer, DWORD size, DWORD *written, void *overlapped)
{
    return WriteFile(h, buffer, size, written, overlapped);
}

DWORD __stdcall XAPILIB__GetFileSize(void *h, DWORD *high) { return GetFileSize(h, high); }

int __stdcall XAPILIB__GetOverlappedResult(void *h, void *overlapped, DWORD *transferred, int wait)
{
    return GetOverlappedResult(h, overlapped, transferred, wait);
}

/* FUN_0012dade: CopyFileA */
int __stdcall FUN_0012dade(const char *from, const char *to, int fail_if_exists)
{
    char a[MAX_PATH], b[MAX_PATH];
    return CopyFileA(map_path(from, a, sizeof a), map_path(to, b, sizeof b), fail_if_exists);
}

/* FUN_0012f09a: GetFileAttributesA */
DWORD __stdcall FUN_0012f09a(const char *name)
{
    char path[MAX_PATH];
    return GetFileAttributesA(map_path(name, path, sizeof path));
}

/* FUN_0012f1fd: DeleteFileA */
unsigned char __stdcall FUN_0012f1fd(const char *name)
{
    char path[MAX_PATH];
    return DeleteFileA(map_path(name, path, sizeof path)) != 0;
}

/* 0x1eb830 (NtCreateFile with FILE_DIRECTORY_FILE + FILE_CREATE): CreateDirectoryA */
int __stdcall XAPI_CreateDirectoryA(const char *name, void *security)
{
    char path[MAX_PATH];
    return CreateDirectoryA(map_path(name, path, sizeof path), security);
}

/* FUN_00125782: GetDiskFreeSpaceExA */
int __stdcall FUN_00125782(const char *dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free_)
{
    char path[MAX_PATH];
    return GetDiskFreeSpaceExA(map_path(dir, path, sizeof path), avail, total, free_);
}

/* --------------------------------------------------------------------------------------------- save games -- */
/* A save game is a folder under u:\ named after a hash of its display name; the name itself is kept in
 * <folder>\savename.bin (UTF-16) so XFindFirstSaveGame can report it. */

#define MAX_GAMENAME 128

typedef struct {
    WIN32_FIND_DATAA find;
    char szSaveGameDirectory[MAX_PATH];
    unsigned short szSaveGameName[MAX_GAMENAME];
} XGAME_FIND_DATA;

static void save_folder_name(const unsigned short *name, char out[13])
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *name; name++)
        h = (h ^ *name) * 1099511628211ULL;
    for (int i = 11; i >= 0; i--, h >>= 4)
        out[i] = "0123456789ABCDEF"[h & 15];
    out[12] = 0;
}

DWORD __stdcall XAPILIB__XCreateSaveGame(const char *root, const unsigned short *name, DWORD disposition,
                                         DWORD flags, char *path_out, DWORD path_size)
{
    char folder[13], xbox[MAX_PATH], win[MAX_PATH];
    (void)flags;
    save_folder_name(name, folder);
    str_copy(xbox, sizeof xbox, root);
    str_append(xbox, sizeof xbox, folder);
    map_path(xbox, win, sizeof win);
    int exists = GetFileAttributesA(win) != INVALID_FILE_ATTRIBUTES;
    if (!exists && disposition == 3 /* OPEN_EXISTING */)
        return ERROR_PATH_NOT_FOUND;
    if (exists && disposition == 1 /* CREATE_NEW */)
        return ERROR_ALREADY_EXISTS;
    if (!exists) {
        make_folders(win);
        char meta[MAX_PATH];
        str_copy(meta, sizeof meta, win);
        str_append(meta, sizeof meta, "\\savename.bin");
        void *f = CreateFileA(meta, 0x40000000 /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n = 0, written;
            while (name[n])
                n++;
            WriteFile(f, name, (n + 1) * 2, &written, 0);
            CloseHandle(f);
        }
    }
    str_append(xbox, sizeof xbox, "\\");
    str_copy(path_out, path_size, xbox);
    return ERROR_SUCCESS;
}

DWORD __stdcall XAPILIB__XDeleteSaveGame(const char *root, const unsigned short *name)
{
    char folder[13], xbox[MAX_PATH], win[MAX_PATH], pattern[MAX_PATH], file[MAX_PATH];
    WIN32_FIND_DATAA fd;
    save_folder_name(name, folder);
    str_copy(xbox, sizeof xbox, root);
    str_append(xbox, sizeof xbox, folder);
    map_path(xbox, win, sizeof win);
    str_copy(pattern, sizeof pattern, win);
    str_append(pattern, sizeof pattern, "\\*");
    void *h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            str_copy(file, sizeof file, win);
            str_append(file, sizeof file, "\\");
            str_append(file, sizeof file, fd.cFileName);
            DeleteFileA(file);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(win) ? ERROR_SUCCESS : GetLastError();
}

typedef struct {
    void *find;
    char root[8];  /* the Xbox root the search was started with, e.g. "u:\" */
    char win_root[MAX_PATH];
} save_search;

static save_search searches[4];

/* fill an XGAME_FIND_DATA from a found entry; 0 if the entry isn't a save folder */
static int fill_save(save_search *s, XGAME_FIND_DATA *out)
{
    if (!(out->find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || out->find.cFileName[0] == '.')
        return 0;
    str_copy(out->szSaveGameDirectory, MAX_PATH, s->root);
    str_append(out->szSaveGameDirectory, MAX_PATH, out->find.cFileName);
    str_append(out->szSaveGameDirectory, MAX_PATH, "\\");
    mem_zero(out->szSaveGameName, sizeof out->szSaveGameName);
    char meta[MAX_PATH];
    str_copy(meta, sizeof meta, s->win_root);
    str_append(meta, sizeof meta, "\\");
    str_append(meta, sizeof meta, out->find.cFileName);
    str_append(meta, sizeof meta, "\\savename.bin");
    void *f = CreateFileA(meta, 0x80000000 /* GENERIC_READ */, 1, 0, 3, 0x80, 0);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD read;
        ReadFile(f, out->szSaveGameName, sizeof out->szSaveGameName - 2, &read, 0);
        CloseHandle(f);
    }
    return 1;
}

void *__stdcall XAPILIB__XFindFirstSaveGame(const char *root, XGAME_FIND_DATA *data)
{
    save_search *s = 0;
    for (int i = 0; i < 4; i++)
        if (!searches[i].find)
            s = &searches[i];
    if (!s)
        return INVALID_HANDLE_VALUE;
    char pattern[MAX_PATH];
    str_copy(s->root, sizeof s->root, root);
    map_path(root, s->win_root, sizeof s->win_root);
    if (s->win_root[str_len(s->win_root) - 1] == '\\')
        s->win_root[str_len(s->win_root) - 1] = 0;
    str_copy(pattern, sizeof pattern, s->win_root);
    str_append(pattern, sizeof pattern, "\\*");
    s->find = FindFirstFileA(pattern, &data->find);
    if (s->find == INVALID_HANDLE_VALUE) {
        s->find = 0;
        return INVALID_HANDLE_VALUE;
    }
    while (!fill_save(s, data)) {
        if (!FindNextFileA(s->find, &data->find)) {
            FindClose(s->find);
            s->find = 0;
            SetLastError(ERROR_NO_MORE_FILES);
            return INVALID_HANDLE_VALUE;
        }
    }
    return s;
}

int __stdcall XAPILIB__XFindNextSaveGame(save_search *s, XGAME_FIND_DATA *data)
{
    do {
        if (!FindNextFileA(s->find, &data->find)) {
            SetLastError(ERROR_NO_MORE_FILES);
            return 0;
        }
    } while (!fill_save(s, data));
    return 1;
}

unsigned char __stdcall XAPILIB__XFindClose(save_search *s)
{
    if (!s || s == INVALID_HANDLE_VALUE || !s->find)
        return 0;
    FindClose(s->find);
    s->find = 0;
    return 1;
}

/* Save-game signatures. The Xbox signs saves with a per-console key; here any stable 20-byte digest will do, since
 * the PC version only has to verify its own saves. Five FNV-1a lanes with different seeds. */
typedef struct { unsigned lanes[5]; int used; } signature_ctx;
static signature_ctx signatures[4];

signature_ctx *__stdcall XAPILIB__XCalculateSignatureBegin(DWORD flags)
{
    (void)flags;
    for (int i = 0; i < 4; i++) {
        if (!signatures[i].used) {
            signatures[i].used = 1;
            for (int k = 0; k < 5; k++)
                signatures[i].lanes[k] = 2166136261u ^ (0x9e3779b9u * (unsigned)(k + 1));
            return &signatures[i];
        }
    }
    return INVALID_HANDLE_VALUE;
}

DWORD __stdcall XAPILIB__XCalculateSignatureUpdate(signature_ctx *ctx, const unsigned char *data, DWORD size)
{
    for (DWORD i = 0; i < size; i++)
        for (int k = 0; k < 5; k++)
            ctx->lanes[k] = (ctx->lanes[k] ^ (data[i] + (unsigned)k)) * 16777619u;
    return ERROR_SUCCESS;
}

DWORD __stdcall XAPILIB__XCalculateSignatureEnd(signature_ctx *ctx, unsigned char *signature)
{
    for (int k = 0; k < 5; k++)
        for (int b = 0; b < 4; b++)
            signature[k * 4 + b] = (unsigned char)(ctx->lanes[k] >> (8 * b));
    ctx->used = 0;
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------------------------- devices and gamepads -- */
/* Windows XInput (xinput1_4.dll, else xinput9_1_0.dll), loaded on first use. Xbox gamepads have analog face
 * buttons and Black/White; they map to the Windows pad's buttons (255 when pressed) and shoulder buttons. */

/* the device-type descriptors the game passes to XGetDevices / XInputOpen (compared by address) */
unsigned char XAPILIB__g_DeviceType_Gamepad[64];
unsigned char XAPILIB__g_DeviceType_MU[64];

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; short sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    WIN_GAMEPAD;
typedef struct { DWORD dwPacketNumber; WIN_GAMEPAD Gamepad; } WIN_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } WIN_VIBRATION;

typedef struct { WORD wButtons; BYTE bAnalogButtons[8]; short sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    XBOX_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XBOX_GAMEPAD Gamepad; } XBOX_STATE;

static DWORD(WINAPI *win_get_state)(DWORD, WIN_STATE *);
static DWORD(WINAPI *win_set_state)(DWORD, WIN_VIBRATION *);

static int xinput_ready(void)
{
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m)
            m = LoadLibraryA("xinput9_1_0.dll");
        if (m) {
            win_get_state = (DWORD(WINAPI *)(DWORD, WIN_STATE *))GetProcAddress(m, "XInputGetState");
            win_set_state = (DWORD(WINAPI *)(DWORD, WIN_VIBRATION *))GetProcAddress(m, "XInputSetState");
        }
    }
    return win_get_state != 0;
}

/* XInputGetState on an empty slot is slow, and the game asks for device changes every tick: presence is re-polled
 * at most twice a second */
static DWORD connected_pads(void)
{
    static DWORD mask;
    static LONGLONG next_poll, frequency;
    LONGLONG now;
    if (!frequency)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    if (now < next_poll)
        return mask;
    next_poll = now + frequency / 2;
    WIN_STATE st;
    mask = 0;
    if (xinput_ready())
        for (DWORD i = 0; i < 4; i++)
            if (win_get_state(i, &st) == ERROR_SUCCESS)
                mask |= 1u << i;
    return mask;
}

static DWORD last_pads;

void __stdcall XAPILIB__XInitDevices(DWORD count, void *types) { (void)count, (void)types; xinput_ready(); }

DWORD __stdcall XAPILIB__XGetDevices(void *type)
{
    if (type != XAPILIB__g_DeviceType_Gamepad)
        return 0;  /* no memory units: saves go to u:\ */
    return last_pads = connected_pads();
}

unsigned char __stdcall XAPILIB__XGetDeviceChanges(void *type, DWORD *inserted, DWORD *removed)
{
    *inserted = *removed = 0;
    if (type != XAPILIB__g_DeviceType_Gamepad)
        return 0;
    DWORD now = connected_pads();
    *inserted = now & ~last_pads;
    *removed = last_pads & ~now;
    last_pads = now;
    return (*inserted | *removed) != 0;
}

/* the handle is the port number + 1 */
void *__stdcall XAPILIB__XInputOpen(void *type, DWORD port, DWORD slot, void *polling)
{
    (void)slot, (void)polling;
    if (type != XAPILIB__g_DeviceType_Gamepad || port > 3 || !(connected_pads() & (1u << port)))
        return 0;
    return (void *)(port + 1);
}

void __stdcall XAPILIB__XInputClose(void *handle) { (void)handle; }

DWORD __stdcall XAPILIB__XInputGetState(void *handle, XBOX_STATE *out)
{
    WIN_STATE st;
    DWORD port = (DWORD)handle - 1;
    if (!xinput_ready() || port > 3 || win_get_state(port, &st) != ERROR_SUCCESS)
        return ERROR_DEVICE_NOT_CONNECTED;
    WORD b = st.Gamepad.wButtons;
    out->dwPacketNumber = st.dwPacketNumber;
    out->Gamepad.wButtons = b & 0x00FF;  /* d-pad, start, back, thumb clicks: same bits on both */
    out->Gamepad.bAnalogButtons[0] = (b & 0x1000) ? 255 : 0;  /* A */
    out->Gamepad.bAnalogButtons[1] = (b & 0x2000) ? 255 : 0;  /* B */
    out->Gamepad.bAnalogButtons[2] = (b & 0x4000) ? 255 : 0;  /* X */
    out->Gamepad.bAnalogButtons[3] = (b & 0x8000) ? 255 : 0;  /* Y */
    out->Gamepad.bAnalogButtons[4] = (b & 0x0200) ? 255 : 0;  /* Black <- right shoulder */
    out->Gamepad.bAnalogButtons[5] = (b & 0x0100) ? 255 : 0;  /* White <- left shoulder */
    out->Gamepad.bAnalogButtons[6] = st.Gamepad.bLeftTrigger;
    out->Gamepad.bAnalogButtons[7] = st.Gamepad.bRightTrigger;
    out->Gamepad.sThumbLX = st.Gamepad.sThumbLX;
    out->Gamepad.sThumbLY = st.Gamepad.sThumbLY;
    out->Gamepad.sThumbRX = st.Gamepad.sThumbRX;
    out->Gamepad.sThumbRY = st.Gamepad.sThumbRY;
    return ERROR_SUCCESS;
}

/* XINPUT_FEEDBACK: a 66-byte header (dwStatus first), then the left and right motor speeds; the game writes them at
 * +0x42 and +0x44. The Xbox completes asynchronously; here it is done at once. */
DWORD __stdcall XAPILIB__XInputSetState(void *handle, unsigned char *feedback)
{
    DWORD port = (DWORD)handle - 1;
    *(DWORD *)feedback = ERROR_SUCCESS;
    if (!xinput_ready() || !win_set_state || port > 3)
        return ERROR_DEVICE_NOT_CONNECTED;
    WIN_VIBRATION v = { *(WORD *)(feedback + 0x42), *(WORD *)(feedback + 0x44) };
    win_set_state(port, &v);
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- settings -- */

DWORD __stdcall XAPILIB__XGetLanguage(void) { return 1; }       /* XC_LANGUAGE_ENGLISH */
DWORD __stdcall XAPILIB__XGetVideoStandard(void) { return 1; }  /* XC_VIDEO_STANDARD_NTSC_M */
DWORD __stdcall XAPILIB__XGetVideoFlags(void) { return 0; }     /* 4:3, no HDTV modes */

/* used to return to the dashboard or reboot the title: on PC that's quitting */
void __stdcall XAPILIB__XLaunchNewImageA(const char *image, void *launch_data)
{
    (void)image, (void)launch_data;
    ExitProcess(0);
}
