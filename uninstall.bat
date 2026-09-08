@echo off
echo.
echo  Restoring Explorer as the shell for this user ...
set "HIVE=HKCU"
set "MACHINEWIDE="
for %%a in (%*) do (
    if /I "%%~a"=="/machine"  set "MACHINEWIDE=1"
    if /I "%%~a"=="--machine" set "MACHINEWIDE=1"
)
if defined MACHINEWIDE (
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

taskkill /F /IM mshelld.exe >nul 2>&1

echo  Reverting OS-shortcut hardening ...
set "HAVEBACKUP="
reg query "HKCU\Software\mshell\TweakBackup" >nul 2>&1 && set "HAVEBACKUP=1"
if defined HAVEBACKUP if exist "C:\mshell\mshell.exe" (
    "C:\mshell\mshell.exe" --tweaks revert input
    "C:\mshell\mshell.exe" --tweaks revert apps
    goto :reverted
)
if exist "%~dp0harden-undo.reg" (
    reg import "%~dp0harden-undo.reg" >nul
) else if exist "C:\mshell\harden-undo.reg" (
    reg import "C:\mshell\harden-undo.reg" >nul
) else (
    echo  ^(harden-undo.reg not found next to this script or in C:\mshell —
    echo   Windows shortcut defaults were left unchanged.^)
)
:reverted

echo.
echo  Done. Sign out and back in to return to Explorer.
echo  Your config is still at %APPDATA%\mshell\init.lua
echo  ^(delete that folder too if you want mshell fully gone^).
echo.
