@echo off
REM ============================================================
REM  mshell installer — sets mshell as the per-user shell.
REM
REM  Run this on Windows from the folder that contains
REM  mshell.exe, mshelld.exe and the config\ directory (i.e.
REM  next to this .bat). It installs the program to C:\mshell,
REM  your config to %APPDATA%\mshell, and points the per-user
REM  Winlogon Shell key at the exe.
REM
REM  Options:
REM    /helper   also register the elevated logon task that runs
REM              mshelld.exe, the privileged helper (see
REM              INSTALL.md). Needs an administrator prompt.
REM              mshelld.exe is INSTALLED either way — this only
REM              decides whether it is started for you.
REM
REM  Re-running it is how you upgrade: the installed exe is
REM  ALWAYS replaced, even when it is the shell you are looking
REM  at right now, and if mshell was running it is restarted at
REM  the end so the build you just installed is the one running
REM  — no sign-out. Your config is the one thing never
REM  overwritten.
REM
REM  Per-user (HKCU) is the DEFAULT and is deliberate: it only
REM  affects THIS account and is trivially reversible with
REM  uninstall.bat. Run it as the account that will use mshell
REM  — HKCU and %APPDATA% both follow whoever runs this script.
REM
REM  install.bat /machine sets the shell for EVERY account on
REM  the machine (HKLM). It needs an elevated prompt, and it is
REM  a much bigger commitment: get it wrong and every user signs
REM  in to a black screen, not just you. There is deliberately
REM  no way to do it by accident.
REM ============================================================
setlocal
set DEST=C:\mshell
set CFGDIR=%APPDATA%\mshell

REM  --- options ---
REM  Flags rather than prompts on purpose: this script is also run unattended
REM  (the :wait helper below already accommodates a non-interactive console),
REM  and a question waiting on stdin would hang that. /machine is the one
REM  exception — see below.
REM
REM    /helper   also register mshelld.exe's logon task
REM    /machine  set the shell for EVERY account (HKLM) rather than just this one
set "WANTHELPERTASK="
set "MACHINEWIDE="
for %%a in (%*) do (
    if /I "%%~a"=="/helper"   set "WANTHELPERTASK=1"
    if /I "%%~a"=="--helper"  set "WANTHELPERTASK=1"
    if /I "%%~a"=="/machine"  set "MACHINEWIDE=1"
    if /I "%%~a"=="--machine" set "MACHINEWIDE=1"
)

REM  --- per-user or machine-wide? ---
REM  HKCU is the default and affects only the account running this script.
REM  HKLM affects every account, which is a much bigger commitment: get it wrong
REM  and everyone signs in to a black screen, not just you. Hence the elevation
REM  check up front — so the failure is a sentence rather than an opaque
REM  "Access is denied" halfway through an install that has already copied
REM  files — and hence the confirmation, which is the one place a prompt is
REM  worth blocking an unattended run.
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

REM  --- is there a live mshell to replace? ---
REM  Settled up front, before we touch anything: both the locked-exe handling
REM  below and the restart at the end hang off the answer. The pid is recorded
REM  too, so the restart can tell "the old process is still there" apart from
REM  "this is the new one, already back up".
set "WASRUNNING="
set "RESTARTED="
set "OLDPID="
call :running && set "WASRUNNING=1"
for /f "tokens=2 delims=," %%p in ('tasklist /FI "IMAGENAME eq mshell.exe" /NH /FO CSV 2^>nul') do set "OLDPID=%%~p"

echo.
echo  Installing mshell to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"

REM  --- mshell.exe (handle the "already running as the shell" case) ---
REM  If mshell is your current shell, C:\mshell\mshell.exe is the LIVE process
REM  image: Windows refuses to overwrite it (sharing violation) — which is why
REM  a plain copy "doesn't update the exe". You CAN rename a running image,
REM  though, so: try a direct copy first; if it's locked, move the old one
REM  aside (the running process keeps using it) and drop the new one in.
REM
REM  The new exe is therefore always in place by the end of this block; the
REM  restart at the bottom is what makes it the running one. Replacing before
REM  restarting is the safe order — if the restart goes wrong, the upgrade is
REM  still installed and starts at your next sign-in.
copy /Y "%~dp0mshell.exe" "%DEST%\mshell.exe" >nul 2>&1
if errorlevel 1 (
    echo  mshell.exe is in use ^(running as your shell^) - staging the update ...
    del  "%DEST%\mshell.exe.old" >nul 2>&1
    move /Y "%DEST%\mshell.exe" "%DEST%\mshell.exe.old" >nul 2>&1
    copy /Y "%~dp0mshell.exe" "%DEST%\mshell.exe" >nul || goto :fail
)

