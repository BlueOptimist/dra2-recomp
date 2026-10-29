/* The few Win32 declarations the platform layer uses, written out so the port builds without the Windows SDK
 * headers (only the import libraries are needed at link time). Types and signatures are the documented ones. */
#ifndef DRA2_WIN32_MIN_H
#define DRA2_WIN32_MIN_H

#define WINAPI __stdcall
#define WINIMPORT __declspec(dllimport)

typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long SIZE_T;

#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define MAX_PATH 260
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define HEAP_ZERO_MEMORY 0x08

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NO_MORE_FILES 18
#define ERROR_IO_PENDING 997
#define ERROR_DEVICE_NOT_CONNECTED 1167

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS;

typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(void *param);

WINIMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *security, DWORD disposition,
                                    DWORD flags, HANDLE templ);
WINIMPORT BOOL WINAPI ReadFile(HANDLE h, void *buffer, DWORD size, DWORD *read, void *overlapped);
WINIMPORT BOOL WINAPI WriteFile(HANDLE h, const void *buffer, DWORD size, DWORD *written, void *overlapped);
WINIMPORT DWORD WINAPI GetFileSize(HANDLE h, DWORD *high);
WINIMPORT BOOL WINAPI GetOverlappedResult(HANDLE h, void *overlapped, DWORD *transferred, BOOL wait);
WINIMPORT BOOL WINAPI CloseHandle(HANDLE h);
WINIMPORT BOOL WINAPI CopyFileA(const char *from, const char *to, BOOL fail_if_exists);
WINIMPORT BOOL WINAPI DeleteFileA(const char *name);
WINIMPORT DWORD WINAPI GetFileAttributesA(const char *name);
WINIMPORT BOOL WINAPI CreateDirectoryA(const char *name, void *security);
WINIMPORT BOOL WINAPI RemoveDirectoryA(const char *name);
WINIMPORT BOOL WINAPI GetDiskFreeSpaceExA(const char *dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free_);
WINIMPORT HANDLE WINAPI FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *data);
WINIMPORT BOOL WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data);
WINIMPORT BOOL WINAPI FindClose(HANDLE h);

WINIMPORT HANDLE WINAPI CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *param,
                                     DWORD flags, DWORD *id);
WINIMPORT HANDLE WINAPI CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name);
WINIMPORT HANDLE WINAPI CreateMutexA(void *security, BOOL owned, const char *name);
WINIMPORT BOOL WINAPI SetEvent(HANDLE h);
WINIMPORT BOOL WINAPI ReleaseMutex(HANDLE h);
WINIMPORT DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms);
WINIMPORT DWORD WINAPI SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL alertable);
WINIMPORT void WINAPI Sleep(DWORD ms);
WINIMPORT DWORD WINAPI GetLastError(void);
WINIMPORT void WINAPI SetLastError(DWORD error);
WINIMPORT BOOL WINAPI QueryPerformanceCounter(LONGLONG *count);
WINIMPORT BOOL WINAPI QueryPerformanceFrequency(LONGLONG *frequency);

WINIMPORT void WINAPI GlobalMemoryStatus(MEMORYSTATUS *status);
WINIMPORT HANDLE WINAPI GetProcessHeap(void);
WINIMPORT void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
WINIMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *mem);
WINIMPORT void *WINAPI VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect);
WINIMPORT BOOL WINAPI VirtualFree(void *address, SIZE_T size, DWORD type);

/* windows and GDI (user32, gdi32) */
typedef void *HWND;
typedef void *HDC;
typedef void *HINSTANCE;
typedef unsigned int UINT;
typedef unsigned int WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef LRESULT(WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { long left, top, right, bottom; } RECT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; long pt_x, pt_y; } MSG;
typedef struct {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    void *hIcon, *hCursor, *hbrBackground;
    const char *lpszMenuName, *lpszClassName;
} WNDCLASSA;
typedef struct {
    DWORD biSize;
    long biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    long biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[1]; } BITMAPINFO;

#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_VISIBLE 0x10000000
#define CW_USEDEFAULT ((int)0x80000000)
#define WM_DESTROY 0x0002
#define WM_CLOSE 0x0010
#define PM_REMOVE 0x0001
#define SW_SHOW 5
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define SRCCOPY 0x00CC0020

WINIMPORT unsigned short WINAPI RegisterClassA(const WNDCLASSA *wc);
WINIMPORT HWND WINAPI CreateWindowExA(DWORD ex_style, const char *cls, const char *title, DWORD style, int x, int y,
                                      int w, int h, HWND parent, void *menu, HINSTANCE instance, void *param);
WINIMPORT LRESULT WINAPI DefWindowProcA(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
WINIMPORT BOOL WINAPI PeekMessageA(MSG *msg, HWND hwnd, UINT min, UINT max, UINT remove);
WINIMPORT BOOL WINAPI TranslateMessage(const MSG *msg);
WINIMPORT LRESULT WINAPI DispatchMessageA(const MSG *msg);
WINIMPORT BOOL WINAPI GetClientRect(HWND hwnd, RECT *rect);
WINIMPORT BOOL WINAPI AdjustWindowRect(RECT *rect, DWORD style, BOOL menu);
WINIMPORT BOOL WINAPI ShowWindow(HWND hwnd, int cmd);
WINIMPORT HDC WINAPI GetDC(HWND hwnd);
WINIMPORT int WINAPI ReleaseDC(HWND hwnd, HDC dc);
WINIMPORT void WINAPI PostQuitMessage(int code);
WINIMPORT HMODULE WINAPI GetModuleHandleA(const char *name);
WINIMPORT int WINAPI StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                   const void *bits, const BITMAPINFO *bmi, UINT usage, DWORD rop);

WINIMPORT HMODULE WINAPI LoadLibraryA(const char *name);
WINIMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
WINIMPORT DWORD WINAPI GetModuleFileNameA(HMODULE module, char *name, DWORD size);
WINIMPORT DWORD WINAPI GetEnvironmentVariableA(const char *name, char *value, DWORD size);
WINIMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

#endif
