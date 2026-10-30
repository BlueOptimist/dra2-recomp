# Full rebuild: Ghidra export -> compile check -> objects (MinGW) -> Xbox stubs -> generated data -> link -> smoke run.
# Usage: powershell -File tools\build_all.ps1 [-SkipExport] [-SkipRun] [-Instrument] [-PatternInit]  (-Instrument: count calls, see port/calls.c; -PatternInit: locals start as 0xAA bytes)
# Paths go through the 8.3 short name of the project folder: Ghidra's .bat launchers break on "(USA)".
param([switch]$SkipExport, [switch]$SkipRun, [switch]$Instrument, [switch]$PatternInit)
$ErrorActionPreference = 'Stop'
$s = 'C:\Users\Veo\DOWNLO~1\DIGIMO~2'
$llvm = 'C:\Users\Veo\Tools\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin'
$mingw = 'C:\Users\Veo\Tools\llvm-mingw-20261006-ucrt-x86_64\bin'
$ghidra = 'C:\Users\Veo\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat'
$env:JAVA_HOME = 'C:\Users\Veo\Tools\jdk-21.0.12.1+1'

if (-not $SkipExport) {
    Remove-Item -Recurse -Force "$s\decomp\src", "$s\decomp\lib" -ErrorAction SilentlyContinue
    & $ghidra "$s\ghidra" DigimonRumbleArena2 -process default.xbe -noanalysis -readOnly -scriptPath "$s\tools\ghidra_scripts" `
        -postScript ExportDecomp.java "$s\decomp" .text 150 "$s\analysis\modules.tsv" `
        -postScript ExportHeader.java "$s\decomp\build\program.h" `
        -postScript DumpDataSymbols.java "$s\analysis" *> "$s\ghidra\export_console.txt"
    Select-String -Path "$s\ghidra\export_console.txt" -Pattern 'decompiled \d+' | ForEach-Object { $_.Line -replace '^.*> ', '' }
}
python -I "$s\tools\compile_check.py" "$s\decomp" "$llvm\clang.exe" > "$s\decomp\build\compile_check_log.txt"
Select-String -Path "$s\decomp\build\compile_check_log.txt" -Pattern 'compile cleanly' | ForEach-Object { $_.Line }
python -I "$s\tools\build_objects.py" "$s\decomp" $mingw --target=i686-w64-mingw32 $(if ($Instrument) { '--instrument' }) $(if ($PatternInit) { '--pattern-init' }) | Select-String -Pattern 'Lua|FAILED|objects:|stubbed'
python -I "$s\tools\gen_platform_stubs.py" "$s\decomp" $mingw | Select-Object -First 1
& "$mingw\clang.exe" -c --target=i686-w64-mingw32 -std=gnu11 -fms-extensions -O0 -w -Dsize_t=ghidra_size_t `
    "-I$s\decomp\build" "-I$s\decomp" "$s\decomp\port\xbox_stubs.c" -o "$s\decomp\build\obj\port_xbox_stubs.obj"
python -I "$s\tools\gen_data.py" "$s\extracted\default.xbe" "$s\decomp" "$s\analysis" $mingw --target=i686-w64-mingw32 |
    Select-String -Pattern 'after the data'
python -I "$s\tools\link_exe.py" "$s\decomp" $mingw

if (-not $SkipRun -and (Test-Path "$s\decomp\build\dra2.exe")) {
    $tmp = Join-Path $env:TEMP 'dra2_smoke'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $env:DRA2_DATA = "$s\extracted"
    $env:DRA2_SAVES = "$tmp\saves"
    $psi = New-Object System.Diagnostics.ProcessStartInfo "$s\decomp\build\dra2.exe"
    $psi.WorkingDirectory = $tmp
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); 'smoke run: still running after 30 s (stopped)' }
    else { 'smoke run: exit code {0} (0x{0:X8})' -f $p.ExitCode }
    ($out.Result -split "`n" | Select-Object -Last 25) -join "`n"
    $err.Result
}
