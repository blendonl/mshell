@echo off
REM ============================================================
REM  mshell uninstaller — restores Windows Explorer as the shell.
REM
REM  Deleting the per-user Shell value makes Windows fall back to
REM  the system default (explorer.exe). Files in C:\mshell are
REM  left in place; delete that folder by hand if you want them gone.
REM  Your config (%APPDATA%\mshell\init.lua) is never touched, so
REM  reinstalling later picks your keybindings straight back up.
REM
REM  The privileged helper's logon task (mshelld) IS removed, since
REM  leaving it would keep starting an elevated binary from a folder
REM  you have just been told to delete. That part needs an
REM  administrator prompt.
REM ============================================================
echo.
echo  Restoring Explorer as the shell for this user ...
reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f >nul 2>&1

if %errorlevel%==0 (
    echo  Shell override removed.
) else (
    echo  No per-user Shell override was set ^(already on Explorer^).
)

REM  --- the privileged helper's logon task ---
REM  Removed rather than left behind: it starts an ELEVATED binary out of
REM  C:\mshell at every sign-in, and the header above tells you to delete that
REM  folder by hand. An uninstall that leaves the task registered leaves a
REM  privileged autostart pointing into a directory that is about to become
REM  user-writable, which is a worse thing to forget than a stray exe.
REM  Deleting it needs administrator rights — that is what /rl highest cost to
REM  create — so this reports rather than fails when run unelevated.
schtasks /query /tn "mshelld" >nul 2>&1 || goto :notask
echo  Removing the mshelld logon task ...
schtasks /end    /tn "mshelld"    >nul 2>&1
schtasks /delete /tn "mshelld" /f >nul 2>&1
if %errorlevel%==0 (
    echo  Helper logon task removed.
) else (
    echo  Could not remove it ^(needs administrator^). From an admin prompt:
    echo    schtasks /delete /tn "mshelld" /f
)
:notask

REM  A helper started by hand rather than by the task is still running, and
REM  still elevated. Stop that too, so "uninstalled" means nothing of mshell's
REM  is left running as administrator. Silent when there is nothing to stop or
REM  we do not have the rights; mshell copes with the helper vanishing.
taskkill /F /IM mshelld.exe >nul 2>&1

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
