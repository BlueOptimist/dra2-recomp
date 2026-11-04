/* DRA 2.exe: double-click to open the launcher (tools/launcher.ps1) without a console window. Built into the repository
 * root by tools/launcher/build.ps1.
 *   DRA 2.exe             the launcher window
 *   "DRA 2.exe" -Now      start the game straight away (tools/play.ps1 -Now)
 *   "DRA 2.exe" -Console  the text menu (tools/play.ps1), in a console window
 *   "DRA 2.exe" -Extract <image.iso> <folder>
 *                         extract the disc image's game partition into the folder (what tools/xdvdfs_extract.py does),
 *                         so playing needs no Python; progress goes to standard output (the launcher's output box)
 * Otherwise it only starts Windows PowerShell 5.1, which comes with Windows, and does not wait for it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  /* built with -municode, which defines UNICODE */
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void fail(const wchar_t *text)
{
    MessageBoxW(0, text, L"Digimon Rumble Arena 2", MB_OK | MB_ICONERROR);
}

/* ------------------------------------------------------------------------------------------- disc extraction -- */
/* An original Xbox disc's game partition is XDVDFS: 2048-byte sectors; at sector 32 the volume descriptor
 * ("MICROSOFT*XBOX*MEDIA", then the root directory's sector and size). A directory is a binary tree of entries:
 * u16 left, u16 right (in 4-byte units from the table's start, 0 = none), u32 sector, u32 size, u8 attributes,
 * u8 name length, then the name; left 0xFFFF is padding. */
#define SECTOR 2048
#define ATTR_DIRECTORY 0x10
static HANDLE image;
static unsigned long long partition;
static unsigned files, folders;
static unsigned long long bytes;
static unsigned char *buffer;
#define BUFFER_SIZE (8u << 20)

static void say(const char *text)
{
    DWORD n;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE)
        WriteFile(out, text, (DWORD)strlen(text), &n, 0);
}

static int read_at(unsigned long long offset, void *to, DWORD size)
{
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)offset;
    DWORD got;
    return SetFilePointerEx(image, at, 0, FILE_BEGIN) && ReadFile(image, to, size, &got, 0) && got == size;
}

static int extract_file(unsigned long long offset, unsigned long long length, const wchar_t *path)
{
    HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (out == INVALID_HANDLE_VALUE)
        return 0;
    int ok = 1;
    while (ok && length) {
        DWORD chunk = length < BUFFER_SIZE ? (DWORD)length : BUFFER_SIZE, wrote;
        ok = read_at(offset, buffer, chunk) && WriteFile(out, buffer, chunk, &wrote, 0) && wrote == chunk;
        offset += chunk, length -= chunk;
    }
    CloseHandle(out);
    return ok;
}

static int walk_directory(unsigned sector, unsigned size, const wchar_t *folder);

/* one entry of a directory table and, through left and right, the entries below it */
static int visit(const unsigned char *table, unsigned size, unsigned offset, unsigned char *seen, const wchar_t *folder)
{
    if (offset + 14 > size || seen[offset / 4])
        return 1;
    seen[offset / 4] = 1;
    unsigned left = table[offset] | table[offset + 1] << 8, right = table[offset + 2] | table[offset + 3] << 8;
    if (left == 0xFFFF)  /* padding, not an entry */
        return 1;
    unsigned start, length;
    memcpy(&start, table + offset + 4, 4);
    memcpy(&length, table + offset + 8, 4);
    unsigned attributes = table[offset + 12], name_length = table[offset + 13];
    if (offset + 14 + name_length > size)
        return 1;
    if (left && !visit(table, size, left * 4, seen, folder))
        return 0;
    wchar_t name[256], path[1024];
    int n = MultiByteToWideChar(28591, 0, (const char *)table + offset + 14, (int)name_length, name, 255);  /* Latin-1 */
    name[n] = 0;
    _snwprintf(path, 1024, L"%s\\%s", folder, name);
    path[1023] = 0;
    if (attributes & ATTR_DIRECTORY) {
        CreateDirectoryW(path, 0);
        folders++;
        if (!walk_directory(start, length, path))
            return 0;
    } else {
        if (!extract_file(partition + (unsigned long long)start * SECTOR, length, path)) {
            char text[1200];
            _snprintf(text, sizeof text, "Couldn't write %ls (is the disk full?)\r\n", path);
            text[sizeof text - 1] = 0;
            say(text);
            return 0;
        }
        files++, bytes += length;
        if (files % 250 == 0) {
            char text[96];
            _snprintf(text, sizeof text, "  %u files (%.0f MiB)...\r\n", files, bytes / 1048576.0);
            say(text);
        }
    }
    return !right || visit(table, size, right * 4, seen, folder);
}

static int walk_directory(unsigned sector, unsigned size, const wchar_t *folder)
{
    if (!size)
        return 1;
    unsigned char *table = malloc(size), *seen = calloc(size / 4 + 1, 1);
    int ok = table && seen && read_at(partition + (unsigned long long)sector * SECTOR, table, size);
    if (!ok)
        say("The disc image ended early: is it complete?\r\n");
    else
        ok = visit(table, size, 0, seen, folder);
    free(table), free(seen);
    return ok;
}

static int extract(const wchar_t *iso, const wchar_t *folder)
{
    image = CreateFileW(iso, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (image == INVALID_HANDLE_VALUE) {
        say("Can't open the disc image.\r\n");
        return 1;
    }
    /* where the game partition starts: redump-style images (XGD1, 2, 3), or an image of the partition alone */
    static const unsigned long long offsets[] = { 0x18300000, 0xFD90000, 0x2080000, 0 };
    unsigned char descriptor[28];
    int found = 0;
    for (int i = 0; i < 4 && !found; i++) {
        partition = offsets[i];
        found = read_at(partition + 32 * SECTOR, descriptor, sizeof descriptor) &&
                !memcmp(descriptor, "MICROSOFT*XBOX*MEDIA", 20);
    }
    if (!found) {
        say("This isn't an original Xbox disc image (no XDVDFS volume found).\r\n");
        CloseHandle(image);
        return 1;
    }
    unsigned root_sector, root_size;
    memcpy(&root_sector, descriptor + 20, 4);
    memcpy(&root_size, descriptor + 24, 4);
    char text[160];
    _snprintf(text, sizeof text, "Game partition at 0x%llX, root directory at sector %u (%u bytes)\r\n", partition,
              root_sector, root_size);
    text[sizeof text - 1] = 0;
    say(text);
    buffer = malloc(BUFFER_SIZE);
    CreateDirectoryW(folder, 0);
    int ok = buffer && walk_directory(root_sector, root_size, folder);
    CloseHandle(image);
    _snprintf(text, sizeof text, ok ? "Extracted %u files in %u folders (%.1f MiB)\r\n" : "Extraction stopped after %u files.\r\n",
              files, folders, bytes / 1048576.0);
    text[sizeof text - 1] = 0;
    say(text);
    return ok ? 0 : 1;
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
    if (!_wcsnicmp(args, L"-Extract", 8) && (args[8] == L' ' || args[8] == L'\t')) {
        int count;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &count);  /* [0] the program, [1] -Extract */
        if (!argv || count != 4) {
            say("Usage: \"DRA 2.exe\" -Extract <image.iso> <folder>\r\n");
            return 1;
        }
        return extract(argv[2], argv[3]);
    }
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
