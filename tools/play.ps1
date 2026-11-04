# Text-mode launcher for the native build (decomp/build/dra2_native.exe): a small menu to play, rebuild and find saves
# and logs. "Play.cmd -Console" opens it (Play.cmd alone opens the graphical one, tools/launcher.ps1); "Play.cmd -Now"
# plays at once.
#   saves: <repo>\saves (kept between runs)   logs: <repo>\logs\last_run.txt (and sound.wav when recording)
param([switch]$Now, [switch]$Console)  # -Console: this menu (Play.cmd -Console)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'decomp\build\dra2_native.exe'
$data = Join-Path $root 'extracted'
$saves = Join-Path $root 'saves'
$logs = Join-Path $root 'logs'
try { $Host.UI.RawUI.WindowTitle = 'Digimon Rumble Arena 2 (native build)' } catch { }  # no console with Play.exe -Now

function Show-Controls {
    ''
    Write-Host '  Keyboard (player 1; a Windows controller works too, and players 2-4 use controllers)'
    Write-Host '    Arrow keys   move / d-pad          Enter        Start (pause)'
    Write-Host '    Z            A (jump)              Esc          Back'
    Write-Host '    X            B (grab / throw)      Q / W        left / right trigger'
    Write-Host '    C            X (special attack)    E / R        White / Black'
    Write-Host '    V            Y                     I J K L      right stick'
    ''
}

function Test-Ready {
    if (-not (Test-Path (Join-Path $data 'default.xbe'))) {
        $iso = Get-ChildItem $root -Filter *.iso | Select-Object -First 1
        if (-not $iso) {
            Write-Host '  The game data is missing: put your disc image (.iso) in the repository folder and run this again.'
            return $false
        }
        Write-Host "  The game data hasn't been extracted yet. Extracting $($iso.Name) (about 1.6 GB, a few minutes)..."
        python -I (Join-Path $root 'tools\xdvdfs_extract.py') $iso.FullName $data
        if ($LASTEXITCODE -or -not (Test-Path (Join-Path $data 'default.xbe'))) { Write-Host '  Extraction failed.'; return $false }
    }
    if (-not (Test-Path $exe)) {
        Write-Host '  The game hasn''t been built yet. Building it...'
        if (-not (Invoke-Build)) { return $false }
    }
    return $true
}

function Invoke-Build {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\build_native.ps1') | Select-Object -Last 3 |
        ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -or -not (Test-Path $exe)) { Write-Host '  The build failed (see the lines above).'; return $false }
    Write-Host '  Built.'
    return $true
}

function Start-Game([switch]$Mute, [switch]$Record) {
    if (-not (Test-Ready)) { return }
    New-Item -ItemType Directory -Force $saves, $logs | Out-Null
    foreach ($name in 'DRA2_INPUT', 'DRA2_ARENA', 'DRA2_SNAPSHOT', 'DRA2_LOGFRAME', 'DRA2_LOGDRAW', 'DRA2_SAMPLE',
                      'DRA2_MUTE', 'DRA2_SOUNDDUMP', 'DRA2_LOGSOUND') {
        Remove-Item "env:$name" -ErrorAction SilentlyContinue
    }
    $env:DRA2_DATA = $data
    $env:DRA2_SAVES = $saves
    if ($Mute) { $env:DRA2_MUTE = '1' }
    $wav = Join-Path $logs 'sound.wav'
    if ($Record) { $env:DRA2_SOUNDDUMP = $wav }
    $log = Join-Path $logs 'last_run.txt'
    Write-Host '  Starting the game. Close its window to come back here.'
    $started = Get-Date
    $p = Start-Process $exe -PassThru -WorkingDirectory $logs -RedirectStandardOutput $log `
        -RedirectStandardError (Join-Path $logs 'last_run_errors.txt')
    $null = $p.Handle  # keeps the handle, so the exit code can be read once it has exited
    $p.WaitForExit()
    $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    $problems = Select-String $log -Pattern '^\[crash\]|Fatal|fatal error' -ErrorAction SilentlyContinue |
        Select-Object -First 5
    if ($problems) {
        Write-Host "  The game stopped with an error after $minutes min:"
        $problems | ForEach-Object { Write-Host "    $($_.Line)" }
        Write-Host "  The full log is $log"
    } else {
        Write-Host "  The game closed after $minutes min (exit code $($p.ExitCode))."
    }
    if ($Record) { "  Sound recorded to $wav" }
}

if ($Now) { Start-Game; exit }

while ($true) {
    ''
    Write-Host '  Digimon Rumble Arena 2 (native build)'
    Write-Host '  -------------------------------------'
    Write-Host '  [1] Play'
    Write-Host '  [2] Play without sound'
    Write-Host '  [3] Play and record the sound (logs\sound.wav)'
    Write-Host '  [4] Rebuild the game (after changing or pulling the code)'
    Write-Host '  [5] Open the saves folder'
    Write-Host '  [6] Open the last log'
    Write-Host '  [7] Controls'
    Write-Host '  [Q] Quit'
    $choice = (Read-Host '  Choose').Trim().ToUpper()
    switch ($choice) {
        '1' { Start-Game }
        '2' { Start-Game -Mute }
        '3' { Start-Game -Record }
        '4' { $null = Invoke-Build }
        '5' { New-Item -ItemType Directory -Force $saves | Out-Null; Invoke-Item $saves }
        '6' {
            $log = Join-Path $logs 'last_run.txt'
            if (Test-Path $log) { Invoke-Item $log } else { Write-Host '  No log yet: play first.' }
        }
        '7' { Show-Controls }
        'Q' { exit }
        default { Write-Host '  Type a number from the list, or Q.' }
    }
}
