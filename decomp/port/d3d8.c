/* Direct3D 8 (Xbox) on Windows: the beginning of the graphics layer.
 *
 * Every function keeps the name, calling convention and parameter count of its prototype in build/program.h, so it
 * replaces the weak stub in xbox_stubs.c. Only memory management is implemented so far; rendering comes next.
 */
#include "win32_min.h"

/* D3D_AllocContiguousMemory(size, alignment): memory the GPU reads directly (vertex/index buffers, textures, push
 * buffers). On Windows it is ordinary committed memory; VirtualAlloc's 64 KB alignment covers any alignment asked. */
unsigned int __stdcall D3D8__D3D_AllocContiguousMemory(unsigned int size, unsigned int alignment)
{
    (void)alignment;
    return (unsigned int)VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
