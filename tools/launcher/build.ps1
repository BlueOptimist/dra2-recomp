# Build "DRA 2.exe" (tools/launcher/play.c, with play.rc's icon and version) into the repository root.
# Usage: powershell -File tools\launcher\build.ps1
# The icon is cover.ico when there is one (make_icon.ps1 -Image <picture>), otherwise icon.ico.
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$mingw = 'C:\Users\Veo\Tools\llvm-mingw-20261006-ucrt-x86_64\bin'
$tmp = Join-Path $env:TEMP 'dra2_play_exe'
New-Item -ItemType Directory -Force $tmp | Out-Null
if (-not (Test-Path (Join-Path $here 'icon.ico'))) { & (Join-Path $here 'make_icon.ps1') }
$icon = Join-Path $here 'cover.ico'
if (-not (Test-Path $icon)) { $icon = Join-Path $here 'icon.ico' }
"icon: $icon"
# play.rc names icon.ico: compile it next to a copy of the chosen icon
Copy-Item (Join-Path $here 'play.rc') $tmp -Force
Copy-Item $icon (Join-Path $tmp 'icon.ico') -Force
Push-Location $tmp
& (Join-Path $mingw 'x86_64-w64-mingw32-windres.exe') play.rc -O coff -o play_res.o
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { 'resource compile failed'; exit 1 }
$exe = Join-Path $root 'DRA 2.exe'
& (Join-Path $mingw 'x86_64-w64-mingw32-clang.exe') -O2 -municode -mwindows -Wall (Join-Path $here 'play.c') `
    (Join-Path $tmp 'play_res.o') -o $exe
if ($LASTEXITCODE) { 'DRA 2.exe failed to build'; exit 1 }
# the old name, from before it was renamed
Remove-Item (Join-Path $root 'Play.exe') -ErrorAction SilentlyContinue
"built $exe ({0} KB)" -f [int]((Get-Item $exe).Length / 1KB)
