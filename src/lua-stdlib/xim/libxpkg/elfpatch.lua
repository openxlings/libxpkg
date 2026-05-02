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

-- Read ELF e_machine (offset 18, 2 bytes little-endian for ELFCLASS64
-- on x86_64; ELF header layout is identical across the two classes for
-- the e_machine field). Returns nil for non-ELF files.
local _EM_X86_64  = 62      -- 0x3e
local _EM_AARCH64 = 183     -- 0xb7
local _EM_386     = 3
local _EM_ARM     = 40

local function _read_e_machine(filepath)
    local f = io.open(filepath, "rb")
    if not f then return nil end
    local hdr = f:read(20)
    f:close()
    if not hdr or #hdr < 20 then return nil end
    if hdr:sub(1, 4) ~= "\x7fELF" then return nil end
    local lo = hdr:byte(19) or 0
    local hi = hdr:byte(20) or 0
    return lo + hi * 256
end

-- Best-effort host-arch detection. Default x86_64 because that's where
-- xlings's binary distributions live; aarch64 is the second most common.
-- Mismatch (e.g. an x86_64 host with an aarch64 ELF in install_dir) means
-- the binary is for a different target and must NOT be patched — patchelf
-- on it would corrupt or no-op spectacularly. _is_elf_for_host returns
-- true only when the file is ELF AND its e_machine matches the host.
local function _host_e_machine()
    local arch = (os.arch and os.arch()) or "x86_64"
    if arch:find("aarch64") or arch:find("arm64") then return _EM_AARCH64 end
    if arch:find("x86_64")  or arch == "x64"      then return _EM_X86_64 end
    if arch:find("i386")    or arch == "x86"      then return _EM_386 end
    if arch:find("arm")                            then return _EM_ARM end
    return _EM_X86_64
end

local function _is_elf_for_host(filepath)
    if not _is_elf(filepath) then return false end
    local em = _read_e_machine(filepath)
    if not em then return false end
    return em == _host_e_machine()
end

-- Read PT_INTERP existence: a static binary has no PT_INTERP segment;
-- patchelf is a no-op on it, but checking lets us skip the fork+exec.
-- Quick & approximate check: we run `patchelf --print-interpreter` and
-- look at exit status. If patchelf isn't around yet, skip the check
-- (patchelf will be a no-op anyway).
local function _has_pt_interp(filepath, patch_tool)
    if not patch_tool then return true end
    local cmd = _shell_quote(patch_tool.program)
        .. " --print-interpreter " .. _shell_quote(filepath) .. _err_redirect()
    local h = io.popen(cmd, "r")
    if not h then return true end
    local out = h:read("*a") or ""
    h:close()
    return out:gsub("%s+", "") ~= ""
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
    local function _push(p)
        if p and not seen[p] then seen[p] = true; table.insert(values, p) end
    end

    -- Self libdirs: prefer self_exports.libdirs (already absolute, declared
    -- by the package itself); fall back to {lib64, lib} convention.
    local self_libdirs = _RUNTIME and _RUNTIME.self_exports and _RUNTIME.self_exports.libdirs
    if self_libdirs and #self_libdirs > 0 then
        for _, d in ipairs(self_libdirs) do _push(d) end
    else
        local install_dir = _RUNTIME and _RUNTIME.install_dir
        if install_dir then
            for _, sub in ipairs({"lib64", "lib"}) do
                local self_libdir = path.join(install_dir, sub)
                if os.isdir(self_libdir) then _push(self_libdir); break end
            end
        end
    end

    -- Per-dep libdirs: prefer runtime_deps_list (post-#249 split, avoids
    -- build_dep RPATH pollution). Old callers passing opt.deps_list keep
    -- working. For each dep, prefer deps_exports[spec].libdirs (declared
    -- via the provides side) when present; fall back to {lib64, lib}
    -- convention via pkginfo.dep_install_dir lookup.
    local deps_list = opt.deps_list
        or (_RUNTIME and (_RUNTIME.runtime_deps_list or _RUNTIME.deps_list))
        or {}
    local deps_exports = _RUNTIME and _RUNTIME.deps_exports or {}
    for _, dep_spec in ipairs(deps_list) do
        local declared = deps_exports[dep_spec]
        if declared and declared.libdirs and #declared.libdirs > 0 then
            for _, d in ipairs(declared.libdirs) do _push(d) end
        else
            local dep_name    = dep_spec:gsub("@.*", ""):gsub("^.+:", "")
            local dep_version = dep_spec:find("@", 1, true) and dep_spec:match("@(.+)") or nil
            local dep_dir
            if _LIBXPKG_MODULES and _LIBXPKG_MODULES.pkginfo then
                dep_dir = _LIBXPKG_MODULES.pkginfo.dep_install_dir(dep_name, dep_version)
            end
            if dep_dir then
                for _, sub in ipairs({"lib64", "lib"}) do
                    local libdir = path.join(dep_dir, sub)
                    if os.isdir(libdir) then _push(libdir); break end
                end
            end
        end
    end

    local sysroot = _RUNTIME and _RUNTIME.subos_sysrootdir
    if sysroot and sysroot ~= "" then _push(path.join(sysroot, "lib")) end

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

