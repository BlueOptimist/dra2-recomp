/* DRA 2.exe: double-click to open the launcher (tools/launcher.ps1) without a console window. Built into the repository
 * root by tools/launcher/build.ps1.
 *   DRA 2.exe             the launcher window
 *   "DRA 2.exe" -Now      start the game straight away (tools/play.ps1 -Now)
 *   "DRA 2.exe" -Console  the text menu (tools/play.ps1), in a console window
 * It only starts Windows PowerShell 5.1, which comes with Windows, and does not wait for it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  /* built with -municode, which defines UNICODE */
#include <wchar.h>

static void fail(const wchar_t *text)
{
    MessageBoxW(0, text, L"Digimon Rumble Arena 2", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show)
{
    (void)instance, (void)previous, (void)show;
    wchar_t folder[MAX_PATH], script[MAX_PATH], shell[MAX_PATH], command[4 * MAX_PATH];
    /* the folder DRA 2.exe is in: the repository root */
    DWORD n = GetModuleFileNameW(0, folder, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        fail(L"Can't find the folder DRA 2.exe is in.");
        return 1;
    }
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash)
        *slash = 0;

    while (*args == L' ' || *args == L'\t')
        args++;
    for (size_t end = wcslen(args); end && (args[end - 1] == L' ' || args[end - 1] == L'\t'); end--)
        args[end - 1] = 0;  /* "-Now " (a shortcut's arguments can end in a space) */
    int now = !_wcsicmp(args, L"-Now"), console = !_wcsicmp(args, L"-Console");
    if (*args && !now && !console) {
        fail(L"DRA 2.exe takes no options, -Now (start the game at once) or -Console (the text menu).");
        return 1;
    }
    _snwprintf(script, MAX_PATH, L"%s\\tools\\%s", folder, *args ? L"play.ps1" : L"launcher.ps1");
    script[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        wchar_t text[MAX_PATH + 64];
        _snwprintf(text, MAX_PATH + 64, L"Can't find %s.\n\nDRA 2.exe has to stay in the port's folder.", script);
        text[MAX_PATH + 63] = 0;
        fail(text);
        return 1;
    }
    GetSystemDirectoryW(shell, MAX_PATH);
    wcscat(shell, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (*args)
        _snwprintf(command, 4 * MAX_PATH, L"\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\" %s", shell, script, args);
    else
        _snwprintf(command, 4 * MAX_PATH, L"\"%s\" -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File \"%s\"",
                   shell, script);
    command[4 * MAX_PATH - 1] = 0;

    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    /* the launcher window needs no console; the text menu gets one of its own */
    DWORD flags = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (!CreateProcessW(shell, command, 0, 0, FALSE, flags, 0, folder, &si, &pi)) {
        fail(L"Can't start Windows PowerShell, which the launcher runs in.");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
