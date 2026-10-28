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

WINIMPORT HMODULE WINAPI LoadLibraryA(const char *name);
WINIMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
WINIMPORT DWORD WINAPI GetModuleFileNameA(HMODULE module, char *name, DWORD size);
WINIMPORT DWORD WINAPI GetEnvironmentVariableA(const char *name, char *value, DWORD size);
WINIMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

#endif
