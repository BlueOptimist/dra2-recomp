/* dra2_native.exe: holds the Xbox image's address range for the native build. Windows maps a process's .exe before
 * anything else (thread stacks, heaps, mapped system files soon take the low addresses), so this .exe is linked at
 * 0x10000 with a placeholder section running past the end of the game's image (0x331aa0); dra2_native.dll then copies
 * default.xbe over it, this code included, so native_main never returns here. No C runtime: the DLL has its own. */
__attribute__((section(".xbox"))) char xbox_image_placeholder[0x322000] = { 1 };

__declspec(dllimport) void *__stdcall LoadLibraryA(const char *name);
__declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
__declspec(dllimport) void __stdcall ExitProcess(unsigned code);

void start(void)
{
    void *dll = LoadLibraryA("dra2_native.dll");
    void (*native_main)(void) = dll ? (void (*)(void))GetProcAddress(dll, "native_main") : 0;
    if (!native_main)
        ExitProcess(2);
    native_main();
    ExitProcess(xbox_image_placeholder[0]);  /* not reached; keeps the placeholder from being discarded */
}
