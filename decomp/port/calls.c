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

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    unsigned r = recent_next++ % RECENT;
    recent_fn[r] = fn;
    recent_site[r] = site;
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
