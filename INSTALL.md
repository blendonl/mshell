# Installing mshell as your Windows shell

This replaces `explorer.exe` — no taskbar, no tray, no desktop. Read the
**Recovery** section before you start; a broken shell is easy to undo once
you know the Task Manager trick.

## 0. Build (on Linux)

```
make clean && make          # produces mshell.exe
```

You need `mshell.exe` and the `config/` folder on the Windows machine.

## 1. Get the files onto Windows

Copy these to the Windows box (USB, scp, network share) into one folder:

```
mshell.exe
config\init.lua
install.bat
uninstall.bat
```

`install.bat` then splits these into the two places they belong — the program
under `C:\mshell`, your config in your own profile:

```
C:\mshell\mshell.exe
%APPDATA%\mshell\init.lua      (C:\Users\<you>\AppData\Roaming\mshell\init.lua)
```

That's the standard Windows spot for per-user configuration, so your config is
never overwritten by reinstalling mshell and follows your roaming profile. An
existing `init.lua` there is always kept as-is; upgrading only replaces the exe.

If `%APPDATA%\mshell\init.lua` does not exist, mshell falls back to
`config\init.lua` next to `mshell.exe` — which is what makes step 2 below work
straight out of the unzipped folder, before you install anything.

## 2. Try it WITHOUT committing (strongly recommended)

Before making it your shell, run it alongside Explorer:

```
mshell.exe --test
```

Tile some windows, switch desktops (Win+1..9), open a terminal
(Win+Shift+Return). Quit with **Win+Shift+Q** — in `--test` that just closes
the process, it does NOT log you out. If this feels right, continue.

## 3. Install as the shell

Double-click **`install.bat`** (or run it from a terminal). It copies the
program to `C:\mshell`, installs the default config to `%APPDATA%\mshell\init.lua`
(keeping yours if one is already there), and sets the per-user shell key:

```
HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon
    Shell = "C:\mshell\mshell.exe --shell"
```

This is **per-user (HKCU)** — it only affects your account, and it overrides
the system default (`explorer.exe`) just for you. Other accounts still get
Explorer. Run `install.bat` as the account that will use mshell: both `HKCU`
and `%APPDATA%` follow whoever runs the script.

> Upgrading from 0.3 or earlier? `install.bat` moves an existing
> `C:\mshell\config\init.lua` to `%APPDATA%\mshell\init.lua` for you, so your
> keybindings carry over.

**Sign out and back in.** mshell is now your shell. Explorer never launches.

### Reinstalling / upgrading

Running `install.bat` again is the upgrade path, and it always wins over what
is already installed:

- **The exe is replaced even while it is your running shell.** Windows locks
  the image of a running process, so the installer renames the old one to
  `mshell.exe.old` and drops the new one in its place.
- **Then it restarts mshell for you**, so the build you just installed is the
  one running — no sign-out. Your windows are left where they are; the new
  instance picks them up. (If mshell wasn't running, there is nothing to
  restart and it simply starts at your next sign-in.)
- **Your config is the exception** — an existing `%APPDATA%\mshell\init.lua` is
  never overwritten.

The restart works by stopping the running mshell: Windows then puts the shell
back from the `Shell` value above — the new exe — and the installer only starts
it itself if Windows didn't. That safety net is Winlogon's `AutoRestartShell`
(`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`, `1` unless
someone turned it off). **If it is `0`, a shell that exits logs the session
off**, so the installer checks it first and skips the restart rather than
dropping you at the login screen with apps open.

If the installer can't stop the running mshell, or skips the restart, it says
so — the upgrade is still installed, so sign out and back in to switch to it.
Note that a restart launched by the installer inherits the installer's token,
so an `install.bat` you ran elevated leaves mshell elevated until your next
sign-in.

## 4. Elevation (optional, for full window control)

A non-elevated shell can't move or resize windows owned by *elevated*
processes (Task Manager, admin consoles) — Windows blocks that cross-integrity
(UIPI). mshell handles this gracefully: those windows simply float instead of
tiling. Because a shell process can't relaunch itself elevated without logging
you out (see note below), full control requires the login token itself to be
elevated. In order of preference:

- **Leave UAC on and let elevated windows float (recommended).** mshell runs
  fine as the shell; only the occasional admin window won't tile. For almost
  every setup this is the right choice — no security trade-off at all.

- **Run mshell elevated via a logon scheduled task (keeps UAC on).** Create a
  task set to *Run with highest privileges*, triggered *At log on* for your
  account, that starts `C:\mshell\mshell.exe --shell`, and point the Winlogon
  Shell key at a do-nothing stub (or leave Explorer’s default). This gives
  mshell a full token without weakening UAC system-wide. More moving parts,
  but the safe way to get full control.