-- ─────────────────────────────────────────────────────────────────────
-- Public API (v0.1.0+) — declarative ElfPatch
-- ─────────────────────────────────────────────────────────────────────
--
-- Default (consumer install hook does nothing): xlings post-install
-- predicate-driven trigger applies elfpatch automatically when the
-- consumer's runtime deps include a package that declared
-- `xpm.<plat>.exports.runtime.loader`.
--
-- Hook-level overrides (in order of precedence):
--
--   elfpatch.skip()              → don't auto-patch this package
--   elfpatch.set({...})          → use these params (predicate stays off)
--
-- Override is "覆盖式" — once set is called, the predicate-driven auto
-- path stops; xlings uses exactly the params provided. If you want
-- partial customisation, prefer providing all required fields explicitly
-- (loader / rpath) rather than mixing.
--
-- Lower-level escape hatches (rare, advanced):
--   elfpatch.patch_elf_loader_rpath(target, opts)   manual call
--   elfpatch.closure_lib_paths(opts)                compute rpath only
function M.set(opts)
    _RUNTIME = _RUNTIME or {}
    _RUNTIME.elfpatch_user_override = true
    _RUNTIME.elfpatch_user_opts = opts or {}
end

function M.skip()
    _RUNTIME = _RUNTIME or {}
    _RUNTIME.elfpatch_user_skip = true
end

-- ─────────────────────────────────────────────────────────────────────
-- DEPRECATED — half-year transition compat (drop after 2026-11)
-- ─────────────────────────────────────────────────────────────────────
-- Old API: elfpatch.auto({enable, shrink, bins, libs, interpreter, rpath})
-- mapped to elfpatch.set({...}). Logs once at debug level so verbose
-- mode users see who's still on the old path; silent in normal mode.
local _auto_warn_once = false
function M.auto(enable_or_opts)
    if not _auto_warn_once then
        _auto_warn_once = true
        local log = _get_log()
        if log then
            log.debug("elfpatch.auto() is deprecated; use elfpatch.set({...}) "
                .. "or elfpatch.skip(). The old API will be removed after 2026-11.")
        end
    end
    _RUNTIME = _RUNTIME or {}
    if type(enable_or_opts) == "table" then
        if enable_or_opts.enable == false then return M.skip() end
        local opts = {
            shrink      = enable_or_opts.shrink,
            interpreter = enable_or_opts.interpreter,
            extra_rpath = enable_or_opts.rpath,
        }
        -- Old `bins`/`libs` were per-directory whitelists; map to new `scan`.
        local scan_dirs = {}
        for _, d in ipairs(enable_or_opts.bins or {}) do table.insert(scan_dirs, d) end
        for _, d in ipairs(enable_or_opts.libs or {}) do table.insert(scan_dirs, d) end
        if #scan_dirs > 0 then opts.scan = scan_dirs end
        return M.set(opts)
    elseif enable_or_opts == false then
        return M.skip()
    end
    -- enable_or_opts == true or nil → enable predicate-driven auto.
    -- That's the default, so do nothing (don't touch user_override / skip).
end

