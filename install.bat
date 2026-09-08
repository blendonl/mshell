@echo off
setlocal
set DEST=C:\mshell
set CFGDIR=%APPDATA%\mshell

set "WANTHELPERTASK="
set "MACHINEWIDE="
set "NORESTART="
for %%a in (%*) do (
    if /I "%%~a"=="/helper"    set "WANTHELPERTASK=1"
    if /I "%%~a"=="--helper"   set "WANTHELPERTASK=1"
    if /I "%%~a"=="/machine"   set "MACHINEWIDE=1"
    if /I "%%~a"=="--machine"  set "MACHINEWIDE=1"
    if /I "%%~a"=="/norestart" set "NORESTART=1"
    if /I "%%~a"=="--norestart" set "NORESTART=1"
)

set "HIVE=HKCU"
set "SCOPE=per-user"
if defined MACHINEWIDE (
    set "HIVE=HKLM"
    set "SCOPE=MACHINE-WIDE"
    net session >nul 2>&1 || (
        echo.
        echo  ERROR: /machine needs an elevated prompt.
        echo         Right-click cmd.exe and pick "Run as administrator".
        echo.
        exit /b 1
    )
    echo.
    echo  *** MACHINE-WIDE INSTALL ***
    echo  Every account on this computer will get mshell as its shell.
    echo  Each user still needs their own %%APPDATA%%\mshell\init.lua;
    echo  without one they land on the six-binding fallback keymap.
    echo.
    choice /C YN /M "  Continue" || exit /b 1
)

set "WASRUNNING="
set "RESTARTED="
set "OLDPID="
call :running && set "WASRUNNING=1"
for /f "tokens=2 delims=," %%p in ('tasklist /FI "IMAGENAME eq mshell.exe" /NH /FO CSV 2^>nul') do set "OLDPID=%%~p"

echo.
echo  Installing mshell to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"

copy /Y "%~dp0mshell.exe" "%DEST%\mshell.exe" >nul 2>&1
if errorlevel 1 (
    echo  mshell.exe is in use ^(running as your shell^) - staging the update ...
    del  "%DEST%\mshell.exe.old" >nul 2>&1
    move /Y "%DEST%\mshell.exe" "%DEST%\mshell.exe.old" >nul 2>&1
    copy /Y "%~dp0mshell.exe" "%DEST%\mshell.exe" >nul || goto :fail
)

copy /Y "%~dp0mshelld.exe" "%DEST%\mshelld.exe" >nul 2>&1
if errorlevel 1 (
    echo  mshelld.exe is in use ^(the helper is running^) - staging the update ...
    del  "%DEST%\mshelld.exe.old" >nul 2>&1
    move /Y "%DEST%\mshelld.exe" "%DEST%\mshelld.exe.old" >nul 2>&1
    copy /Y "%~dp0mshelld.exe" "%DEST%\mshelld.exe" >nul || goto :fail
)

if not exist "%CFGDIR%" mkdir "%CFGDIR%"
if exist "%CFGDIR%\init.lua" (
    echo  Keeping your existing config at %CFGDIR%\init.lua
    copy /Y "%~dp0config\init.lua" "%CFGDIR%\init.lua.new" >nul 2>&1
    echo  This release's default config is beside it as init.lua.new
    echo  Upgrading from 0.5.0? Compare them - see CHANGELOG.md, "Upgrading"
) else if exist "%DEST%\config\init.lua" (
    move /Y "%DEST%\config\init.lua" "%CFGDIR%\init.lua" >nul || goto :fail
    rmdir "%DEST%\config" >nul 2>&1
    echo  Moved your old %DEST%\config\init.lua to %CFGDIR%\init.lua
) else (
    copy /Y "%~dp0config\init.lua" "%CFGDIR%\init.lua" >nul || goto :fail
    echo  Default config installed to %CFGDIR%\init.lua
)

copy /Y "%~dp0config\init.full.lua" "%CFGDIR%\init.full.lua" >nul 2>&1

if not exist "%CFGDIR%\meta" mkdir "%CFGDIR%\meta"
copy /Y "%~dp0meta\mshell.lua" "%CFGDIR%\meta\mshell.lua" >nul 2>&1
copy /Y "%~dp0meta\types.lua"  "%CFGDIR%\meta\types.lua"  >nul 2>&1
if not exist "%CFGDIR%\.luarc.json" (
    copy /Y "%~dp0config\.luarc.json" "%CFGDIR%\.luarc.json" >nul 2>&1
    echo  Editor type definitions installed to %CFGDIR%\meta
)

echo  Pointing the %SCOPE% shell at %DEST%\mshell.exe ...
reg add "%HIVE%\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" ^
    /v Shell /t REG_SZ /d "%DEST%\mshell.exe --shell" /f >nul || goto :fail

echo  Applying registry hardening (the `input` tweaks) ...
"%DEST%\mshell.exe" --tweaks apply input

"%DEST%\mshell.exe" --tweaks apply apps

net session >nul 2>&1 || (
    echo  ^(Not an administrator prompt, so the three policy values were left
    echo   alone: the Win-key hotkey policy, Win+L, and the Chromium
    echo   occlusion policy that keeps a hidden browser window drawing.
    echo   Re-run this from an admin prompt to apply those.^)
)
copy /Y "%~dp0harden.reg"       "%DEST%\harden.reg"       >nul
copy /Y "%~dp0harden-undo.reg"  "%DEST%\harden-undo.reg"  >nul

call :helpertask

if not defined WASRUNNING goto :report

