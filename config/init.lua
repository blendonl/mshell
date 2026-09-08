--[[
    mshell — default configuration.

    Deliberately small, and it assumes nothing is installed but Windows itself.
    Everything here works on a bare machine; change the terminal on the first
    line below and you have your setup.

    A much larger, heavily commented example lives beside this file as
    init.full.lua — leader menus, per-desktop auto-launch, game rules,
    which-key styling, event handlers. Read it when you want more; copy it over
    this file if you want all of it.

    Location:  %APPDATA%\mshell\init.lua
    Reload:    save the file (auto-reload), or press Win+Shift+R.
    Safety:    a syntax error keeps the PREVIOUS config running and writes the
               reason to %LOCALAPPDATA%\mshell\mshell.log — a bad edit cannot
               strand you.

    Editor:    mshell ships the type definitions for everything below in
               %APPDATA%\mshell\meta, and a .luarc.json beside this file that
               points lua-language-server at them. Open this folder in an
               editor with that LSP and you get completion, signatures and a
               warning on anything mshell does not have.
--]]

-- The one thing worth changing first. cmd.exe is used because it is the only
-- terminal guaranteed to exist on every Windows install; swap in
-- "wt.exe", "alacritty.exe", "pwsh.exe", … once you know what you have.
local TERMINAL = "cmd.exe"

local mod, shft, ctrl = "LWin", "Shift", "Ctrl"

----------------------------------------------------------------------
-- Appearance
----------------------------------------------------------------------
mshell.layout.gaps(6, 6)               -- between windows, at the screen edge
mshell.appearance.border {             -- focus ring plus a bar on one edge, because
    width  = 2,                        -- a 2px ring at the screen edge is easy to miss
    focused = 0xffffff,                -- 0xRRGGBB
    accent = "bottom",                 -- "top" | "bottom" | "none"
    accent_width = 3,
}
mshell.appearance.background(0x1e1e2e) -- desktop backdrop (there is no wallpaper)
mshell.layout.set("tiling")            -- tiling|monocle|grid|spiral|centered|bstack|columns
mshell.layout.master.ratio(0.60)

-- Status bar. There is no taskbar, and because desktops are created and
-- destroyed as you use them, this is the only thing telling you which ones
-- exist and which you are on.
--
-- mode = "top_bar" is a strip on every monitor that reserves its space, so
-- tiled windows sit below it while a fullscreen window still covers it.
-- mode = "floating" is instead one panel in the middle of the focused monitor
-- — big clock, date, desktops, and mshell's notifications listed inline — that
-- floats over the windows and reserves nothing. Bind mshell.bar.toggle to get
-- it out of the way.
mshell.bar.setup({
    enabled  = true,
    mode     = "top_bar",    -- or "floating"
    position = "top",        -- or "bottom" (top_bar mode)
    height   = 28,           -- scaled per monitor for DPI; sets the type scale
    bg       = 0x1e1e2e,
    fg       = 0xcdd6f4,
    accent   = 0x7aa2f7,     -- the desktop you're on
    dim      = 0x6c7086,     -- the others
    -- "notifications" is floating-only: with it on, the panel is where
    -- mshell's messages appear instead of as separate toasts.
    modules  = { "desktops", "layout", "title", "clock", "notifications" },
})

