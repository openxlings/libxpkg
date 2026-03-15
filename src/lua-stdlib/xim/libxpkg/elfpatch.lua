-- xim.libxpkg.elfpatch: ELF and Mach-O patch helpers
local M = {}

local _tool_cache = {}

local function _trim(s)
    if not s then return s end
    return s:match("^%s*(.-)%s*$")
end

local function _shell_quote(s)
    s = tostring(s or "")
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

local function _get_log()
    return _LIBXPKG_MODULES and _LIBXPKG_MODULES["log"]
end

local function _warn(msg)
    local log = _get_log()
    if log then log.warn("elfpatch: %s", msg)
    else io.write("[xim:xpkg]: WARNING: " .. msg .. "\n") end
end

local function _info(msg)
    local log = _get_log()
    if log then log.debug("elfpatch: %s", msg)
    else io.write("[xim:xpkg]: elfpatch: " .. msg .. "\n") end
end

local function _null_redirect()
    if _RUNTIME and _RUNTIME.platform == "windows" then return " >NUL 2>&1" end
    return " >/dev/null 2>&1"
end

local function _err_redirect()
    if _RUNTIME and _RUNTIME.platform == "windows" then return " 2>NUL" end
    return " 2>/dev/null"
end

local function _exec_ok(cmd)
    local ok, _, code = os.execute(cmd .. _null_redirect())
    if ok == true or ok == 0 then return true end
    _info("exec failed (code=" .. tostring(code) .. "): " .. cmd)
    return false
end

local function _iorun(cmd)
    local f = io.popen(cmd .. _err_redirect())
    if not f then return nil end
    local output = f:read("*a")
    f:close()
    return output
end