REM  --- mshelld.exe (the privileged helper) ---
REM  ALWAYS installed, even though running it is opt-in. INSTALL.md's logon
REM  task names %DEST%\mshelld.exe, so the file has to be there for that command
REM  to mean anything — and schtasks does not validate the path it is given, so
REM  shipping the exe without installing it is exactly how you get a task that
REM  registers successfully and then fails silently at every sign-in, with no
REM  symptom beyond "elevated windows still float".
REM
REM  Installing it in lockstep with mshell.exe also keeps the pair from
REM  drifting: the two shake hands on MSHELLD_PROTO_VERSION (src/proto.h) and a
REM  mismatch is refused, so replacing one binary and not the other is a
REM  supported way to break the helper across an upgrade.
REM
REM  Same staging dance as above, for the same reason: a helper already running
REM  as the logon task has its image locked. Renaming it aside is allowed and
REM  the running process keeps using the old file until it is restarted, which
REM  the helper block further down does when it has the rights to.
copy /Y "%~dp0mshelld.exe" "%DEST%\mshelld.exe" >nul 2>&1
if errorlevel 1 (
    echo  mshelld.exe is in use ^(the helper is running^) - staging the update ...
    del  "%DEST%\mshelld.exe.old" >nul 2>&1
    move /Y "%DEST%\mshelld.exe" "%DEST%\mshelld.exe.old" >nul 2>&1
    copy /Y "%~dp0mshelld.exe" "%DEST%\mshelld.exe" >nul || goto :fail
)

REM  --- config: %APPDATA%\mshell\init.lua ---
REM  This is the standard Windows spot for per-user config, so it lives in
REM  YOUR profile and is never touched by a reinstall. An existing init.lua
REM  is left exactly as it is — upgrading must never eat your keybindings.
REM  When a config is already there we keep it, but we also drop this release's
REM  default beside it as init.lua.new. Without that an upgrade can silently fail
REM  to deliver a config-level fix: 0.6.0 fixes a bug in the 0.5.0 default config
REM  that rejected the whole file and left you with no keybinds at all, and a
REM  kept config carries that bug forward.
if not exist "%CFGDIR%" mkdir "%CFGDIR%"
if exist "%CFGDIR%\init.lua" (
    echo  Keeping your existing config at %CFGDIR%\init.lua
    copy /Y "%~dp0config\init.lua" "%CFGDIR%\init.lua.new" >nul 2>&1
    echo  This release's default config is beside it as init.lua.new
    echo  Upgrading from 0.5.0? Compare them - see CHANGELOG.md, "Upgrading"
) else if exist "%DEST%\config\init.lua" (
    REM  Pre-0.4 install: the config used to sit next to the exe. Move the
    REM  one you have been editing rather than overwriting it with defaults.
    move /Y "%DEST%\config\init.lua" "%CFGDIR%\init.lua" >nul || goto :fail
    rmdir "%DEST%\config" >nul 2>&1
    echo  Moved your old %DEST%\config\init.lua to %CFGDIR%\init.lua
) else (
    copy /Y "%~dp0config\init.lua" "%CFGDIR%\init.lua" >nul || goto :fail
    echo  Default config installed to %CFGDIR%\init.lua
)

REM  The large annotated example always goes alongside, and is always
REM  refreshed: it is reference material, not your config, so there is nothing
REM  in it to preserve and an out-of-date copy would document the wrong release.
copy /Y "%~dp0config\init.full.lua" "%CFGDIR%\init.full.lua" >nul 2>&1

echo  Pointing the %SCOPE% shell at %DEST%\mshell.exe ...
reg add "%HIVE%\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" ^
    /v Shell /t REG_SZ /d "%DEST%\mshell.exe --shell" /f >nul || goto :fail

echo  Applying registry hardening (harden.reg) ...
REM  All registry hardening lives in harden.reg (single source of truth):
REM  blanket Win-hotkey off, longer low-level-hook timeout, Win+L lock, Game
REM  Bar, the accessibility chords, and bare PrtScn. Reverted by harden-undo.reg
REM  (uninstall.bat imports it). Also copy both next to the exe so the shell
REM  install is self-contained.
reg import "%~dp0harden.reg" >nul || goto :fail
copy /Y "%~dp0harden.reg"       "%DEST%\harden.reg"       >nul
copy /Y "%~dp0harden-undo.reg"  "%DEST%\harden-undo.reg"  >nul

REM  --- the privileged helper's logon task ---
REM  Done BEFORE mshell is restarted, so the shell comes up last and connects to
REM  a helper that is already listening rather than having to notice one appear.
call :helpertask

