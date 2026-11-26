-- spec3.lua: the environment of a recipe of spec "3" (design 2026-11-27 §9.6).
--
-- A recipe of spec "3", and each index lib it imports, runs in a table of its
-- own: the computing part of standard Lua, and the modules it imports. None of
-- the compatibility layer is there -- no os.mv, no io, no is_host, no global
-- path -- so a call is an action, a query or a declaration, and an action that
-- does not happen raises. The module implementations still run in the global
-- environment (and some use the compatibility layer's functions there); that
-- is theirs, not the recipe's.
--
-- A module is seen through a view: a table of exactly the names the interface
-- manifest (manifest.lua, _LIBXPKG_MANIFEST) lists for it. A module the
-- manifest does not list is an error to import, not a stub.
--
-- This is no sandbox: a hook still runs any program through system.exec.
-- Keeping a hook to what it may touch is the host's isolation; this keeps
-- every call's outcome known.

local manifest = _LIBXPKG_MANIFEST

local views = {}
-- function -> "module.name", for every function a recipe may not call at its
-- top level: all but the pure ones. The top level also runs when the package
-- table is read (search, an index build), where nothing is being installed.
_LIBXPKG_TOPLEVEL_FORBIDDEN = {}

local function module_entry(name)
    for _, m in ipairs(manifest.modules) do
        if m.name == name then return m end
    end
    return nil
end

local function view(name)
    if views[name] then return views[name] end
    local entry = module_entry(name)
    local impl = _LIBXPKG_MODULES[name]
    if not entry or not impl then return nil end
    local v = {}
    for _, row in ipairs(entry.entries) do
        local fn = impl[row[1]]
        if fn == nil then
            error("libxpkg: the manifest lists " .. name .. "." .. row[1] .. " and the module has none")
        end
        v[row[1]] = fn
        if row[2] ~= "pure" and type(fn) == "function" then
            _LIBXPKG_TOPLEVEL_FORBIDDEN[fn] = name .. "." .. row[1]
        end
    end
    views[name] = v
    return v
end

-- A view is made when it is first imported, so it holds what the host
-- registered before the recipe ran (fs.extract); its functions join the
-- top-level rule then, before the recipe can reach them.

local BASE = {
    "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
    "rawlen", "rawset", "select", "setmetatable", "getmetatable", "tonumber",
    "tostring", "type", "xpcall",
}

local libs = {}   -- index lib name -> its module table, once per state

local function new_env()
    local env = {}
    for _, name in ipairs(BASE) do env[name] = _G[name] end
    -- The executor captures print while a hook runs by replacing the global
    -- one; this finds the current one at each call.
    env.print = function(...) return _G.print(...) end
    env.string, env.table, env.math, env.utf8 = string, table, math, utf8
    env.os = { time = os.time, date = os.date, clock = os.clock, getenv = os.getenv }
    env._VERSION = _VERSION
    env._G = env

    env.import = function(mod)
        if type(mod) ~= "string" then error("import: a module's name is a string", 2) end
        local short = mod:match("[^.]+$") or mod
        local name = mod:match("^xim%.libxpkg%.(.+)$")
        if name then
            local v = view(name)
            if not v then
                error(string.format("import: %q is not a module of the hook interface of spec \"3\" "
                    .. "(an older name, or one a newer xlings has)", mod), 2)
            end
            env[short] = v
            return v
        end
        local lib = mod:match("^xim%.pkgindex%.(.+)$")
        if lib then
            local m = libs[lib]
            if m == nil then
                local dir = _PKGINDEX_DIR or (_RUNTIME and _RUNTIME.pkgindex_dir)
                if not dir or dir == "" then
                    error(string.format("import: %q: this recipe is not in an index, which has no libs", mod), 2)
                end
                local file = dir .. "/libs/" .. lib .. ".lua"
                -- A lib runs in an environment of its own, under the same rules.
                local chunk, err = loadfile(file, "t", new_env())
                if not chunk then error(string.format("import: %q: %s", mod, tostring(err)), 2) end
                m = chunk()
                if type(m) ~= "table" then
                    error(string.format("import: %q (%s) returns no module table", mod, file), 2)
                end
                libs[lib] = m
            end
            env[short] = m
            return m
        end
        error(string.format("import: unknown module %q", mod), 2)
    end
    return env
end

-- The environment the executor runs a recipe of spec "3" in.
function _LIBXPKG_SPEC3_ENV()
    return new_env()
end
