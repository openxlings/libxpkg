-- compat.lua: the compatibility layer of the hook environment, and import().
--
-- Recipes of spec "1" and "2" were written against names that live here, in
-- Lua's own tables and the globals: os.mv, os.cp, os.mkdir, os.tryrm,
-- os.isfile, os.exec, io.readfile, string.replace, the global path, format,
-- raise, is_host, try, cprint, and an import() that returns a stub for a
-- module it does not know. Such a name reports a failure by its return value
-- (or not at all: os.tryrm is always true), so a call written as a statement
-- loses it.
--
-- The layer is frozen (design 2026-11-27 §9.6): every name keeps exactly the
-- behaviour it has, and no name is added. The interface that replaces it is the
-- modules (xim.libxpkg.fs, .system, .pkginfo, .path): an action states what
-- holds when it returns and raises otherwise. A recipe of spec "3" runs in an
-- environment without this layer. It stays because installed packages keep
-- the recipe they were installed with, and third-party and local recipes are
-- not migrated by anyone. What it adds over the old behaviour is a record:
-- os.mv, os.cp, os.mkdir and os.exec failing are noted for the host
-- (HookResult::call_failures); what they return is unchanged.
--
-- Loaded into every Lua state the executor makes, after the modules; the
-- module implementations use some of these names themselves.

-- _LIBXPKG_MODULES is populated by C++ before this file runs
_LIBXPKG_MODULES = _LIBXPKG_MODULES or {}

-- A failed action of this layer, noted for the host (HookResult::call_failures)
-- while a hook runs; the name keeps returning what it always returned.
function _LIBXPKG_CALL_FAILED(call, what, why)
    local list = _LIBXPKG_CALL_FAILURES
    if type(list) == "table" and #list < 100 then
        table.insert(list, { call = call, what = what, why = why })
    end
end

-- Save Lua's built-in package.config before xpkg scripts overwrite `package` global
local _PATH_SEP = package.config:sub(1,1)

-- import(): maps "xim.libxpkg.X" to preloaded modules, and binds the module to
-- the global of its last name as well (import("xim.libxpkg.log") makes `log`),
-- which is how recipes of spec "1" and "2" use it.
function import(mod_path)
    local name = mod_path:match("xim%.libxpkg%.(.+)")
    if name and _LIBXPKG_MODULES[name] then
        _G[name] = _LIBXPKG_MODULES[name]
        return _LIBXPKG_MODULES[name]
    end
    -- pkgindex custom modules: import("xim.pkgindex.<name>")
    -- Loads <pkgindex_dir>/libs/<name>.lua from the package index repository.
    -- Modules are cached in _LIBXPKG_MODULES after first load.
    local pkgindex_mod = mod_path:match("xim%.pkgindex%.(.+)")
    if pkgindex_mod then
        -- Check cache first
        if _LIBXPKG_MODULES[pkgindex_mod] then
            _G[pkgindex_mod] = _LIBXPKG_MODULES[pkgindex_mod]
            return _LIBXPKG_MODULES[pkgindex_mod]
        end
        -- _PKGINDEX_DIR is set early (before L_dofile) so top-level imports work;
        -- _RUNTIME.pkgindex_dir is set later by inject_context for hook calls.
        local pkgindex_dir = _PKGINDEX_DIR
            or (_RUNTIME and _RUNTIME.pkgindex_dir)
        local why
        if pkgindex_dir and pkgindex_dir ~= "" then
            local mod_file = pkgindex_dir .. "/libs/" .. pkgindex_mod .. ".lua"
            local loader, err = loadfile(mod_file)
            if loader then
                local ok, mod = pcall(loader)
                if ok and mod then
                    _LIBXPKG_MODULES[pkgindex_mod] = mod
                    _G[pkgindex_mod] = mod
                    return mod
                end
                why = string.format("the index's lib %s could not be loaded: %s", mod_file,
                    ok and "it returns no module" or tostring(mod))
            else
                why = string.format("the index's lib %s could not be read: %s", mod_file, tostring(err))
            end
        else
            why = "this recipe is read from no index with libs/"
        end
        -- A stand-in, so that reading the recipe (a search, an index build)
        -- still works; but a hook that calls it did not do what it says: each
        -- call is a failed call of the hook (the host fails the hook with it,
        -- naming the lib), never a silent success (design 2026-11-27 §20.1).
        local short = mod_path:match("[^.]+$") or mod_path
        local function make_lib_proxy(member)
            return setmetatable({}, {
                __index = function(_, k) return make_lib_proxy(member .. "." .. tostring(k)) end,
                __call = function()
                    _LIBXPKG_CALL_FAILED("import(\"" .. mod_path .. "\")", member, why)
                    return make_lib_proxy(member)
                end,
                __tostring = function() return '' end,
                __concat = function(a, b) return tostring(a) .. tostring(b) end,
            })
        end
        local stub = setmetatable({}, {
            __index = function(_, k) return make_lib_proxy(short .. "." .. tostring(k)) end,
        })
        _G[short] = stub
        return stub
    end

    -- Stub for unknown imports (platform, base.runtime, etc.)
    local log = _LIBXPKG_MODULES and _LIBXPKG_MODULES["log"]
    if log then log.debug("unknown module '%s', returning stub", mod_path) end
    local short = mod_path:match("[^.]+$") or mod_path
    local function make_proxy()
        return setmetatable({}, {
            __index = function(_, k) return make_proxy() end,
            __call = function() return make_proxy() end,
            __tostring = function() return '' end,
            __concat = function(a, b) return tostring(a) .. tostring(b) end,
        })
    end
    local stub = setmetatable({}, {
        __index = function(_, k) return make_proxy() end,
    })
    _G[short] = stub
    return stub