REM  --- swap the running mshell for the one just installed ---
REM  Nothing to restart if mshell wasn't running when this script started
REM  (first install, or you're still on Explorer) — it starts at sign-in.
if not defined WASRUNNING goto :report

REM  Stopping the shell is only safe while Windows is willing to put a shell
REM  back: that is Winlogon's AutoRestartShell (HKLM, 1 by default). With it
REM  explicitly set to 0, the session LOGS OFF when the shell exits instead —
REM  which would throw away whatever you have open. So look before killing and
REM  leave a running mshell alone if that is the setup; the new build is
REM  already installed and starts at the next sign-in either way.
set "AUTORESTART=0x1"
for /f "tokens=3" %%v in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoRestartShell 2^>nul ^| find /I "AutoRestartShell"') do set "AUTORESTART=%%v"
REM  0x0 is what a REG_DWORD reads back as; "0" covers it being set as a string.
if "%AUTORESTART%"=="0x0" goto :noautorestart
if "%AUTORESTART%"=="0"   goto :noautorestart

echo  Restarting mshell so the new build takes over ...
taskkill /F /IM mshell.exe >nul 2>&1

REM  Winlogon normally relaunches the shell by itself the moment it exits
REM  (AutoRestartShell, on by default), and it launches whatever the Shell
REM  value names — the exe written above. So wait for that, then look at what
REM  is actually running: nothing means we start the new build ourselves; the
REM  pid we recorded up front means the kill was refused and the old process
REM  is still there; any other pid is Winlogon having done it for us, and we
REM  must NOT start a second instance — two would fight over the keyboard hook.
REM  Comparing pids (rather than taskkill's exit code, which is 128 both for
REM  "already gone" and for some failures, or its SUCCESS/ERROR text, which is
REM  translated on a non-English Windows) is what makes this reliable.
call :wait 3
call :running   || goto :start_it
call :oldalive  && goto :nokill
goto :restarted

:start_it
REM  Started the same way the Shell key starts it. One difference worth
REM  knowing: this instance inherits THIS script's token, so if you ran
REM  install.bat elevated it is elevated until your next sign-in.
start "" "%DEST%\mshell.exe" --shell
call :wait 2
call :running || goto :nostart

:restarted
REM  The old image was only kept because it was locked; the process holding
REM  it is gone now, so the staged copy can go too.
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

REM ---- the privileged helper's logon task ---------------------------------
REM  mshelld.exe is always installed; RUNNING it is opt-in, because the task
REM  needs administrator rights and mshell is fully usable without it (elevated
REM  windows float instead of tiling, which is the documented default).
REM
REM  Three cases. Already registered: restart the helper so the build just
REM  installed is the live one. Asked for with /helper: create the task. Neither:
REM  set HELPERHINT so :report says how, once, and carry on.
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
REM  Make the freshly installed helper the running one. mshelld holds a
REM  singleton mutex (Local\mshelld_singleton), so launching a second copy while
REM  the old one is alive just exits immediately and leaves the OLD build
REM  serving — which is why this stops it first and confirms it actually
REM  stopped rather than assuming schtasks /end succeeded.
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
REM  HELPERHINT so the summary at the end repeats this: the line above scrolls
REM  past while the shell is being restarted, and :report is what gets read.
set "HELPERHINT=1"
echo  /helper needs an administrator prompt ^(the task is /rl highest^).
echo  mshelld.exe IS installed - re-run  install.bat /helper  as administrator.
goto :eof

:helper_createfail
echo  Could not register the mshelld logon task. By hand:
echo    schtasks /create /tn "mshelld" /tr "%DEST%\mshelld.exe" /sc onlogon /rl highest /f
goto :eof

REM ---- helpers (reached by CALL only) ------------------------------------
REM  Exit code 0 = an mshell.exe is running, 1 = none. `goto :eof` keeps
REM  find's errorlevel, so callers can use  call :running && ...
:running
tasklist /FI "IMAGENAME eq mshell.exe" /NH 2>nul | find /I "mshell.exe" >nul
goto :eof

REM  Exit code 0 = the exact process we tried to kill is still alive.
:oldalive
if not defined OLDPID exit /b 1
tasklist /FI "PID eq %OLDPID%" /NH 2>nul | find /I "mshell.exe" >nul
goto :eof

REM  Exit code 0 = an mshelld.exe is running, 1 = none. The IMAGENAME filter is
REM  exact, so this and :running cannot see each other's process.
:helperrunning
tasklist /FI "IMAGENAME eq mshelld.exe" /NH 2>nul | find /I "mshelld.exe" >nul
goto :eof

REM  Exit code 0 = this script has administrator rights. `net session` is the
REM  usual probe: it needs the full token, so it fails for a filtered one.
:iselevated
net session >nul 2>&1
goto :eof

REM  Pause for %1 seconds. timeout(1) is the normal path; ping covers the case
REM  where it refuses to run because stdin isn't an interactive console.
:wait
timeout /t %1 /nobreak >nul 2>&1 || ping -n %1 127.0.0.1 >nul 2>&1
goto :eof
