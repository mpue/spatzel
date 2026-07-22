@echo off
REM Thin wrapper so the build also runs from cmd.exe / double-click.
REM All arguments are forwarded to build.ps1, e.g.:  build.cmd -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