end

-- os.*: the file and process names of the layer. The executor replaces most
-- of these with C++ functions after this file runs (register_os_funcs).
os.isfile = function(p)
    -- io.open succeeds on directories on Linux (fopen quirk), so also check it's not a dir
    local f = io.open(p, "r")
    if not f then return false end
    f:close()
    -- Reject directories: try reading 0 bytes; directories fail with "Is a directory"
    local f2 = io.open(p, "rb")
    if not f2 then return false end
    local ok, _ = f2:read(0)
    f2:close()
    -- read(0) returns "" on regular files, nil on directories
    return ok ~= nil
end
os.isdir = function(p)
    local sep = _PATH_SEP
    if sep == "\\" then
        local ret = os.execute('if exist "' .. p .. '\\" exit 0')
        return ret == 0 or ret == true
    else
        local ret = os.execute('[ -d "' .. p .. '" ]')
        return ret == 0 or ret == true
    end
end
os.host = function()
    return _RUNTIME and _RUNTIME.platform or "linux"
end
os.trymv = function(src, dst)
    -- If dst is an existing directory, move src INTO it (unix mv semantics)
    if os.isdir(dst) then
        local fname = src:match("[^/\\]+$") or src
        dst = dst:gsub("[/\\]+$", "") .. "/" .. fname
    end
    local ok = pcall(os.rename, src, dst)
    if ok then return true end
    -- Cross-device or directory: fallback to shell mv
    local ret = os.execute('mv "' .. src .. '" "' .. dst .. '" 2>/dev/null')
    if ret == 0 or ret == true then return true end
    -- Last resort: file copy + remove (files only)
    local inf = io.open(src, "rb")
    if not inf then return false end
    local content = inf:read("*a"); inf:close()
    local outf = io.open(dst, "wb")
    if not outf then return false end
    outf:write(content); outf:close()
    local rm_ok = pcall(os.remove, src)
    return rm_ok
end
os.mv = function(src, dst) return os.trymv(src, dst) end
os.cp = function(src, dst, opts)
    -- opts is accepted and ignored: cp -a already keeps links as links
    -- Try shell cp first (handles directories, symlinks, etc.)
    local sep = _PATH_SEP
    if sep ~= "\\" then
        local ret = os.execute('cp -a "' .. src .. '" "' .. dst .. '" 2>/dev/null')
        if ret == 0 or ret == true then return true end
    end
    -- Fallback: file copy
    local inf = io.open(src, "rb")
    if not inf then return false end
    local content = inf:read("*a"); inf:close()
    local outf = io.open(dst, "wb")
    if not outf then return false end
    outf:write(content); outf:close()
    return true
end
-- os.dirs, os.iorun and os.execute (so os.exec below) are the executor's C++
-- functions, registered after this prelude runs (register_os_funcs); a command
-- line reaches the shell only through xpkg-shell.cppm.
os.sleep = function(ms) end  -- stub
os.cd = function(dir)
    if not dir then return false end
    -- Fallback: pure Lua cannot chdir; C++ override replaces this after prelude
    _CURRENT_DIR = dir
    return true