- **Disable UAC (last resort — not recommended).** Setting `EnableLUA=0` turns
  off UAC for the *entire machine*, which also disables the sandbox/AppContainer
  isolation many modern apps (browsers, Store apps) rely on. Only do this on a
  dedicated/experimental machine you understand the risk on:
  ```
  reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" ^
      /v EnableLUA /t REG_DWORD /d 0 /f
  ```
  Reboot. Undo with `EnableLUA=1` + reboot (see Uninstall).

> Why not self-elevate? mshell never elevates itself — it is manifested
> `asInvoker` and runs with whatever token it was given. Besides the logoff loop
> that a self-elevating *shell* would cause (exiting ends your session, so
> "relaunch elevated, then exit" would log you straight out), elevation changes
> what your config file is. Elevate the login token instead, if you must (above).

### What elevation does to `init.lua`

**If you run mshell elevated, treat `init.lua` as administrator-level code.**

The config is a Lua script executed with the full standard library —
`os.execute`, `io`, `package.loadlib` — and it lives in `%APPDATA%\mshell\`,
which your *normal, unelevated* account can write to. So an elevated mshell
means anything running as you at ordinary privilege can edit that file and have
it run with administrator rights.

Two things follow:

- **Auto-reload is disabled when mshell is elevated.** Normally the config
  folder is watched and a save applies ~250 ms later; that would make the above
  automatic and silent, so the watcher simply does not start. `Win+Shift+R`
  still reloads, which keeps a deliberate keypress in the loop. mshell logs
  that it is elevated at startup.
- **If you elevate, restrict the folder.** Keep `%APPDATA%\mshell\` from being
  writable by anything you would not trust with admin.

The recommended option above — leave UAC on, let elevated windows float — avoids
all of this. A future release moves the privileged work into a small helper with
no config and no scripting, which removes the trade-off entirely.

## 5. Turn off the rest of Windows (optional, recommended)

mshell itself already eats every `Win`+* combo, Alt+Tab / Alt+Esc / Ctrl+Esc,
strips title bars, and — baked into `mshell.exe` (`window.c`) — forces square
corners and kills the per-window open/close/minimise animation on every
window it manages. Everything else Windows adds lives in the registry, split
into three importable files by blast radius:

- **`harden.reg`** — keyboard shortcuts the low-level hook can't reach: Win+L
  lock, the Sticky / Filter / Toggle / High-Contrast / Mouse-Keys hotkeys, and
  bare PrtScn → Snip. Per-user, no admin. `install.bat` already applies these;
  the file is for re-applying by hand. Undo: `harden-undo.reg`.

- **`debloat.reg`** — the visual layer: window/menu/tooltip animations and
  fades, Aero Snap / Snap Assist / Aero Shake / Aero Peek, transparency,
  accent colour on borders, toasts and "suggested content", the error beep.
  All per-user (HKCU), no admin, fully reversible with `debloat-undo.reg`.
  Double-click to apply, then sign out and back in for everything to settle.

- **`services.reg`** — machine-wide (HKLM), needs **admin + a reboot**. Turns
  off the lock screen and startup sound and disables background services a
  minimal shell doesn't use (telemetry, Xbox, Maps, geolocation, biometrics,
  fax, …). **Read it first** and delete any block you actually need — an
  AGGRESSIVE section (Search, Print Spooler, SysMain) is left commented out for
  you to opt into. Right-click → **Merge** (accept UAC), or from an elevated
  prompt `reg import services.reg`. Undo: `services-undo.reg` (also elevated).

Before cutting services, see what's actually running on your machine so you
can tune the list:

```
powershell -c "Get-Service | ? Status -eq 'Running' | sort DisplayName | ft -Auto"
```

## Recovery — if the screen is black / mshell misbehaves

`Ctrl+Shift+Esc` opens **Task Manager**, which works even with no shell and
even if mshell has grabbed the keyboard hook. From there:

- **File → Run new task → `explorer.exe`** — get a temporary normal desktop
  back immediately (doesn't undo the install, just launches Explorer for now).
- **File → Run new task → `regedit`** — go to
  `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon` and delete the
  `Shell` value to permanently revert.
- **File → Run new task → `cmd`**, then run `uninstall.bat`.
- If mshell is stuck, find `mshell.exe` in the Details tab and End Task; your
  Win key comes back.

Keep a **second administrator account** around as a belt-and-suspenders
fallback: the HKCU override is per-user, so you can always log into the other
account (which still has Explorer) and fix things.

## Uninstall

Run **`uninstall.bat`**, or delete the `Shell` value under
`HKCU\...\Winlogon`. Sign out and back in — Explorer returns. To also disable
elevated running, restore UAC:

```
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" ^
    /v EnableLUA /t REG_DWORD /d 1 /f
```
(reboot required)
