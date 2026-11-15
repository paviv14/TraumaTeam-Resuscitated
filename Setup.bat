@echo off
rem Trauma Team Resuscitated - setup window (builds the game from your own extracted disc; the work is in Setup.ps1).
rem For the text-only setup run: powershell -ExecutionPolicy Bypass -File Setup.ps1
start "" powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0SetupGUI.ps1"
