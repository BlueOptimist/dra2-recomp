/* The rendering backend: a Win32 window and a Direct3D 9 device that the Xbox Direct3D layer (port/d3d8.c) draws
 * through. Direct3D 9 is the closest Windows API to what the game expects: the same fixed-function render and
 * texture-stage states, ARGB colours and vertex formats as Direct3D 8 on the Xbox.
 *
 * This file is its own translation unit with the real Windows headers (the other port files use win32_min.h); d3d8.c
 * only sees the gpu_* functions below, with plain C types.
 */
#undef size_t  /* the decompiler build defines size_t as ghidra_size_t */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static HWND window;
static IDirect3D9 *d3d;
static IDirect3DDevice9 *device;
static int in_scene;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY)
        ExitProcess(0);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* the window (shown at twice the game's resolution) and a device rendering at the game's resolution */
int gpu_open(unsigned width, unsigned height)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "DRA2Window";
    RegisterClassA(&wc);
    RECT r = { 0, 0, (LONG)width * 2, (LONG)height * 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExA(0, wc.lpszClassName, "Digimon Rumble Arena 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("[gpu] Direct3DCreate9 failed\n");
        return 0;
    }
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;  /* the game paces itself (frame limiter) */
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(hr)) {
        printf("[gpu] CreateDevice failed: %08lx\n", (unsigned long)hr);
        return 0;
    }
    printf("[gpu] Direct3D 9 device %ux%u\n", width, height);
    return 1;
}

static void begin_scene(void)
{
    if (device && !in_scene && SUCCEEDED(IDirect3DDevice9_BeginScene(device)))
        in_scene = 1;
}

/* Clear with Xbox flags: D3DCLEAR_ZBUFFER 1, STENCIL 2, TARGET_R/G/B/A 0x10-0x80 */
void gpu_clear(int count, const long *rects, unsigned flags, unsigned color, float z, unsigned stencil)
{
    if (!device)
        return;
    DWORD d9 = 0;
    if (flags & 0xF0)
        d9 |= D3DCLEAR_TARGET;
    if (flags & 1)
        d9 |= D3DCLEAR_ZBUFFER;
    if (flags & 2)
        d9 |= D3DCLEAR_STENCIL;
    if (!d9)
        return;
    begin_scene();
    IDirect3DDevice9_Clear(device, rects ? (DWORD)count : 0, (const D3DRECT *)(rects ? rects : 0), d9, color, z,
                           stencil);
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void gpu_present(void)
{
    if (device) {
        if (in_scene) {
            IDirect3DDevice9_EndScene(device);
            in_scene = 0;
        }
        IDirect3DDevice9_Present(device, 0, 0, 0, 0);
    }
    pump_messages();
}
