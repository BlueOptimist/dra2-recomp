# Run decomp/build/dra2.exe for a while and print its output with every code address symbolized.
# Usage: powershell -File tools\run_game.ps1 [-Seconds 15] [-Sample 3|200ms] [-Watch DAT_002f663c:8,DAT_0031f8bc]
#   -Sample: stop the game's thread every so often and print where it is (port/crash.c, DRA2_SAMPLE)
#   -Watch:  dwords printed with each sample; names are labels of the build (resolved with llvm-nm), and each
#            ":offset" follows the pointer at the current address (DAT_x:0:c = *(*(DAT_x) + 0xc))
param([int]$Seconds = 15, [string]$Sample = '', [string]$Watch = '')
$root = Split-Path -Parent $PSScriptRoot
$mingw = 'C:\Users\Veo\Tools\llvm-mingw-20261006-ucrt-x86_64\bin'
$exe = Join-Path $root 'decomp\build\dra2.exe'
$tmp = Join-Path $env:TEMP 'dra2_smoke'
New-Item -ItemType Directory -Force $tmp | Out-Null

$env:DRA2_DATA = Join-Path $root 'extracted'
$env:DRA2_SAVES = Join-Path $tmp 'saves'
$env:DRA2_SAMPLE = $Sample
$env:DRA2_WATCH = ''
if ($Watch) {
    if (-not $Sample) { $env:DRA2_SAMPLE = '3' }
    $symbols = @{}
    foreach ($line in & "$mingw\llvm-nm.exe" $exe) {
        $parts = $line -split ' '
        if ($parts.Count -eq 3) { $symbols[$parts[2].TrimStart('_')] = $parts[0] }
    }
    $specs = foreach ($w in $Watch -split ',') {
        $name, $offsets = $w.Trim() -split ':'
        $address = if ($symbols.ContainsKey($name)) { $symbols[$name] } else { $name }
        (@($address) + @($offsets | Where-Object { $_ })) -join ':'
    }
    $env:DRA2_WATCH = $specs -join ','
}

$out = Join-Path $tmp 'run.txt'
$p = Start-Process $exe -NoNewWindow -PassThru -WorkingDirectory $tmp -RedirectStandardOutput $out `
    -RedirectStandardError (Join-Path $tmp 'run_err.txt')
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "[run] still running after $Seconds s (stopped)" }
else { '[run] exit code {0} (0x{0:X8})' -f $p.ExitCode }
python -I (Join-Path $root 'tools\symbolize.py') $exe "$mingw\llvm-nm.exe" $out
