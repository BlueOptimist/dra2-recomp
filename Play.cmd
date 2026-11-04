@echo off
rem Double-click to open the launcher for the native build (tools\launcher.ps1).
rem   Play.cmd -Now       start the game straight away, no launcher
rem   Play.cmd -Console   the text menu instead of the launcher window (tools\play.ps1)
if "%~1"=="" (
    start "" powershell -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0tools\launcher.ps1"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1" %*
if errorlevel 1 pause
