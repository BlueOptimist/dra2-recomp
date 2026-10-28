/* Core of the Windows platform layer: shared helpers for the code that replaces the Xbox system libraries.
 * Only the C runtime is assumed (no Windows SDK headers yet).
 */
int printf(const char *format, ...);
int strcmp(const char *a, const char *b);

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