-- Find a tool by searching fixed paths then system PATH.
-- Search order: subos/bin → _RUNTIME.bin_dir → system PATH (/usr/bin etc.)
-- Returns { program = "/abs/path/to/tool" } or nil.
local function _find_tool(toolname)
    if _tool_cache[toolname] ~= nil then
        if _tool_cache[toolname] == false then return nil end
        return _tool_cache[toolname]
    end

    local candidates = {}

    -- 1. subos bin (patchelf, readelf live here)
    local sysroot = _RUNTIME and _RUNTIME.subos_sysrootdir
    if sysroot and sysroot ~= "" then
        candidates[#candidates + 1] = path.join(sysroot, "bin", toolname)
    end

    -- 2. _RUNTIME.bin_dir (~/.xlings/bin)
    local bin_dir = _RUNTIME and _RUNTIME.bin_dir
    if bin_dir then
        candidates[#candidates + 1] = path.join(bin_dir, toolname)
    end

    -- 3. macOS system tools
    if is_host("macosx") then
        candidates[#candidates + 1] = "/usr/bin/" .. toolname
    end

    -- 4. common system paths
    candidates[#candidates + 1] = "/usr/bin/" .. toolname
    candidates[#candidates + 1] = "/usr/local/bin/" .. toolname

    for _, p in ipairs(candidates) do
        if os.isfile(p) then
            local tool = { program = p }
            _info("using " .. toolname .. ": " .. p)
            _tool_cache[toolname] = tool
            return tool
        end
    end

    -- 5. Last resort: search system PATH via shell
    local which_cmd = is_host("windows") and "where" or "which"
    local resolved = _trim(_iorun(which_cmd .. " " .. _shell_quote(toolname)))
    if resolved and resolved ~= "" and os.isfile(resolved) then
        local tool = { program = resolved }
        _info("using " .. toolname .. ": " .. resolved .. " (PATH)")
        _tool_cache[toolname] = tool
        return tool
    end

    _warn(toolname .. " not found")
    _tool_cache[toolname] = false
    return nil
end

local function _read_magic(filepath, size)
    local f = io.open(filepath, "rb")
    if not f then return nil end
    local magic = f:read(size)
    f:close()
    return magic
end

local function _is_elf(filepath)
    return _read_magic(filepath, 4) == "\x7fELF"
end

local function _is_macho(filepath)
    local magic = _read_magic(filepath, 4)
    if magic == "\xfe\xed\xfa\xce"
        or magic == "\xfe\xed\xfa\xcf"
        or magic == "\xce\xfa\xed\xfe"
        or magic == "\xcf\xfa\xed\xfe"
        or magic == "\xca\xfe\xba\xbe"
        or magic == "\xbe\xba\xfe\xca" then
        return true
    end

    local otool = _find_tool("otool")
    if not otool then
        return false
    end
    return _exec_ok(_shell_quote(otool.program) .. " -h " .. _shell_quote(filepath))
end

local function _collect_targets(target, opts)
    if not target then return {} end
    if os.isfile(target) then return { target } end
    if not os.isdir(target) then return {} end

    opts = opts or {}
    local recurse = opts.recurse
    if recurse == nil then recurse = true end
    local include_shared_libs = opts.include_shared_libs
    if include_shared_libs == nil then include_shared_libs = true end

    local matcher = _is_elf
    if is_host("macosx") then
        matcher = _is_macho
    end

    local find_cmd
    if recurse then
        find_cmd = "find " .. _shell_quote(target) .. " -type f"
    else
        find_cmd = "find " .. _shell_quote(target) .. " -maxdepth 1 -type f"
    end

    local binaries = {}
    local f = io.popen(find_cmd .. " 2>/dev/null")
    if f then
        for line in f:lines() do
            local filepath = _trim(line)
            if filepath and filepath ~= "" then
                if not include_shared_libs then
                    local is_shared = filepath:find("%.so", 1, true) ~= nil
                                   or filepath:find("%.dylib", 1, true) ~= nil
                    if is_shared then
                        goto continue
                    end
                end
                if matcher(filepath) then
                    table.insert(binaries, filepath)
                end
                ::continue::
            end
        end
        f:close()
    end
    return binaries
end

local function _normalize_rpath_list(rpath)
    if not rpath then return nil end
    if type(rpath) == "string" then
        local values, seen = {}, {}
        for p in rpath:gmatch("[^:]+") do
            p = _trim(p)
            if p and p ~= "" and not seen[p] then
                seen[p] = true
                table.insert(values, p)
            end
        end
        return #values > 0 and values or nil
    end
    if type(rpath) ~= "table" then return nil end

    local seen, values = {}, {}
    for _, p in ipairs(rpath) do
        if p and p ~= "" and not seen[p] then
            seen[p] = true
            table.insert(values, p)
        end
    end
    return #values > 0 and values or nil
end

local function _normalize_rpath(rpath)
    local values = _normalize_rpath_list(rpath)
    if not values then return nil end
    return table.concat(values, ":")
end

local function _detect_system_loader()
    local candidates = {
        "/lib64/ld-linux-x86-64.so.2",
        "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        "/lib/ld-musl-x86_64.so.1",
    }
    for _, p in ipairs(candidates) do
        if os.isfile(p) then return p end
    end

    local readelf = _find_tool("readelf")
    if readelf and os.isfile("/bin/sh") then
        local output = _iorun(_shell_quote(readelf.program) .. " -l /bin/sh")
        if output then
            local loader = _trim(output:match("Requesting program interpreter:%s*([^%]]+)"))
            if loader and os.isfile(loader) then
                return loader
            end
        end
    end
    return nil
end

local function _resolve_loader(loader_opt)
    if not loader_opt then return nil end
    if loader_opt == "system" then return _detect_system_loader() end
    if loader_opt == "subos" then
        local sysroot = _RUNTIME and _RUNTIME.subos_sysrootdir
        if sysroot and sysroot ~= "" then
            for _, p in ipairs({
                path.join(sysroot, "lib", "ld-linux-x86-64.so.2"),
                path.join(sysroot, "lib64", "ld-linux-x86-64.so.2"),
                path.join(sysroot, "lib", "ld-musl-x86_64.so.1"),
            }) do
                if os.isfile(p) then
                    return p
                end
            end
        end
        return nil
    end
    return loader_opt
end

local function _fix_macho_dylib_refs(tool, filepath, opts)
    local otool = _find_tool("otool")
    if not otool then
        return true
    end

    local output = _iorun(_shell_quote(otool.program) .. " -L " .. _shell_quote(filepath))
    if not output or output == "" then
        return true
    end

    for line in output:gmatch("[^\n]+") do
        local dep = _trim(line:match("^%s*(.-)%s+%("))
        if dep and dep ~= ""
           and not dep:match("^@")
           and not dep:match("^/usr/lib/")
           and not dep:match("^/System/") then
            local basename = path.filename(dep)
            local new_ref = "@rpath/" .. basename
            local cmd = _shell_quote(tool.program)
                     .. " -change "
                     .. _shell_quote(dep) .. " "
                     .. _shell_quote(new_ref) .. " "
                     .. _shell_quote(filepath)
            if not _exec_ok(cmd) then
                local msg = "failed to change " .. dep .. " for " .. filepath
                if opts.strict then
                    error(msg)
                end
                _warn(msg)
                return false
            end
        end
    end
    return true
end

-- Shared shrink helper
local function _apply_shrink(patch_tool, filepath, shrink, result)
    if shrink == true then
        if _exec_ok(_shell_quote(patch_tool.program) .. " --shrink-rpath " .. _shell_quote(filepath)) then
            result.shrinked = result.shrinked + 1
        else
            result.shrink_failed = result.shrink_failed + 1
        end
    end
end

-- Patch directories as executables (interpreter + rpath)
local function _patch_elf_executables(patch_tool, dirs, install_dir, loader, rpath, shrink, result)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local ok = true
            if loader then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath))
            end
            if ok and rpath and rpath ~= "" then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath))
            end
            if ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end
end

-- Patch directories as libraries (rpath only, no interpreter)
local function _patch_elf_libraries(patch_tool, dirs, install_dir, rpath, shrink, result)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local ok = true
            if rpath and rpath ~= "" then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath))
            end
            if ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end
