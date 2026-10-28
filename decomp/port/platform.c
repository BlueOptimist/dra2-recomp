/* Core of the Windows platform layer: shared helpers for the code that replaces the Xbox system libraries.
 * Only the C runtime is assumed (no Windows SDK headers yet).
 */
int printf(const char *format, ...);
int strcmp(const char *a, const char *b);
void abort(void);

/* Every entry of the Xbox kernel import table (start of .rdata) points at a generated stub that calls this with the
 * import's name (tools/gen_data.py). The game itself never calls the kernel: only the Xbox libraries do, and the port
 * replaces those, so reaching this means a library path the port still runs by mistake. */
void port_kernel_unimplemented(const char *name)
{
    printf("[port] Xbox kernel import called: %s (no Windows implementation)\n", name);
    abort();
}

/* 0x1d4ba0, the body of FatalErrorf (format, va_list): the engine's fatal-error report. The Xbox version formats the
 * message, calls an optional hook, appends it to t:\crashlog.txt and breaks into the debugger. Its decompiled body
 * doesn't compile yet (the hook is an untyped global), so this replaces the weak trap stub. */
int vprintf(const char *format, char *args);

void FUN_001d4ba0(char *format, char *args)
{
    printf("[game] FATAL ERROR: ");
    vprintf(format, args);
    printf("\n");
    abort();
}

/* A code label the game uses as a callback, with no recompiled function at that address (tools/gen_data.py). */
void port_unrecompiled_code(const char *name)
{
    printf("[port] reached code that isn't recompiled yet: %s\n", name);
    abort();
}

/* Report an Xbox system call that has no Windows implementation yet: once per function, so the log shows exactly
 * which parts of the platform layer a run actually needs. */
void port_todo(const char *name)
{
    static const char *seen[512];
    static int count;
    for (int i = 0; i < count; i++)
        if (seen[i] == name || strcmp(seen[i], name) == 0)
            return;
    if (count < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[count++] = name;
    printf("[port] not implemented yet: %s\n", name);
}
