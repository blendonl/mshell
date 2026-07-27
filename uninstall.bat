@echo off
REM ============================================================
REM  mshell uninstaller — restores Windows Explorer as the shell.
REM
REM  Deleting the per-user Shell value makes Windows fall back to
REM  the system default (explorer.exe). Files in C:\mshell are
REM  left in place; delete that folder by hand if you want them gone.
REM  Your config (%APPDATA%\mshell\init.lua) is never touched, so
REM  reinstalling later picks your keybindings straight back up.
REM ============================================================
echo.
echo  Restoring Explorer as the shell for this user ...
REM  Remove the per-user override first. If /machine was used, that key is in
REM  HKLM instead — passed the same way, and needing the same elevated prompt.
set "HIVE=HKCU"
if /I "%~1"=="/machine" (
    set "HIVE=HKLM"
    net session >nul 2>&1 || (
        echo  ERROR: /machine needs an elevated prompt.
        exit /b 1
    )
)
reg delete "%HIVE%\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f >nul 2>&1

if %errorlevel%==0 (
    echo  Shell override removed.
) else (
    echo  No per-user Shell override was set ^(already on Explorer^).
)

echo  Reverting OS-shortcut hardening (harden-undo.reg) ...
if exist "%~dp0harden-undo.reg" (
    reg import "%~dp0harden-undo.reg" >nul
) else if exist "C:\mshell\harden-undo.reg" (
    reg import "C:\mshell\harden-undo.reg" >nul
) else (
    echo  ^(harden-undo.reg not found next to this script or in C:\mshell —
    echo   Windows shortcut defaults were left unchanged.^)
)

echo.
echo  Done. Sign out and back in to return to Explorer.
echo  Your config is still at %APPDATA%\mshell\init.lua
echo  ^(delete that folder too if you want mshell fully gone^).
echo.