if defined NORESTART (
    echo  Installed. The caller asked to restart mshell itself - not touching it.
    goto :report
)

set "AUTORESTART=0x1"
for /f "tokens=3" %%v in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoRestartShell 2^>nul ^| find /I "AutoRestartShell"') do set "AUTORESTART=%%v"
if "%AUTORESTART%"=="0x0" goto :noautorestart
if "%AUTORESTART%"=="0"   goto :noautorestart

echo  Restarting mshell so the new build takes over ...
taskkill /F /IM mshell.exe >nul 2>&1

call :wait 3
call :running   || goto :start_it
call :oldalive  && goto :nokill
goto :restarted

:start_it
start "" "%DEST%\mshell.exe" --shell
call :wait 2
call :running || goto :nostart

:restarted
del "%DEST%\mshell.exe.old" >nul 2>&1
set RESTARTED=1
echo  Restarted - the build you just installed is the one running.
goto :report

:nokill
echo  Could not stop the running mshell ^(not enough rights?^).
echo  The new build IS installed - sign out and back in to switch to it.
goto :report

:noautorestart
echo  Not restarting: AutoRestartShell is off on this machine, so stopping the
echo  shell would log you out rather than bring mshell back.
echo  The new build IS installed - sign out and back in to switch to it.
goto :report

:nostart
echo  mshell did not come back up. Press Ctrl+Shift+Esc ^> File ^> Run new
echo  task, run  %DEST%\mshell.exe --shell  , or just sign out and back in.
goto :report

:report
echo.
if defined RESTARTED (
    echo  Done. mshell is the shell for THIS user account and the new build
    echo  is already running - no sign-out needed.
) else (
    echo  Done. mshell is now the shell for THIS user account.
    echo  Sign out and back in to start it ^(Explorer will NOT launch^).
)
echo  ^(Ctrl+Alt+Del is a kernel sequence and cannot be disabled - by design.^)
echo.
echo  Your config lives at:
echo    %CFGDIR%\init.lua
echo  Edit it and press Win+Shift+R to reload without signing out.
echo.
if defined HELPERHINT (
    echo  Windows owned by elevated processes ^(Task Manager, regedit^) will
    echo  float rather than tile: that needs the privileged helper, which is
    echo  installed at %DEST%\mshelld.exe but not started. To run it at every
    echo  sign-in, re-run this script from an administrator prompt:
    echo    install.bat /helper
    echo  It is optional - see INSTALL.md, "The privileged helper".
    echo.
)
echo  RECOVERY if something goes wrong:
echo    Press Ctrl+Shift+Esc (Task Manager works with no shell),
echo    File ^> Run new task ^> type  explorer.exe   for a temporary
echo    desktop, or   regedit   to remove the Shell value, or just
echo    run uninstall.bat.
echo.
goto :eof

:fail
echo.
echo  INSTALL FAILED. Make sure mshell.exe, mshelld.exe and
echo  config\init.lua are next to this script, and that you can
echo  write to C:\ and %CFGDIR%.
exit /b 1

:helpertask
set "HELPERTASK="
schtasks /query /tn "mshelld" >nul 2>&1 && set "HELPERTASK=1"

if defined HELPERTASK     goto :helper_refresh
if defined WANTHELPERTASK goto :helper_create
set "HELPERHINT=1"
goto :eof

:helper_create
call :iselevated || goto :helper_needadmin
echo  Registering the mshelld logon task ...
schtasks /create /tn "mshelld" /tr "%DEST%\mshelld.exe" /sc onlogon /rl highest /f >nul 2>&1 || goto :helper_createfail

:helper_refresh
call :helperrunning || goto :helper_start
schtasks /end /tn "mshelld" >nul 2>&1
call :wait 2
call :helperrunning && goto :helper_nostop

:helper_start
schtasks /run /tn "mshelld" >nul 2>&1
call :wait 2
call :helperrunning || goto :helper_nostart
del "%DEST%\mshelld.exe.old" >nul 2>&1
echo  Privileged helper running - windows owned by elevated processes will tile.
goto :eof

:helper_nostop
echo  Could not stop the running mshelld ^(needs administrator^).
echo  The new helper IS installed - it takes over at your next sign-in.
goto :eof

:helper_nostart
echo  The mshelld logon task is registered but the helper is not up yet.
echo  It starts at your next sign-in; %%TEMP%%\mshelld.log says why if it does not.
goto :eof

:helper_needadmin
set "HELPERHINT=1"
echo  /helper needs an administrator prompt ^(the task is /rl highest^).
echo  mshelld.exe IS installed - re-run  install.bat /helper  as administrator.
goto :eof

:helper_createfail
echo  Could not register the mshelld logon task. By hand:
echo    schtasks /create /tn "mshelld" /tr "%DEST%\mshelld.exe" /sc onlogon /rl highest /f
goto :eof

:running
tasklist /FI "IMAGENAME eq mshell.exe" /NH 2>nul | find /I "mshell.exe" >nul
goto :eof

:oldalive
if not defined OLDPID exit /b 1
tasklist /FI "PID eq %OLDPID%" /NH 2>nul | find /I "mshell.exe" >nul
goto :eof

:helperrunning
tasklist /FI "IMAGENAME eq mshelld.exe" /NH 2>nul | find /I "mshelld.exe" >nul
goto :eof

:iselevated
net session >nul 2>&1
goto :eof

:wait
timeout /t %1 /nobreak >nul 2>&1 || ping -n %1 127.0.0.1 >nul 2>&1
goto :eof
