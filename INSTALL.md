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

### The privileged helper (recommended instead of elevating mshell)

`mshelld.exe` ships alongside mshell and exists to make elevating mshell
unnecessary. It is elevated; mshell is not. It has **no config file, no Lua, no
scripting, no window rules and no keyboard hook** — it accepts four requests:
"put this window at this rectangle", "put this window in or out of the
always-on-top band", "cloak or uncloak this window" and "post this window a
WM_CLOSE". Every decision stays in the unelevated shell.

With it running, an unelevated mshell tiles windows owned by elevated processes
(Task Manager, regedit, an admin terminal) instead of leaving them floating —
and, just as importantly, *hides* them when you switch desktops: cloaking a
window is blocked by the same integrity check as moving it, so without the
helper an elevated window is visible on every desktop at once.

**And cloaking is not only about elevated windows.** DWM refuses
`DWMWA_CLOAK` on *any* window an unelevated process does not own, ordinary
same-user windows included, so with no helper running the default `"cloak"`
hide policy is not what actually happens: every desktop switch falls back to
`ShowWindow(SW_HIDE)`. Chromium-based apps (Chrome, Edge, Electron) rebuild
their compositor in the wrong place after that — the page walks further into
the window on every switch, with the app's own frame colour filling the gap,
until the app is restarted. If you switch desktops with a browser open, install
the helper.

It is also what
lets `float_on_top` hold for such a window: floats are kept above the grid by
putting them in the always-on-top band, that call meets the same check, and
without the helper an elevated float is the one window a click can still bury.
The close
keybind reaches them too. And your `init.lua` is never administrator-level
code.

`install.bat` always installs `mshelld.exe` next to `mshell.exe` in `C:\mshell`,
and always replaces it in step with the shell — the two shake hands on a
protocol version and refuse a mismatch, so upgrading one without the other is a
supported way to break the helper. Installing the file is not the same as
running it, though: that part is opt-in, because it needs a logon task with
administrator rights.

To register that task, re-run the installer **from an administrator prompt**:

```
install.bat /helper
```

It creates the task below, and starts the helper straight away rather than
making you sign out. On later upgrades you do not need the flag again — once the
task exists, `install.bat` restarts the helper so the build you just installed
is the one running.

If you would rather do it by hand, this is the same command. A logon task is
what gives the helper a full token without weakening UAC:

```
schtasks /create /tn "mshelld" /tr "C:\mshell\mshelld.exe" ^
         /sc onlogon /rl highest /f
```

mshell finds it automatically; nothing needs configuring. If it is not running,
mshell behaves exactly as it always has — those windows float and stay visible
on every desktop — so this is entirely opt-in. `%TEMP%\mshelld.log` is the
helper's own log if you need to see whether it started.

**What it does not do:** keybinds still stop responding while an elevated window
has *focus*. That needs the keyboard hook itself to be elevated, and the hook was
deliberately left in mshell — moving it would mean putting the whole keymap
state machine (submaps, leader, exit keys) inside the elevated process as
config-derived data, which is a far larger surface than a list of rectangles and
defeats the point of keeping the privileged half dumb. See `src/proto.h`.

So: if you want elevated windows *tiled and desktop-bound*, install the helper.
If you additionally want keybinds to work while an elevated window has focus,
that is the one remaining reason to elevate mshell itself — and the trade-off
below applies.

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

The recommended option above — leave UAC on, let the helper do the privileged
work — avoids all of this: the elevated half is a small binary with no config
and no scripting, and `init.lua` stays unprivileged.

## 5. Turn off the rest of Windows (optional, recommended)

mshell itself already eats every `Win`+* combo, Alt+Tab / Alt+Esc / Ctrl+Esc,
strips title bars, and — baked into `mshell.exe` (`window.c`) — forces square
corners and kills the per-window open/close/minimise animation on every
window it manages. Everything else Windows adds lives in the registry, split
into three importable files by blast radius:

- **`harden.reg`** — keyboard shortcuts the low-level hook can't reach: Win+L
  lock, the Sticky / Filter / Toggle / High-Contrast / Mouse-Keys hotkeys, and
  bare PrtScn → Snip. All per-user (HKCU), and `install.bat` already applies
  them — as `mshell.exe --tweaks apply input`, one value at a time, which also
  records what each was so the uninstall can put *your* value back rather than
  Microsoft's default. The file is for applying the same set without mshell.

  **Two of them need an administrator prompt**, and they are the two under
  `HKCU\…\CurrentVersion\Policies\` — the blanket Win-key hotkey policy and
  Win+L. Windows ACLs that subtree read-only for the user who owns the hive, so
  an ordinary per-user install applies the other nine and skips those, and says
  so. Re-run `install.bat` elevated (or right-click → Merge `harden.reg` and
  accept UAC) to pick them up. Undo: `harden-undo.reg`, or `mshell.exe
  --tweaks revert input`.

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
`HKCU\...\Winlogon`. Sign out and back in — Explorer returns.

`uninstall.bat` also removes the `mshelld` logon task, so nothing keeps starting
an elevated binary out of `C:\mshell` after you delete that folder. That step
needs an administrator prompt — the task was created with `/rl highest` — and
the script tells you if it could not do it:

```
schtasks /delete /tn "mshelld" /f
```

To also disable elevated running, restore UAC:

```
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" ^
    /v EnableLUA /t REG_DWORD /d 1 /f
```
(reboot required)
