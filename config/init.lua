--[[
    mshell — default configuration.

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

local mod, shft, ctrl = "LWin", "Shift", "Ctrl"

local TERMINALS = { "wt.exe", "pwsh.exe", "cmd.exe" }

local function find_app(name)
    local roots = {
        (os.getenv("ProgramFiles") or "C:\\Program Files") .. "\\mshell\\" .. name,
        (os.getenv("LOCALAPPDATA") or "") .. "\\Programs\\" .. name,
    }
    for _, root in ipairs(roots) do
        local path = root .. "\\" .. name .. ".exe"
        local f = io.open(path, "rb")
        if f then
            f:close()
            return path
        end
    end
end

local MRUN     = find_app("mrun")
local MNOTIFY  = find_app("mnotify")
local MCAPTURE = find_app("mcapture")

local function terminal()
    for _, exe in ipairs(TERMINALS) do
        if mshell.exec(exe) then return end
    end
end

local function launcher()
    if not (MRUN and mshell.exec(MRUN)) then mshell.launcher.open() end
end

local function capture(args)
    return function()
        if MCAPTURE then mshell.exec(MCAPTURE, args) else mshell.exec("ms-screenclip:") end
    end
end

local function notifications(args)
    return function()
        if MNOTIFY then
            mshell.exec(MNOTIFY, args)
        else
            mshell.notify("mnotify is not installed — re-run the mshell installer to add it", "warn")
        end
    end
end

local function go(name)     return function() mshell.desktop.focus(name) end end
local function send(name)   return function() mshell.window.move.to_desktop(name) end end
local function follow(name)
    return function()
        mshell.window.move.to_desktop(name)
        mshell.desktop.focus(name)
    end
end

mshell.layout.gaps(6, 6)
mshell.layout.smart_gaps(true)
mshell.layout.set("tiling")
mshell.layout.master.ratio(0.55)

mshell.appearance.border {
    width        = 2,
    focused      = 0x89b4fa,
    accent       = "bottom",
    accent_width = 3,
}
mshell.appearance.background(0x1e1e2e)

mshell.bar.setup({
    enabled  = true,
    mode     = "top_bar",
    position = "top",
    height   = 28,
    bg       = 0x1e1e2e,
    fg       = 0xcdd6f4,
    accent   = 0x89b4fa,
    dim      = 0x7f849c,
    modules  = { "desktops", "layout", "title", "clock", "notifications" },
})

mshell.window.rule({ dialog = true }, "float")
mshell.window.rule({ process = "consent.exe"            }, "float")
mshell.window.rule({ process = "CredentialUIBroker.exe" }, "float")
mshell.window.rule({ process = "Taskmgr.exe"            }, "float")
mshell.window.rule({ process = "SnippingTool.exe"       }, "float")

mshell.keys.bind({mod},       "h", mshell.window.focus.left)
mshell.keys.bind({mod},       "j", mshell.window.focus.down)
mshell.keys.bind({mod},       "k", mshell.window.focus.up)
mshell.keys.bind({mod},       "l", mshell.window.focus.right)
mshell.keys.bind({mod, shft}, "h", mshell.window.move.left)
mshell.keys.bind({mod, shft}, "j", mshell.window.move.down)
mshell.keys.bind({mod, shft}, "k", mshell.window.move.up)
mshell.keys.bind({mod, shft}, "l", mshell.window.move.right)
mshell.keys.bind({mod, ctrl}, "h", mshell.layout.master.ratio.shrink)
mshell.keys.bind({mod, ctrl}, "l", mshell.layout.master.ratio.grow)
mshell.keys.bind({mod, ctrl}, "j", mshell.layout.cfact.shrink)
mshell.keys.bind({mod, ctrl}, "k", mshell.layout.cfact.grow)

for i = 1, 9 do
    local name = tostring(i)
    mshell.keys.bind({mod},       name, go(name),     { desc = name })
    mshell.keys.bind({mod, shft}, name, send(name),   { desc = "send → " .. name })
    mshell.keys.bind({mod, ctrl}, name, follow(name), { desc = "take → " .. name })
end
mshell.keys.bind({mod}, "Tab", mshell.desktop.focus.last)
mshell.keys.bind({mod}, "]",   mshell.desktop.focus.next)
mshell.keys.bind({mod}, "[",   mshell.desktop.focus.prev)

mshell.keys.bind({mod},       ",", mshell.monitor.focus.prev)
mshell.keys.bind({mod},       ".", mshell.monitor.focus.next)
mshell.keys.bind({mod, shft}, ",", mshell.window.move.to_monitor.prev)
mshell.keys.bind({mod, shft}, ".", mshell.window.move.to_monitor.next)

mshell.keys.bind({mod},       "Return", terminal, { desc = "terminal" })
mshell.keys.bind({mod},       "Space",  launcher, { desc = "launcher" })
mshell.keys.bind({mod, shft}, "Return", mshell.layout.master.zoom)
mshell.keys.bind({mod, shft}, "Space",  mshell.layout.cycle)

mshell.keys.bind({mod},       "q",   mshell.window.close)
mshell.keys.bind({mod, shft}, "q",   mshell.window.kill)
mshell.keys.bind({mod},       "f",   mshell.window.float.toggle)
mshell.keys.bind({mod, shft}, "f",   mshell.window.fullscreen.window)
mshell.keys.bind({mod, ctrl}, "f",   mshell.window.fullscreen.content)
mshell.keys.bind({mod},       "F11", mshell.window.fullscreen.both)
mshell.keys.bind({mod},       "t",   mshell.layout.tiling)
mshell.keys.bind({mod},       "m",   mshell.layout.monocle)
mshell.keys.bind({mod},       "b",   mshell.bar.toggle)
mshell.keys.bind({mod},       "`",   mshell.window.scratchpad.toggle)
mshell.keys.bind({mod, shft}, "`",   mshell.window.scratchpad.mark)

mshell.keys.bind({mod, shft}, "s",           capture("--select"), { desc = "screenshot: region" })
mshell.keys.bind({mod},       "PrintScreen", capture("--screen"), { desc = "screenshot: screen" })
mshell.keys.bind({mod},       "n",           notifications("--history"), { desc = "notifications" })
mshell.keys.bind({mod, shft}, "n",           notifications("--tray"),    { desc = "tray icons" })

mshell.keys.bind({mod}, "VolumeUp",   mshell.media.volume.up)
mshell.keys.bind({mod}, "VolumeDown", mshell.media.volume.down)
mshell.keys.bind({mod}, "VolumeMute", mshell.media.volume.mute)
mshell.keys.bind({mod}, "MediaPlay",  mshell.media.play)
mshell.keys.bind({mod}, "MediaNext",  mshell.media.next)
mshell.keys.bind({mod}, "MediaPrev",  mshell.media.prev)

mshell.keys.bind({mod, shft}, "r", mshell.config.reload)
mshell.keys.bind({mod, shft}, "u", mshell.config.update)

local go_keys, send_keys = {}, {}
for i = 1, 9 do
    local name = tostring(i)
    go_keys[name]   = { go(name),   desc = name }
    send_keys[name] = { send(name), desc = "→ " .. name }
end
go_keys.Tab  = mshell.desktop.focus.last
go_keys["]"] = mshell.desktop.focus.next
go_keys["["] = mshell.desktop.focus.prev
go_keys[","] = mshell.monitor.focus.prev
go_keys["."] = mshell.monitor.focus.next
send_keys[","] = mshell.window.move.to_monitor.prev
send_keys["."] = mshell.window.move.to_monitor.next

mshell.keys.submap("go",   go_keys)
mshell.keys.submap("send", send_keys)

mshell.keys.submap("window", {
    q = mshell.window.close,
    k = mshell.window.kill,
    f = mshell.window.float.toggle,
    c = { function() mshell.window.center() end, desc = "center" },
    s = mshell.window.sticky.toggle,
    t = mshell.window.on_top.toggle,
    z = mshell.layout.master.zoom,
    p = mshell.layout.master.promote,
    w = mshell.window.fullscreen.window,
    i = mshell.window.fullscreen.content,
    a = mshell.window.fullscreen.both,
    m = mshell.window.scratchpad.mark,
    u = mshell.window.urgent.jump,
})

mshell.keys.submap("layout", {
    t     = mshell.layout.tiling,
    m     = mshell.layout.monocle,
    g     = mshell.layout.grid,
    s     = mshell.layout.spiral,
    c     = mshell.layout.centered,
    b     = mshell.layout.bstack,
    o     = mshell.layout.columns,
    Space = mshell.layout.cycle,
    ["="] = mshell.layout.master.count.inc,
    ["-"] = mshell.layout.master.count.dec,
})

mshell.keys.submap("resize", {
    h     = mshell.layout.master.ratio.shrink,
    l     = mshell.layout.master.ratio.grow,
    j     = mshell.layout.cfact.shrink,
    k     = mshell.layout.cfact.grow,
    r     = mshell.layout.cfact.reset,
    Left  = mshell.window.resize.left,
    Down  = mshell.window.resize.down,
    Up    = mshell.window.resize.up,
    Right = mshell.window.resize.right,
}, { persist = true })

mshell.keys.submap("apps", {
    t = { terminal,                    desc = "terminal" },
    r = { launcher,                    desc = "launcher" },
    s = { capture("--select"),         desc = "screenshot: region" },
    w = { capture("--window"),         desc = "screenshot: window" },
    f = { capture("--screen"),         desc = "screenshot: screen" },
    n = { notifications("--history"),  desc = "notifications" },
    y = { notifications("--tray"),     desc = "tray icons" },
})

mshell.keys.submap("media", {
    k = mshell.media.volume.up,
    j = mshell.media.volume.down,
    m = mshell.media.volume.mute,
    p = mshell.media.play,
    l = mshell.media.next,
    h = mshell.media.prev,
}, { persist = true })

mshell.keys.submap("session", {
    l = mshell.system.lock,
    s = mshell.system.sleep,
    h = mshell.system.hibernate,
    o = mshell.system.logoff,
    r = mshell.system.reboot,
    p = mshell.system.shutdown,
})

mshell.keys.submap("mshell", {
    r = mshell.config.reload,
    u = mshell.config.update,
    s = mshell.config.restart,
    h = mshell.config.restart_helper,
    b = mshell.bar.toggle,
    t = mshell.bar.top,
    f = mshell.bar.floating,
    p = mshell.config.panic,
    q = mshell.config.quit,
})

mshell.keys.submap("leader", {
    g      = "go",
    m      = "send",
    w      = "window",
    l      = "layout",
    r      = "resize",
    a      = "apps",
    v      = "media",
    s      = "session",
    x      = "mshell",
    Space  = { launcher, desc = "launcher" },
    Return = { terminal, desc = "terminal" },
    Tab    = mshell.desktop.focus.last,
})

mshell.keys.leader("leader")

if MNOTIFY then mshell.exec.startup(MNOTIFY) end
if MRUN    then mshell.exec.startup(MRUN, "--daemon") end