end

local function _patch_elf(target, opts, result)
    local patch_tool = _find_tool("patchelf")
    if not patch_tool then
        _warn("patchelf not found, skip patching")
        return result
    end

    local loader = _resolve_loader(opts.loader)
    local rpath = _normalize_rpath(opts.rpath)
    if opts.loader and not loader then
        local msg = "cannot resolve loader: " .. tostring(opts.loader)
        if opts.strict then
            error(msg)
        end
        _warn(msg)
    end

    local install_dir = _RUNTIME and _RUNTIME.install_dir or target
    local bins = opts.bins or (_RUNTIME and _RUNTIME.elfpatch_bins)
    local libs = opts.libs or (_RUNTIME and _RUNTIME.elfpatch_libs)

    -- Custom interpreter override (absolute path, skip _resolve_loader)
    local custom_interp = opts.interpreter or (_RUNTIME and _RUNTIME.elfpatch_interpreter)
    if custom_interp then
        loader = custom_interp
    end
    -- Custom rpath override (absolute paths)
    local custom_rpath = opts.custom_rpath or (_RUNTIME and _RUNTIME.elfpatch_rpath)
    if custom_rpath then
        rpath = _normalize_rpath(custom_rpath)
    end

    if bins or libs then
        -- Declarative mode: package already classified bin/lib dirs
        _info(string.format("declared: bins=%s libs=%s loader=%s",
            bins and table.concat(bins, ",") or "nil",
            libs and table.concat(libs, ",") or "nil",
            tostring(loader)))
        _patch_elf_executables(patch_tool, bins or {}, install_dir, loader, rpath, opts.shrink, result)
        _patch_elf_libraries(patch_tool, libs or {}, install_dir, rpath, opts.shrink, result)
    else
        -- Fallback mode: full scan, interpreter and rpath independent (no cascade)
        _info("fallback scan mode, loader=" .. tostring(loader))
        local targets = _collect_targets(target, opts)
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local any_ok = false

            if loader then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            end
            if rpath and rpath ~= "" then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            end

            if any_ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, opts.shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end

    return result
end

local function _patch_macho(target, opts, result)
    local tool = _find_tool("install_name_tool")
    if not tool then
        _warn("install_name_tool not found, skip patching (try: xcode-select --install)")
        return result
    end

    local rpath_paths = _normalize_rpath_list(opts.rpath)
    if not rpath_paths or #rpath_paths == 0 then
        return result
    end

    local targets = _collect_targets(target, opts)
    for _, filepath in ipairs(targets) do
        result.scanned = result.scanned + 1
        local ok = true

        for _, rp in ipairs(rpath_paths) do
            local add_ok = _exec_ok(_shell_quote(tool.program)
                .. " -add_rpath "
                .. _shell_quote(rp) .. " "
                .. _shell_quote(filepath))
            if not add_ok then
                if opts.strict then
                    error("failed to add rpath " .. rp .. " for " .. filepath)
                end
                ok = false
            end
        end

        local fix_ok = true
        if ok then
            fix_ok = _fix_macho_dylib_refs(tool, filepath, opts)
        end
        if fix_ok == false and opts.strict ~= true then
            ok = false
        end

        if ok then
            result.patched = result.patched + 1
        else
            result.failed = result.failed + 1
        end
    end

    return result
end

function M.closure_lib_paths(opt)
    opt = opt or {}
    local values, seen = {}, {}

    local install_dir = _RUNTIME and _RUNTIME.install_dir
    if install_dir then
        for _, sub in ipairs({"lib64", "lib"}) do
            local self_libdir = path.join(install_dir, sub)
            if os.isdir(self_libdir) and not seen[self_libdir] then
                seen[self_libdir] = true
                table.insert(values, self_libdir)
                break
            end
        end
    end

    local deps_list = opt.deps_list or (_RUNTIME and _RUNTIME.deps_list) or {}
    for _, dep_spec in ipairs(deps_list) do
        local dep_name = dep_spec:gsub("@.*", ""):gsub("^.+:", "")
        local dep_version = dep_spec:find("@", 1, true) and dep_spec:match("@(.+)") or nil
        local dep_dir = nil
        if _LIBXPKG_MODULES and _LIBXPKG_MODULES.pkginfo then
            dep_dir = _LIBXPKG_MODULES.pkginfo.dep_install_dir(dep_name, dep_version)
        end
        if dep_dir then
            for _, sub in ipairs({"lib64", "lib"}) do
                local libdir = path.join(dep_dir, sub)
                if os.isdir(libdir) and not seen[libdir] then
                    seen[libdir] = true
                    table.insert(values, libdir)
                    break
                end
            end
        end
    end

    local sysroot = _RUNTIME and _RUNTIME.subos_sysrootdir
    if sysroot and sysroot ~= "" then
        local subos_lib = path.join(sysroot, "lib")
        if not seen[subos_lib] then
            seen[subos_lib] = true
            table.insert(values, subos_lib)
        end
    end

    return values