----------------------------------------------------------------------
-- Window rules
----------------------------------------------------------------------
-- Float anything Windows considers a dialog — file pickers, message boxes,
-- permission prompts. They carry no class or process of their own (a file
-- picker IS the host app's process), so this is the only way to name them.
mshell.window.rule({ dialog = true }, "float")

-- Credential and UAC prompts are their own processes and are not dialogs in
-- the sense above, so they need naming.
mshell.window.rule({ process = "consent.exe"            }, "float")
mshell.window.rule({ process = "CredentialUIBroker.exe" }, "float")

----------------------------------------------------------------------
-- Sub-maps
----------------------------------------------------------------------
-- Bar sub-map (one-shot): switch bar mode then back to root.
mshell.keys.submap("bar", {
    t = mshell.bar.top,       -- top-bar strip on every monitor
    f = mshell.bar.floating,  -- floating panel in the centre
})

-- Extra sub-map (one-shot): Win+x opens it, then pick a category. A submap is
-- entered by naming it, which is why "bar" is a plain string here.
mshell.keys.submap("extra", {
    b = "bar",   -- bar mode: Win+x b t / Win+x b f
})

----------------------------------------------------------------------
-- Keybindings — hold Win and press a key
--
-- The third argument is the action itself, not its name: mshell.window.close
-- rather than "close". Your editor completes them, and a typo is caught as you
-- write it instead of when the config loads.
--
-- Anything that needs telling WHAT to act on goes inside a function, which is
-- why the desktop keys below are written the way they are. A function has no
-- name for the which-key panel to show, so give those a desc.
----------------------------------------------------------------------
-- focus / move, vim keys
mshell.keys.bind({mod},       "h", mshell.window.focus.left)
mshell.keys.bind({mod},       "j", mshell.window.focus.down)
mshell.keys.bind({mod},       "k", mshell.window.focus.up)
mshell.keys.bind({mod},       "l", mshell.window.focus.right)
mshell.keys.bind({mod, shft}, "h", mshell.window.move.left)
mshell.keys.bind({mod, shft}, "j", mshell.window.move.down)
mshell.keys.bind({mod, shft}, "k", mshell.window.move.up)
mshell.keys.bind({mod, shft}, "l", mshell.window.move.right)

-- desktops. These are NAMES, not slots: Win+3 goes to a desktop called "3",
-- which is created when you go there and destroyed when you leave it empty.
-- Any name works, whether or not it appears in this file.
for i = 1, 9 do
    local name = tostring(i)
    mshell.keys.bind({mod}, name,
        function() mshell.desktop.focus(name) end, { desc = name })
    mshell.keys.bind({mod, shft}, name,
        function() mshell.window.move.to_desktop(name) end, { desc = "→ " .. name })
end
mshell.keys.bind({mod}, "`", mshell.desktop.focus.last)   -- back where you came from
mshell.keys.bind({mod}, "]", mshell.desktop.focus.next)   -- step through what exists now
mshell.keys.bind({mod}, "[", mshell.desktop.focus.prev)

-- monitors
mshell.keys.bind({mod},       ",", mshell.monitor.focus.prev)
mshell.keys.bind({mod},       ".", mshell.monitor.focus.next)
mshell.keys.bind({mod, shft}, ",", mshell.window.move.to_monitor.prev)
mshell.keys.bind({mod, shft}, ".", mshell.window.move.to_monitor.next)

-- layout
mshell.keys.bind({mod}, "t",     mshell.layout.tiling)
mshell.keys.bind({mod}, "m",     mshell.layout.monocle)
mshell.keys.bind({mod}, "g",     mshell.layout.grid)
mshell.keys.bind({mod}, "Space", mshell.layout.cycle)
mshell.keys.bind({mod}, "f",     mshell.window.float.toggle)
mshell.keys.bind({mod}, "Return",mshell.layout.master.promote)
mshell.keys.bind({mod}, "b",     mshell.bar.toggle)   -- hide/show the status bar
mshell.keys.bind({mod}, "x",     "extra")             -- Win+x b t/f: bar mode

-- master area: ratio (Ctrl+h/l), count (Ctrl+j/k)
mshell.keys.bind({mod, ctrl}, "h", mshell.layout.master.ratio.shrink)
mshell.keys.bind({mod, ctrl}, "l", mshell.layout.master.ratio.grow)
mshell.keys.bind({mod, ctrl}, "j", mshell.layout.master.count.dec)
mshell.keys.bind({mod, ctrl}, "k", mshell.layout.master.count.inc)

-- fullscreen. Three flavours, because the WINDOW and the app's own CONTENT
-- fullscreen (YouTube's button, F11) are different things:
mshell.keys.bind({mod, shft}, "f",   mshell.window.fullscreen.window)   -- covers the monitor
mshell.keys.bind({mod, ctrl}, "f",   mshell.window.fullscreen.content)  -- stays in the tile
mshell.keys.bind({mod},       "F11", mshell.window.fullscreen.both)     -- covers the display

-- windows. Note restore: there is no taskbar under mshell, so it is the only
-- way to bring a minimized window back.
mshell.keys.bind({mod, shft}, "c", mshell.window.close)
mshell.keys.bind({mod, shft}, "x", mshell.window.kill)
mshell.keys.bind({mod},       "n", mshell.window.minimize)
mshell.keys.bind({mod, shft}, "n", mshell.window.restore)
mshell.keys.bind({mod},       "s", mshell.window.sticky.toggle) -- to every desktop
mshell.keys.bind({mod},       "z", mshell.layout.master.zoom)   -- swap with master

-- Scratchpad: one window you can summon anywhere and dismiss again. Mark a
-- window once (Win+Shift+s), then Win+Ctrl+Space toggles it wherever you are.
mshell.keys.bind({mod, shft}, "s",     mshell.window.scratchpad.mark)
mshell.keys.bind({mod, ctrl}, "Space", mshell.window.scratchpad.toggle)

-- programs
mshell.keys.bind({mod, shft}, "Return",
    function() mshell.exec(TERMINAL) end, { desc = "terminal" })

-- mshell itself
mshell.keys.bind({mod, shft}, "r", mshell.config.reload)
mshell.keys.bind({mod, shft}, "q", mshell.config.quit)   -- as the shell, this logs you out

-- Install the latest GitHub release: downloads it, checks it against the hash
-- GitHub published, and runs the install.bat inside it — which restarts mshell.
mshell.keys.bind({mod, shft}, "u", mshell.config.update)

----------------------------------------------------------------------
-- Leader mode — tap Win on its own, then press bare keys
----------------------------------------------------------------------
-- A submap is persist = true (stay until Esc) or one-shot (next key drops
-- back). This one persists, so h/j/k/l can be pressed repeatedly.
mshell.keys.submap("normal", {
    h = mshell.window.focus.left,
    j = mshell.window.focus.down,
    k = mshell.window.focus.up,
    l = mshell.window.focus.right,
    Space  = mshell.layout.cycle,
    Return = mshell.layout.master.promote,
    f      = mshell.window.float.toggle,
    c      = mshell.window.close,
    t      = { function() mshell.exec(TERMINAL) end, desc = "terminal" },
}, { persist = true })

mshell.keys.leader("normal")   -- a bare Win tap enters it; Esc or Win leaves

----------------------------------------------------------------------
-- Startup
----------------------------------------------------------------------
-- Nothing is launched by default — you may not have any of the usual
-- suspects installed. Add what you want:
--
--   mshell.exec.startup("alacritty.exe")
--   mshell.exec.startup("wt.exe", "-p Ubuntu")   -- arguments are a second string
