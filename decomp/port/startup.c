/* The Windows entry point: what the Xbox startup thread (0x126fac) did, minus the XAPI and C runtime initialisation
 * that the Windows C runtime now does itself.
 *
 * On the Xbox, after XAPI and CRT setup, the thread runs three initializer tables (_initterm) and then calls the
 * game's main(0, 0, 0):
 *   0x2bf860-0x2bf86c, 0x2c2fd4-0x2c2fec  the Xbox CRT's own initializers (skipped: the Windows CRT has its own)
 *   0x2bf870-0x2c2fd0                       C++ static constructors and registration functions: 3,534 game entries,
 *                                           plus 9 that point into XDK library code
 * The table lives in the generated .data (tools/gen_data.py), so entries that are game functions now point at the
 * recompiled code; the XDK ones keep their Xbox address and are skipped here.
 */
int printf(const char *format, ...);
typedef struct _iobuf FILE;
FILE *__acrt_iob_func(unsigned index);
int setvbuf(FILE *stream, char *buffer, int mode, unsigned size);
char *getenv(const char *name);

typedef void (*initializer)(void);
extern initializer DAT_002bf870[];  /* first entry of the C++ initializer table */
extern initializer DAT_002c2fd0[];  /* one past the last */

int game_main(int argc, char **argv, char **envp);
void port_install_crash_reporter(void);

/* an address the generated data couldn't relocate still points into the Xbox image (0x10000-0x340000); the .exe is
 * linked at 0x400000 without ASLR, so nothing of ours can be there */
static int is_xbox_address(unsigned address) { return address >= 0x10000 && address < 0x340000; }

/* what startup is doing right now, for the crash reporter (crash.c): no output, so it doesn't disturb the run */
volatile int port_init_index = -1;
volatile unsigned port_init_slot;
volatile int port_in_game_main;

int main(int argc, char **argv)
{
    int ran = 0, skipped = 0;
    /* unbuffered, so the log survives a crash; DRA2_TRACE_INIT=1 prints each initializer before calling it */
    setvbuf(__acrt_iob_func(1), 0, 4 /* _IONBF */, 0);
    port_install_crash_reporter();
    int trace = getenv("DRA2_TRACE_INIT") != 0;
    (void)argc, (void)argv;
    for (initializer *entry = DAT_002bf870; entry < DAT_002c2fd0; entry++) {
        unsigned address = (unsigned)*entry;
        if (address == 0 || address == 0xffffffffu)
            continue;
        if (is_xbox_address(address)) {
            skipped++;
            continue;
        }
        port_init_index = ran;
        port_init_slot = 0x2bf870 + 4 * (unsigned)(entry - DAT_002bf870);
        if (trace)
            printf("[port] init %d: table slot %#x -> %p\n", ran, port_init_slot, (void *)*entry);
        (*entry)();
        ran++;
    }
    printf("[port] static initializers: %d run, %d Xbox-library entries skipped\n", ran, skipped);
    printf("[port] calling game_main\n");
    port_init_index = -1;
    port_in_game_main = 1;
    return game_main(0, 0, 0);
}