end

function M.patch_elf_loader_rpath(target, opts)
    opts = opts or {}
    local result = { scanned = 0, patched = 0, failed = 0, shrinked = 0, shrink_failed = 0 }

    if is_host("linux") then
        return _patch_elf(target, opts, result)
    elseif is_host("macosx") then
        return _patch_macho(target, opts, result)
    end

    _info("skipping on unsupported platform " .. tostring(os.host()))
    return result
end

function M.set_interpreter(target, interpreter, opts)
    opts = opts or {}
    local result = { scanned = 0, patched = 0, failed = 0 }
    if not is_host("linux") then return result end

    local patch_tool = _find_tool("patchelf")
    if not patch_tool then
        _warn("patchelf not found")
        return result
    end

    local targets = _collect_targets(target, opts)
    for _, filepath in ipairs(targets) do
        result.scanned = result.scanned + 1
        _info("set-interpreter: " .. filepath .. " -> " .. interpreter)
        if _exec_ok(_shell_quote(patch_tool.program)
            .. " --set-interpreter " .. _shell_quote(interpreter)
            .. " " .. _shell_quote(filepath)) then
            result.patched = result.patched + 1
        else
            result.failed = result.failed + 1
        end
    end
    return result
end

function M.set_rpath(target, rpath, opts)
    opts = opts or {}
    local shrink = opts.shrink
    if shrink == nil then shrink = true end
    local result = { scanned = 0, patched = 0, failed = 0, shrinked = 0, shrink_failed = 0 }

    if is_host("linux") then
        local patch_tool = _find_tool("patchelf")
        if not patch_tool then
            _warn("patchelf not found")
            return result
        end
        local rpath_str = _normalize_rpath(rpath)
        if not rpath_str or rpath_str == "" then return result end

        local targets = _collect_targets(target, opts)
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            _info("set-rpath: " .. filepath)
            if _exec_ok(_shell_quote(patch_tool.program)
                .. " --set-rpath " .. _shell_quote(rpath_str)
                .. " " .. _shell_quote(filepath)) then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    elseif is_host("macosx") then
        return _patch_macho(target, { rpath = rpath }, result)
    end

    return result
end

function M.auto(enable_or_opts)
    _RUNTIME = _RUNTIME or {}
    if type(enable_or_opts) == "table" then
        if enable_or_opts.enable ~= nil then
            _RUNTIME.elfpatch_auto = (enable_or_opts.enable == true)
        end
        if enable_or_opts.shrink ~= nil then
            _RUNTIME.elfpatch_shrink = (enable_or_opts.shrink == true)
        end
        -- Declarative bin/lib directories
        if enable_or_opts.bins then
            _RUNTIME.elfpatch_bins = enable_or_opts.bins
        end
        if enable_or_opts.libs then
            _RUNTIME.elfpatch_libs = enable_or_opts.libs
        end
        -- Optional: custom interpreter and rpath (absolute paths)
        if enable_or_opts.interpreter then
            _RUNTIME.elfpatch_interpreter = enable_or_opts.interpreter
        end
        if enable_or_opts.rpath then
            _RUNTIME.elfpatch_rpath = enable_or_opts.rpath
        end
    else
        _RUNTIME.elfpatch_auto = (enable_or_opts == true)
    end
    return _RUNTIME.elfpatch_auto
end

function M.is_auto()
    return _RUNTIME and _RUNTIME.elfpatch_auto == true
end

function M.is_shrink()
    return _RUNTIME and _RUNTIME.elfpatch_shrink == true
end

function M.apply_auto(opts)
    opts = opts or {}
    if not M.is_auto() then
        return { scanned = 0, patched = 0, failed = 0, shrinked = 0, shrink_failed = 0 }
    end

    local target = opts.target or (_RUNTIME and _RUNTIME.install_dir)
    local rpath = opts.rpath or M.closure_lib_paths({
        deps_list = _RUNTIME and _RUNTIME.deps_list
    })
    local shrink = opts.shrink
    if shrink == nil then
        shrink = M.is_shrink()
    end

    return M.patch_elf_loader_rpath(target, {
        loader = opts.loader or "subos",
        rpath = rpath,
        shrink = shrink,
        include_shared_libs = opts.include_shared_libs,
        recurse = opts.recurse,
        strict = opts.strict,
    })
end

return M
