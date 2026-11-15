@echo off
rem Collects the game's logs into a zip on your Desktop and opens the bug report page.
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\ReportBug.ps1"
