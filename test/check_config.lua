--[[
    check_config.lua — load a config against a mock of the real API.

    `luac -p` only proves a config parses. What actually breaks a config is
    naming something mshell does not have, and that is a runtime error inside
    a file the window manager runs once, on a machine this repository cannot
    build for. So: read src/api_spec.c, which is the single table the binary
    itself dispatches through, build a table with exactly that shape, and run
    the config against it.

    A wrong name, a key bound to something unbindable, or a submap entered
    before it is defined fails here rather than on a Windows box.

        lua check_config.lua src/api_spec.c config/init.lua [more.lua ...]
--]]

local spec_path = assert(arg[1], "usage: check_config.lua <api_spec.c> <config.lua>...")

local function read(path)
    local f = assert(io.open(path, "r"), "cannot read " .. path)
    local s = f:read("a")
    f:close()
    return s
end

----------------------------------------------------------------------
-- Read the spec
----------------------------------------------------------------------
local source = read(spec_path)
local rows_src = source:match("const ApiEntry api_spec%[%] = (.-)\n};")
    or error("no api_spec table in " .. spec_path)

local rows, by_path = {}, {}
for body in rows_src:gmatch("%b{}") do
    local path = body:match('^{%s*"([^"]+)"')
    if path then
        local row = {
            path     = path,
            kind     = body:match("(API_[A-Z]+),") or "API_NAMESPACE",
            callable = body:find("API_CALLABLE", 1, true) ~= nil,
            needsarg = body:find("API_PAYLOAD_STR", 1, true) ~= nil,
        }
        rows[#rows + 1] = row
        by_path[path] = row
    end
end
assert(#rows > 100, "only parsed " .. #rows .. " spec rows — parser is wrong")

local removed_src = source:match("const ApiRemoved api_removed%[%] = (.-)\n};") or ""
local removed = {}
for old, new in removed_src:gmatch('{%s*"([^"]+)",%s*"([^"]+)"%s*}') do
    removed[old] = new
end

----------------------------------------------------------------------
-- Build a table with the same shape
----------------------------------------------------------------------
local ACTION = {}   -- marker: this value is a native action

-- Queries return real data at runtime, and configs iterate it and read fields
-- off it. One value answers every shape of that and keeps a config running far
-- enough to check the calls it makes afterwards: named fields are itself, so
-- w.process and w:close() both resolve, while numeric keys are nil, so ipairs
-- stops at once rather than walking for ever.
local ANY
ANY = setmetatable({}, {
    __index    = function(_, k)
        if type(k) == "number" then return nil end
        return ANY
    end,
    __call     = function() return ANY end,
    __tostring = function() return "<value>" end,
    __concat   = function() return "<value>" end,
})

local mshell = {}
local submaps = {}
local fragments_ok = false
local errors  = {}

local function fail(fmt, ...)
    errors[#errors + 1] = string.format(fmt, ...)
end

local function container(path)
    local t = mshell
    for part in path:gmatch("([^.]+)%.") do
        if rawget(t, part) == nil then rawset(t, part, {}) end
        t = rawget(t, part)
    end
    return t, path:match("([^.]+)$")
end

local function has_children(path)
    local prefix = path .. "."
    for _, other in ipairs(rows) do
        if other.path:sub(1, #prefix) == prefix then return true end
    end
    return false
end

-- Build the shape first. The guards that report a wrong name go on afterwards,
-- because an __index that fires while the tree is still being built would
-- answer the builder's own lookups.
local nodes = {}
for _, row in ipairs(rows) do
    local parent, leaf = container(row.path)
    if rawget(parent, leaf) == nil then rawset(parent, leaf, {}) end
    nodes[#nodes + 1] = { row = row, value = rawget(parent, leaf) }
end

for _, node in ipairs(nodes) do
    local row = node.row
    local returns_data = row.kind == "API_QUERY"
    local mt  = {
        __call   = function() if returns_data then return ANY end end,
        marker   = ACTION,
        path     = row.path,
        action   = row.kind == "API_ACTION",
        needsarg = row.needsarg,
    }
    if row.kind == "API_NAMESPACE" or row.callable or has_children(row.path) then
        mt.__index = function(_, k)
            fail("mshell.%s.%s does not exist", row.path, tostring(k))
            return function() end
        end
    end
    setmetatable(node.value, mt)
end

setmetatable(mshell, {
    __index = function(_, k)
        local now = removed[k]
        if now then fail('mshell.%s was removed — use mshell.%s', k, now)
        else            fail("mshell.%s does not exist", k) end
        return function() end
    end,
})

----------------------------------------------------------------------
-- The parts of the binding layer worth reproducing
----------------------------------------------------------------------
local function check_bound(value, where)
    local mt = type(value) == "table" and getmetatable(value)
    if mt and mt.marker == ACTION then
        if not mt.action then
            fail("%s: mshell.%s is not something a key can do", where, mt.path)
        elseif mt.needsarg then
            fail("%s: mshell.%s needs an argument — wrap it in a function",
                 where, mt.path)
        end
        return
    end
    if type(value) == "function" then return end
    if type(value) == "string" then
        if not submaps[value] and not fragments_ok then
            fail("%s: no submap named '%s'", where, value)
        end
        return
    end
    if type(value) == "table" and value[1] ~= nil then
        -- {action, {desc = ...}} is the shape the old payload form had, and it
        -- fails silently: bind reads desc as a field, so a nested table just
        -- loses the label rather than erroring.
        if type(value[2]) == "table" and value[2].desc ~= nil then
            fail("%s: desc goes beside the action, not in a table of its own "
                 .. "— write { f, desc = %q }", where, tostring(value[2].desc))
        elseif value[2] ~= nil then
            fail("%s: a binding takes one action and named options, "
                 .. "got a second positional value", where)
        end
        check_bound(value[1], where)
        return
    end
    fail("%s: expected an action, a function or a submap name, got %s",
         where, type(value))
end

mshell.keys.bind = function(mods, key, action, opts)
    if type(mods) ~= "table" then fail("keys.bind: mods must be a table") end
    if type(key) ~= "string" then fail("keys.bind: key must be a string") end
    check_bound(action, string.format("keys.bind %s", tostring(key)))
    if opts ~= nil and type(opts) ~= "table" then
        fail("keys.bind %s: opts must be a table", tostring(key))
    end
end

mshell.keys.submap = function(name, bindings, opts)
    if type(name) ~= "string" then return fail("keys.submap: name must be a string") end
    if type(bindings) ~= "table" then return fail("keys.submap '%s': bindings must be a table", name) end
    for key, value in pairs(bindings) do
        if type(key) ~= "string" then
            fail("submap '%s': keys must be key-name strings", name)
        else
            check_bound(value, string.format("submap '%s', key '%s'", name, key))
        end
    end
    submaps[name] = true
end

mshell.keys.leader = function(name)
    if not submaps[name] and not fragments_ok then
        fail("keys.leader: no submap named '%s'", name)
    end
end

----------------------------------------------------------------------
-- Run the configs
----------------------------------------------------------------------
local failed = false
local function chunks_of(path)
    local text = read(path)
    if not path:match("%.md$") then return { { name = path, text = text } } end

    local out, n = {}, 0
    for block in text:gmatch("```lua\n(.-)```") do
        n = n + 1
        out[#out + 1] = {
            name = string.format("%s block %d", path, n),
            text = "local mod, shft, ctrl, alt = \"LWin\", \"Shift\", \"Ctrl\", \"Alt\"\n"
                .. "local TERMINAL, discord, valorant, flow = \"a\", \"b\", \"c\", \"d\"\n"
                .. block,
            fragment = true,
        }
    end
    return out
end

local queued = {}
for i = 2, #arg do
    for _, c in ipairs(chunks_of(arg[i])) do queued[#queued + 1] = c end
end

for _, chunk in ipairs(queued) do
    local path = chunk.name
    fragments_ok = chunk.fragment
    errors = {}

    do
        local env = setmetatable({ mshell = mshell }, { __index = _G })
        local f, err = load(chunk.text, "@" .. path, "t", env)
        if not f then
            fail("%s", tostring(err))
        else
            local ok, runerr = pcall(f)
            if not ok then fail("%s", tostring(runerr)) end
        end
    end

    if #errors > 0 then
        print(string.format("  FAIL  %s", path))
        for _, e in ipairs(errors) do print("        " .. e) end
        failed = true
    else
        print(string.format("  ok    %s", path))
    end
end

os.exit(failed and 1 or 0)
