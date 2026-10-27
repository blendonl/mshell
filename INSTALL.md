# Installing mshell as your Windows shell

This replaces `explorer.exe` — no taskbar, no tray, no desktop. Read the
**Recovery** section before you start; a broken shell is easy to undo once
you know the Task Manager trick.

## 1. Run the installer

Download `mshell-<version>.msi` from the
[releases page](https://github.com/blendonl/mshell/releases) (or use
`winget install blendonl.mshell` once the package is listed) and run it **from
the account that will use mshell** — the shell setting and the Windows tweaks
are per-user, and they go to whoever ran the installer. Windows asks for
administrator approval once.

The *Custom Setup* page lists what it can install. Everything is on by default:

| Option | What it does |
|--------|--------------|
| **mshell** | `mshell.exe`, the helper binary, the default config and the editor type definitions, in `C:\Program Files\mshell`. Always installed. |
| **Use mshell as my Windows shell** | Sets the per-user (HKCU) Winlogon `Shell` value. Turn it off to try mshell in test mode first. |
| **Privileged helper** | Registers the `mshelld` sign-in task, so windows of elevated apps tile and follow their desktop. See [The privileged helper](#the-privileged-helper-recommended-instead-of-elevating-mshell). |
| **Windows tweaks** | Hands `Win`+key and PrintScreen to mshell, silences the Sticky/Filter/Toggle Keys prompts, and keeps Chrome and Edge drawing while hidden. See [section 5](#5-turn-off-the-rest-of-windows-optional-recommended). |
| **mrun** | The launcher behind `Win+Space`. Without it, mshell's built-in launcher is used. |
| **mnotify** | App notifications and tray icons, which Explorer would otherwise draw. |
| **mcapture** | Screenshots on `Win+Shift+s` / `Win+PrintScreen`. Without it, Windows' snipping overlay is used. |

The default config adapts to whichever of these you picked, so turning one off
never leaves a key bound to a program that is not there. Run the MSI again (or
*Modify* it under *Apps*) to add or remove one later.

Unattended, the feature ids are `Core`, `Shell`, `Helper`, `Tweaks`, `Mrun`,
`Mnotify` and `Mcapture`:

```
msiexec /i mshell-<version>.msi /passive
msiexec /i mshell-<version>.msi /passive ADDLOCAL=Core,Shell,Helper,Mrun
```

Your config lives at `%APPDATA%\mshell\init.lua`. The installer does not write
it: mshell copies the shipped default there the first time it starts without
one, so every account on the machine gets its own, and an existing `init.lua`
is never touched. That first start shows a notification with the keys to
begin with: tap `Win` for the menu, hold it for the chords. The editor type definitions beside it are refreshed on every
start, so they always describe the build you are running.

## 2. Try it WITHOUT committing (strongly recommended)

Turn **Use mshell as my Windows shell** off, and on the last page tick *Try
mshell now in test mode*. Or, any time:

```
"C:\Program Files\mshell\mshell.exe" --test
```

Tile some windows, switch desktops (`Win+1..9`), open a terminal
(`Win+Return`), tap `Win` alone for the leader menu. Quit from the leader menu,
`x` then `q` — in `--test` that just closes the process, it does NOT log you
out. If this feels right, run the MSI again and turn the shell option on.

## 3. Sign out and back in

mshell is now your shell. Explorer never launches. This is **per-user**: it only
affects the account that ran the installer, and overrides the system default
(`explorer.exe`) just for you:

```
HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon
    Shell = "C:\Program Files\mshell\mshell.exe" --shell
```

If Windows asked for a *different* administrator's password rather than a
Yes/No, the installer ran as that account and set *its* shell. Sign in as the
administrator, or run the MSI from an account that is an administrator itself.

### Upgrading

Run a newer MSI, or press `Win+Shift+u` to have mshell fetch, verify and run
the latest one for you. Either way:

- **The exe is replaced even while it is your running shell.** Windows locks
  the image of a running process, so the installer renames it — and the
  helper, mrun, mnotify and mcapture — to `*.old` and drops the new one in its
  place. Nothing waits for a reboot.
- **Then mshell hands over to the new build** — `Win+Shift+u` does it as soon as
  the installer finishes, and an MSI you ran yourself does it when you press
  *Finish*. Your windows are left where they are; the new instance picks them
  up and starts mrun and mnotify again. `mshell.exe --msg restart` does the
  same by hand.
- **Your choices carry over**, and so does your config — an existing
  `%APPDATA%\mshell\init.lua` is never overwritten. When the release changes
  the default and yours differs from it, the new default is copied to
  `%APPDATA%\mshell\init.lua.new` and a notification says so; diff the two
  and take what you want. It is written once per new default, so deleting it
  keeps it gone.

The hand-over works by mshell exiting: Windows then puts the shell back from
the `Shell` value above. That safety net is Winlogon's `AutoRestartShell`
(`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`, `1` unless
someone turned it off). **If it is `0`, a shell that exits logs the session
off**, so mshell checks it first and tells you to sign out instead.

### Coming from `install.bat` (0.15 and earlier)

Earlier releases installed to `C:\mshell` with a batch script. The MSI points
your `Shell` value and the `mshelld` task at `C:\Program Files\mshell` and
leaves the old folder alone — sign in once on the new build, then delete
`C:\mshell`. Tweaks applied by the old script stay recorded, so
`mshell.exe --tweaks revert input apps` still puts your previous values back.

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
  account, that starts `"C:\Program Files\mshell\mshell.exe" --shell`, and point the Winlogon
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
scripting, no window rules and no keyboard hook** — it accepts three requests:
"put this window at this rectangle", "put this window in or out of the
always-on-top band" and "post this window a WM_CLOSE". Every decision stays in
the unelevated shell.

With it running, an unelevated mshell tiles windows owned by elevated processes
(Task Manager, regedit, an admin terminal) instead of leaving them floating —
and, just as importantly, *hides* them when you switch desktops: an elevated
window can't be sunk under the backdrop, so it is parked off every display
instead, and that move meets the same integrity check as tiling it. Without the
helper an elevated window is visible on every desktop at once.

The helper does not cloak anything. DWM only lets a window's owner cloak it, so
no other process can — elevated or not. Ordinary windows don't need the helper
to be hidden: they are sunk under the desktop backdrop (see "How a desktop is
taken off the screen" in the README).

It is also what
lets `float_on_top` hold for such a window: floats are kept above the grid by
putting them in the always-on-top band, that call meets the same check, and
without the helper an elevated float is the one window a click can still bury.
The close
keybind reaches them too. And your `init.lua` is never administrator-level
code.

The MSI always installs `mshelld.exe` next to `mshell.exe`, and always replaces
it in step with the shell — the two shake hands on a protocol version and
refuse a mismatch, so upgrading one without the other is a supported way to
break the helper. Installing the file is not the same as running it, though:
that needs a logon task with administrator rights, which is what the
**Privileged helper** option registers. It starts the helper straight away
rather than making you sign out, and restarts it on every upgrade so the build
you just installed is the one running. Turning the option off (or uninstalling)
removes the task.

If you would rather do it by hand, this is the same command. A logon task is
what gives the helper a full token without weakening UAC:

```
schtasks /create /tn "mshelld" /tr "\"C:\Program Files\mshell\mshelld.exe\"" ^
         /sc onlogon /rl highest /ru %USERNAME% /it /f
```

mshell finds it automatically; nothing needs configuring. If it is not running,
mshell behaves exactly as it always has — those windows float and stay visible
on every desktop — so leaving the option off is safe. `%TEMP%\mshelld.log` is the helper's own log if you need to see whether it
started.

**Do not change `/ru` to `SYSTEM`.** The helper's pipe is restricted to the
user it runs as, and the command above names *you* — elevated, but still you,
which is why your unelevated mshell can talk to it.
Running it as SYSTEM grants the pipe to SYSTEM and no shell will ever connect;
it would also put the helper in session 0, where it cannot touch your windows at
all. The log says which identity it granted on the line above `listening on`, so
if mshell is not connecting, start there.

**What running it trusts.** The helper is elevated and does what it is asked —
place a window, band it topmost, close it — for any window, including
ones owned by elevated processes. That is the whole point of it: tiling an
administrator's window means being able to move an administrator's window.
mshelld only accepts requests from the `mshell.exe` sitting in its own
directory — and, if `mshelld.exe` is Authenticode-signed, only when that
`mshell.exe` is signed by the same certificate — and logs every refusal to
`%TEMP%\mshelld.log`, so other programs running under your account cannot just
open the pipe and drive it. But those checks identify a file, not an intention:
anything running as you that can relaunch that `mshell.exe` or inject into it
gets the same reach, and on an unsigned build so can anything that can replace
it. The boundary the helper draws is your login session, not administrator — so
keep the install directory writable by administrators only, and that is the
whole of it. `C:\Program Files\mshell` is exactly that by default.

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

### uiAccess (advanced — removes the last UIPI gap without elevating)

There is a third route that needs neither an elevated mshell nor the helper:
build mshell with the **uiAccess** manifest. A uiAccess process may bypass the
UIPI restrictions that stop an unelevated shell from touching elevated windows,
so a single unelevated mshell can tile, restore, re-style and close them **and**
its keyboard hook keeps working while an elevated window has focus — the one
thing the helper cannot do. It stays unelevated, so `init.lua` stays
unprivileged and auto-reload keeps working.

Windows guards the flag strictly, and these are hard requirements, not
suggestions:

- **The binary must be Authenticode-signed by a certificate the machine
  trusts.** The PKI signature check is always enforced for a uiAccess process.
- **It must live under a protected path** — `C:\Program Files\...` or
  `C:\Windows\System32\...`. The MSI's `C:\Program Files\mshell` qualifies.

If either is not met, Windows refuses the flag — the process may launch without
it, or fail to launch at all. Because being started as the Winlogon shell is an
unusual launch path for a uiAccess binary, **verify it actually worked**: the
startup log says `uiAccess GRANTED` when the flag is live. If it doesn't, you
are running an ordinary build and should keep the helper.

Building it:

```
make UIACCESS=1          # embeds src/mshell.uiaccess.manifest instead of the default
```

then sign `mshell.exe` and put it in place of the installed one in
`C:\Program Files\mshell\`. For a
personal machine you can create your own certificate, trust it machine-wide,
sign with it, and delete the private key afterwards so nothing else can ever be
signed as mshell; a public release needs a real code-signing certificate. The
default `make` build is unchanged and still boots unsigned, so
nothing here is required — it is the maximal-control option for those who want
it.

## 5. Turn off the rest of Windows (optional, recommended)

mshell itself already eats every `Win`+* combo, Alt+Tab / Alt+Esc / Ctrl+Esc,
strips title bars, and — baked into `mshell.exe` (`window.c`) — forces square
corners and kills the per-window open/close/minimise animation on every
window it manages. Everything else Windows adds lives in the registry, split
by blast radius. The installer's **Windows tweaks** option covers the first two
below; the `.reg` files are in `C:\Program Files\mshell\extras`:

- **`harden.reg`** — keyboard shortcuts the low-level hook can't reach: Win+L
  lock, the Sticky / Filter / Toggle / High-Contrast / Mouse-Keys hotkeys, and
  bare PrtScn → Snip. All per-user (HKCU), and the **Windows tweaks** option
  applies them — as `mshell.exe --tweaks apply input`, one value at a time,
  which also records what each was so the uninstall can put *your* value back
  rather than Microsoft's default. The file is for applying the same set without
  mshell.

  **Two of them need administrator rights**, and they are the two under
  `HKCU\…\CurrentVersion\Policies\` — the blanket Win-key hotkey policy and
  Win+L. Windows ACLs that subtree read-only for the user who owns the hive, so
  the installer writes those two itself, with the rights it was given, and
  removes them on uninstall. Undo by hand: `harden-undo.reg` (elevated), or
  `mshell.exe --tweaks revert input`.

- **The browser fix (`--tweaks apply apps`)** — Chromium decides for itself
  whether anyone can see each of its windows and stops drawing the ones it
  thinks are hidden. A tiling shell hides windows constantly, and coming back is
  where it breaks: the window returns with its frame painted and **nothing
  inside it**, one flat grey rectangle, and stays that way until the browser is
  restarted. Nothing repairs it from outside — repaint, resize, re-activate and
  minimise/restore were all measured against such a window and none brought a
  pixel back.

  The **Windows tweaks** option sets the policy that turns that decision off,
  for Chrome, Chromium and Edge. Its key lives under `HKCU\Software\Policies`,
  which Windows ACLs read-only for you, so like the two hardening policies above
  the installer writes it with administrator rights. From an admin prompt at any
  time:

  ```
  "C:\Program Files\mshell\mshell.exe" --tweaks apply apps
  ```

  An Electron app (which has no policy at all)? Put the flag on the
  launch instead — in your shortcut, your launcher, or the `app`/`startup`
  command in `init.lua`:

  ```
  chrome.exe --disable-features=CalculateNativeWinOcclusion
  ```

- **`debloat.reg`** — the visual layer: window/menu/tooltip animations and
  fades, Aero Snap / Snap Assist / Aero Shake / Aero Peek, transparency,
  accent colour on borders, Notification Center and "suggested content", the
  error beep. All per-user (HKCU), no admin, fully reversible with
  `debloat-undo.reg`. Double-click to apply, then sign out and back in for
  everything to settle.

  It leaves Windows notifications on. Earlier copies also set `ToastEnabled`
  to `0` under `HKCU\Software\Microsoft\Windows\CurrentVersion\PushNotifications`,
  and so did `mshell --tweaks apply quiet`. That makes Windows drop every
  app's toast before [mnotify](https://github.com/blendonl/mnotify) can show
  it. If you applied either, delete that value (or run `debloat-undo.reg`),
  then sign out and back in.

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
- **File → Run new task → `ms-settings:appsfeatures`** — *Apps* in Settings,
  where *mshell → Uninstall* puts Explorer back for good.
- If mshell is stuck, find `mshell.exe` in the Details tab and End Task; your
  Win key comes back.

Keep a **second administrator account** around as a belt-and-suspenders
fallback: the HKCU override is per-user, so you can always log into the other
account (which still has Explorer) and fix things.

## Uninstall

Uninstall **mshell** from *Settings → Apps* (`ms-settings:appsfeatures`, or
from the Explorer that `mshell.exe --msg panic` starts), or with the MSI you
installed from:

```
msiexec /x mshell-<version>.msi
```

That removes the `Shell` value, the `mshelld` logon task, the companion apps
and the installer's tweaks — putting your previous values back — and leaves
`%APPDATA%\mshell` (your config) in place; delete it too if you want mshell
fully gone. Sign out and back in — Explorer returns.

If the uninstall ran from another account, remove the leftovers by hand:

```
reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell /f
schtasks /delete /tn "mshelld" /f
```

To also disable elevated running, restore UAC:

```
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" ^
    /v EnableLUA /t REG_DWORD /d 1 /f
```
(reboot required)
