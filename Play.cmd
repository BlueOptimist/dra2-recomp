@echo off
rem Double-click to play the native build: a menu to play, rebuild and open saves and logs (tools\play.ps1).
rem "Play.cmd -Now" starts the game straight away.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1" %*
if errorlevel 1 pause
