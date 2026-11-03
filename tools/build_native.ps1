# Build decomp/build/dra2_native.exe and dra2_native.dll: the game's original code, loaded from default.xbe at run
# time, with the port's Xbox library replacements (port/native.c, port/native_launcher.c), compiled here with
# optimisation (build/native/port_*.obj).
# Usage: powershell -File tools\build_native.ps1
$root = Split-Path -Parent $PSScriptRoot
$mingw = 'C:\Users\Veo\Tools\llvm-mingw-20261006-ucrt-x86_64\bin'
$build = Join-Path $root 'decomp\build'
$out = Join-Path $build 'native'
New-Item -ItemType Directory -Force $out | Out-Null

$cc = Join-Path $mingw 'i686-w64-mingw32-clang.exe'
# the Xbox kernel exports (port/kernel.c): compiled first, gen_native.py routes the import slots to them
& $cc -c -O1 -w (Join-Path $root 'decomp\port\kernel.c') -o (Join-Path $out 'kernel.obj')
if ($LASTEXITCODE) { exit 1 }
python -I (Join-Path $root 'tools\gen_native.py') (Join-Path $root 'decomp') $mingw
if ($LASTEXITCODE) { exit 1 }
& $cc -c -O1 -w (Join-Path $root 'decomp\port\native.c') -o (Join-Path $out 'native.obj')
if ($LASTEXITCODE) { exit 1 }
& $cc -c (Join-Path $out 'native_tables.s') -o (Join-Path $out 'native_tables.obj')
if ($LASTEXITCODE) { exit 1 }

# the port's platform layer; not game_fixes (decompiler repairs), startup (the recompiled entry point), xbox_stubs
# (stand-ins for library functions the original code now provides itself), crt_math and cxx_runtime (the game's own C
# runtime runs natively). Compiled here with optimisation: tools\build_all.ps1 builds its copies at -O0, and at -O0
# the vertex program interpreter alone held fights to 20-30 fps. Frame pointers stay for the crash reporter's chains.
$flags = '--target=i686-w64-mingw32', '-std=gnu11', '-fms-extensions', '-w', '-Dsize_t=ghidra_size_t', '-c', '-O2',
    '-fno-omit-frame-pointer', '-fno-builtin', '-Wno-error=int-conversion', '-Wno-error=incompatible-pointer-types',
    '-Wno-error=implicit-function-declaration', "-I$build", "-I$(Join-Path $root 'decomp')"
$ports = foreach ($name in 'calls', 'crash', 'd3d8', 'dsound', 'gpu', 'nv2a', 'platform', 'pushbuffer', 'xapi') {
    $obj = Join-Path $out "port_$name.obj"
    & $cc @flags (Join-Path $root "decomp\port\$name.c") -o $obj
    if ($LASTEXITCODE) { "port/$name.c failed to compile"; exit 1 }
    $obj
}
# the loader and the port, as a DLL above the Xbox range
$dll = Join-Path $build 'dra2_native.dll'
& $cc -shared -o $dll (Join-Path $out 'native.obj') (Join-Path $out 'native_tables.obj') (Join-Path $out 'kernel.obj') @ports `
    '-Wl,--image-base=0x10000000' '-Wl,--disable-dynamicbase' '-Wl,--error-limit=0' -lm -luser32 -lgdi32 -ld3d9 2>&1 | Select-Object -First 30
if ($LASTEXITCODE) { 'native DLL link failed'; exit 1 }
# the launcher, at the Xbox base, holding the range (port/native_launcher.c)
$exe = Join-Path $build 'dra2_native.exe'
& $cc -O1 -nostdlib (Join-Path $root 'decomp\port\native_launcher.c') -o $exe '-Wl,--entry=_start' `
    '-Wl,--image-base=0x10000' '-Wl,--disable-dynamicbase' -lkernel32 2>&1 | Select-Object -First 10
if ($LASTEXITCODE) { 'launcher link failed'; exit 1 }
"linked $dll ({0} KB) and $exe" -f [int]((Get-Item $dll).Length / 1KB)