end
os.setenv = function(k, v)
    if _PATH_SEP == "\\" then
        os.execute(string.format('setx %s "%s"', k, v))
    else
        _ENV_OVERRIDES = _ENV_OVERRIDES or {}
        _ENV_OVERRIDES[k] = v
    end
end
os.addenv = function(k, v)
    if _PATH_SEP == "\\" then
        local cur = os.getenv(k) or ""
        os.execute(string.format('setx %s "%s"', k, cur ~= "" and (cur .. ";" .. v) or v))
    else
        _ENV_OVERRIDES = _ENV_OVERRIDES or {}
        local cur = _ENV_OVERRIDES[k] or os.getenv(k) or ""
        _ENV_OVERRIDES[k] = cur ~= "" and (cur .. ":" .. v) or v
    end
end
os.exec = function(cmd)
    if _ENV_OVERRIDES and _PATH_SEP ~= "\\" then
        local prefix = ""
        for k, v in pairs(_ENV_OVERRIDES) do
            prefix = prefix .. string.format('%s="%s" ', k, v)
        end
        if prefix ~= "" then cmd = prefix .. cmd end
    end
    local ok, how, code = os.execute(cmd)
    if not (ok == true or ok == 0) then
        _LIBXPKG_CALL_FAILED("os.exec", tostring(cmd), how == "exit" and ("exited with " .. tostring(code))
            or how == "signal" and ("was killed by signal " .. tostring(code)) or tostring(how))
    end
    return ok, how, code
end
os.tryrm = function(p)
    if not p then return false end
    local sep = _PATH_SEP
    local cmd
    if sep == "\\" then
        cmd = 'rmdir /s /q "' .. p .. '" 2>nul'
    else
        cmd = 'rm -rf "' .. p .. '" 2>/dev/null'
    end
    os.execute(cmd)
    return true
end
os.mkdir = function(p)
    if not p then return false end
    local sep = _PATH_SEP
    local cmd
    if sep == "\\" then
        cmd = 'mkdir "' .. p .. '" 2>nul'
    else
        cmd = 'mkdir -p "' .. p .. '" 2>/dev/null'
    end
    os.execute(cmd)
    return true
end

-- path: the module (xim.libxpkg.path), as a global
path = _LIBXPKG_MODULES["path"]

-- io extensions
io.readfile = function(p)
    local f = io.open(p, "r")
    if not f then return nil end
    local content = f:read("*a"); f:close()
    return content
end
io.writefile = function(p, content)
    local f = io.open(p, "w")
    if not f then return false end
    f:write(content); f:close()
    return true
end

-- cprint: strip ${color} markers, fallback to print
cprint = function(fmt, ...)
    if type(fmt) == "string" then
        fmt = fmt:gsub("%${%w+}", "")
        local ok, msg = pcall(string.format, fmt, ...)
        print(ok and msg or fmt)
    else
        print(fmt)
    end
end

-- string.split: split string by separator
if not string.split then
    function string.split(s, sep, plain)
        local result = {}
        local i = 1
        while true do
            local j, k = s:find(sep, i, plain)
            if not j then
                table.insert(result, s:sub(i))
                break
            end
            table.insert(result, s:sub(i, j-1))
            i = k + 1
        end
        return result
    end
end

-- string:trim(): remove leading/trailing whitespace
if not string.trim then
    function string.trim(s)
        return s:match("^%s*(.-)%s*$") or s
    end
end

-- Globals of the layer: the host's platform, string.format and error under
-- other names.
function is_host(name)
    local host = _RUNTIME and _RUNTIME.platform or os.host()
    return host == name
end
format = string.format
raise = function(msg) error(msg or "raise called", 2) end

-- string.replace: plain text replacement (not a pattern), as recipes of every
-- spec use it; a pure function, so it is part of both environments.
if not string.replace then
    function string.replace(s, old, new, opts)
        -- Plain text, not a pattern; opts is accepted and ignored
        local result = s
        local i = 1
        while true do
            local pos = result:find(old, i, true)
            if not pos then break end
            result = result:sub(1, pos - 1) .. new .. result:sub(pos + #old)
            i = pos + #new
        end
        return result
    end
end

-- try { function, catch { function } }: runs the first function; when it
-- raises, the catch function gets the error and try returns nil. A recipe of
-- spec "3" writes pcall.
function try(block)
    local fn = block[1]
    local catch_block = block.catch
    local ok, result = pcall(fn)
    if not ok then
        if catch_block and catch_block[1] then
            catch_block[1](result)
        end
        return nil
    end
    return result
end