-- Internal apply, called by xlings's apply_elfpatch_auto() after the
-- install hook returns. Decision tree mirrors the design doc:
--   1. user_skip  → return
--   2. user_override → use hook-given opts
--   3. self_exports.loader exists → use own loader (e.g. glibc itself)
--   4. exactly one runtime-dep with exports.loader → use it
--   5. ≥ 2 such deps → require interp_from in user_opts (fail-fast)
--   6. otherwise → no patch
function M._apply()
    local empty = { scanned = 0, patched = 0, failed = 0, shrinked = 0, shrink_failed = 0 }
    if not _RUNTIME then return empty end

    if _RUNTIME.elfpatch_user_skip then
        local log = _get_log()
        if log then log.debug("elfpatch._apply: user skip") end
        return empty
    end

    local target = (_RUNTIME and _RUNTIME.install_dir)
    if not target then return empty end

    -- Helper: scan runtime deps for loader providers.
    local function _loader_candidates()
        local cands = {}
        local rt = (_RUNTIME and _RUNTIME.runtime_deps_list) or {}
        local exports = (_RUNTIME and _RUNTIME.deps_exports) or {}
        for _, dep_spec in ipairs(rt) do
            local e = exports[dep_spec]
            if e and e.loader and e.loader ~= "" then
                table.insert(cands, { spec = dep_spec, loader = e.loader, abi = e.abi })
            end
        end
        return cands
    end

    -- Compose final {loader, rpath, shrink, scan, skip} from either user
    -- opts (override path) or predicate (default path).
    local function _resolve_predicate()
        local self_loader = _RUNTIME.self_exports and _RUNTIME.self_exports.loader
        if self_loader and self_loader ~= "" then
            return { loader = self_loader, predicate_kind = "self" }
        end
        local cands = _loader_candidates()
        if #cands == 0 then return nil end
        if #cands == 1 then
            return { loader = cands[1].loader, predicate_kind = "single", abi = cands[1].abi }
        end
        return { loader = nil, predicate_kind = "ambiguous", candidates = cands }
    end

    local effective_loader, effective_rpath, effective_shrink
    local effective_scan, effective_skip, effective_extra
    local source

    if _RUNTIME.elfpatch_user_override then
        local u = _RUNTIME.elfpatch_user_opts or {}
        if u.enable == false then return empty end
        source = "user_set"
        if u.interpreter and u.interpreter ~= "" then
            effective_loader = u.interpreter
        elseif u.interp_from and u.interp_from ~= "" then
            for _, c in ipairs(_loader_candidates()) do
                if c.abi == u.interp_from then effective_loader = c.loader; break end
            end
            if not effective_loader then
                _warn("elfpatch.set: interp_from='" .. u.interp_from
                    .. "' did not match any runtime-dep loader provider")
                return empty
            end
        end
        effective_shrink = (u.shrink ~= nil) and u.shrink or false
        effective_scan   = u.scan
        effective_skip   = u.skip
        effective_extra  = u.extra_rpath or {}
    else
        local r = _resolve_predicate()
        if not r then
            local log = _get_log()
            if log then log.debug("elfpatch._apply: no loader provider in deps; skipping") end
            return empty
        end
        if r.predicate_kind == "ambiguous" then
            local lines = {}
            for _, c in ipairs(r.candidates) do
                table.insert(lines, "  - " .. c.spec .. " (abi: " .. tostring(c.abi) .. ")")
            end
            _warn("elfpatch._apply: multiple loader providers in runtime deps:\n"
                .. table.concat(lines, "\n")
                .. "\nUse elfpatch.set({ interp_from = \"<abi>\" }) in install hook to disambiguate.")
            return empty
        end
        source = ("predicate:" .. r.predicate_kind)
        effective_loader = r.loader
        effective_shrink = false
        effective_extra  = {}
    end

    if not effective_loader or effective_loader == "" then
        local log = _get_log()
        if log then log.debug("elfpatch._apply: no loader resolved (source=" .. tostring(source) .. ")") end
        return empty
    end

    -- Build rpath = closure(self libdirs + runtime-dep libdirs + sysroot)
    -- + any extra_rpath the user added via set({...}).
    effective_rpath = M.closure_lib_paths({})
    for _, p in ipairs(effective_extra or {}) do
        table.insert(effective_rpath, p)
    end

    local log = _get_log()
    if log then
        log.debug("elfpatch._apply: source=" .. source
            .. " loader=" .. tostring(effective_loader))
    end

    return M.patch_elf_loader_rpath(target, {
        loader = effective_loader,
        rpath  = effective_rpath,
        shrink = effective_shrink,
        scan   = effective_scan,
        skip   = effective_skip,
    })
end

-- Backward-compat shim for the old name. xlings's apply_elfpatch_auto
-- still calls `apply_auto`; pointing it at `_apply` keeps things working
-- without a libxpkg/xlings lockstep upgrade.
function M.apply_auto(opts)
    return M._apply()
end

-- Legacy queries used by some packages; map to the new state.
function M.is_auto()
    return not (_RUNTIME and _RUNTIME.elfpatch_user_skip)
end
function M.is_shrink()
    if _RUNTIME and _RUNTIME.elfpatch_user_opts then
        return _RUNTIME.elfpatch_user_opts.shrink == true
    end
    return false
end

return M
