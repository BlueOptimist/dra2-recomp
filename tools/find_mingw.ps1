# Find llvm-mingw's bin folder (https://github.com/mstorsjo/llvm-mingw, the UCRT x86_64 Windows build) for the build
# scripts, and print it. Looked for in this order:
#   1. DRA2_MINGW, the bin folder or the folder above it
#   2. i686-w64-mingw32-clang.exe on PATH
#   3. a folder named llvm-mingw-* in the repository root, its parent, or %USERPROFILE%\Tools
$candidates = @()
if ($env:DRA2_MINGW) { $candidates += $env:DRA2_MINGW, (Join-Path $env:DRA2_MINGW 'bin') }
$onPath = Get-Command 'i686-w64-mingw32-clang.exe' -ErrorAction SilentlyContinue
if ($onPath) { $candidates += Split-Path -Parent $onPath.Source }
$root = Split-Path -Parent $PSScriptRoot
foreach ($dir in $root, (Split-Path -Parent $root), (Join-Path $env:USERPROFILE 'Tools')) {
    if (Test-Path $dir) {
        $candidates += Get-ChildItem $dir -Directory -Filter 'llvm-mingw-*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin' }
    }
}
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c 'i686-w64-mingw32-clang.exe'))) { return $c }
}
Write-Host 'llvm-mingw not found. Unzip it (README: Setting up from your own disc) and set DRA2_MINGW to its folder.'
