/* Call counts for debugging. With build_objects.py --instrument the game's functions are compiled with
 * -finstrument-functions: each one calls __cyg_profile_func_enter on entry, which counts it here. The sampler in
 * crash.c (DRA2_SAMPLE) then prints the functions that ran since its last sample, most called first ("[calls] fn"
 * lines, named by tools/symbolize.py): what the game is doing frame after frame, branch by branch.
 * Without --instrument nothing calls in and nothing is printed. */

int printf(const char *format, ...);

#define CALL_SLOTS (1 << 16)

typedef struct { void *fn; unsigned count, reported; } CallSlot;
static CallSlot slots[CALL_SLOTS];
static volatile int any_calls;

/* the last function entries (function, call site), for the crash report: what ran just before a crash whose stack
 * chain is lost (inside the C runtime, or after a stack overwrite) */
#define RECENT 48
static void *recent_fn[RECENT], *recent_site[RECENT];
static unsigned recent_next;

/* DRA2_HEAPVALIDATE=<n>: every n function entries, Windows checks the whole process heap (the game's blocks and the
 * C runtime's alike). The first failure reports the window of call numbers it happened in and the last function
 * entries, then stops. DRA2_HEAPVALIDATE_FROM=<call number> checks on every entry from there on, which names the
 * call during which the heap was damaged. */
typedef void *HANDLE_;
__declspec(dllimport) HANDLE_ __stdcall GetProcessHeap(void);
__declspec(dllimport) int __stdcall HeapValidate(HANDLE_ heap, unsigned long flags, const void *mem);
__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char *name, char *value, unsigned long n);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned code);
void port_calls_recent(void);

static int validate_every = -1;
static unsigned validate_from, last_valid;

__attribute__((no_instrument_function)) static unsigned env_number(const char *name)
{
    char v[16];
    unsigned long n = GetEnvironmentVariableA(name, v, sizeof v);
    unsigned x = 0;
    for (unsigned long i = 0; i < n && v[i] >= '0' && v[i] <= '9'; i++)
        x = x * 10 + (unsigned)(v[i] - '0');
    return x;
}

__attribute__((no_instrument_function)) static void validate_heap(unsigned call)
{
    if (validate_every < 0) {
        validate_every = (int)env_number("DRA2_HEAPVALIDATE");
        validate_from = env_number("DRA2_HEAPVALIDATE_FROM");
    }
    if (validate_every <= 0 && !validate_from)
        return;
    int now = (validate_from && call >= validate_from) || (validate_every > 0 && call % validate_every == 0);
    if (!now)
        return;
    if (HeapValidate(GetProcessHeap(), 0, 0)) {
        last_valid = call;
        return;
    }
    printf("[heap] process heap damaged between function entry %u (last valid check) and %u\n", last_valid, call);
    port_calls_recent();
    ExitProcess(3);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    unsigned call = recent_next;
    unsigned r = recent_next++ % RECENT;
    recent_fn[r] = fn;
    recent_site[r] = site;
    validate_heap(call);
    unsigned h = ((unsigned)fn >> 4) * 2654435761u >> 16;
    for (unsigned k = 0; k < CALL_SLOTS; k++) {
        CallSlot *s = &slots[(h + k) & (CALL_SLOTS - 1)];
        if (s->fn == fn) {
            s->count++;
            return;
        }
        if (!s->fn) {
            s->fn = fn;
            s->count = 1;
            any_calls = 1;
            return;
        }
    }
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) { (void)fn, (void)site; }

/* the last function entries, oldest first ("[calls] recent" lines: function, then the call site) */
__attribute__((no_instrument_function)) void port_calls_recent(void)
{
    if (!any_calls)
        return;
    unsigned n = recent_next < RECENT ? recent_next : RECENT;
    for (unsigned k = 0; k < n; k++) {
        unsigned r = (recent_next - n + k) % RECENT;
        printf("[calls] recent %08x from %08x\n", (unsigned)recent_fn[r], (unsigned)recent_site[r]);
    }
}

/* the functions called since the last report, the `limit` most called first */
__attribute__((no_instrument_function)) void port_calls_report(int limit)
{
    if (!any_calls)
        return;
    static CallSlot *changed[CALL_SLOTS];
    static unsigned delta[CALL_SLOTS];
    int n = 0;
    for (unsigned k = 0; k < CALL_SLOTS; k++) {
        CallSlot *s = &slots[k];
        unsigned count = s->count;
        if (s->fn && count != s->reported) {
            changed[n] = s;
            delta[n++] = count - s->reported;
            s->reported = count;
        }
    }
    printf("[calls] %d functions ran since the last sample\n", n);
    for (int i = 0; i < n && i < limit; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (delta[j] > delta[best])
                best = j;
        CallSlot *ts = changed[i];
        changed[i] = changed[best];
        changed[best] = ts;
        unsigned td = delta[i];
        delta[i] = delta[best];
        delta[best] = td;
        printf("[calls] fn %08x x%u\n", (unsigned)changed[i]->fn, delta[i]);
    }
}
