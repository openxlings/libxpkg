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

-- Which package's payload provides each tool.
--
-- A tool listed here has a payload answer, and that answer wins. A tool NOT
-- listed here has no payload by design -- `otool` and `install_name_tool` are
-- Xcode's, there is no xpkg that could provide them -- so for those the host
-- is the correct source rather than a fallback, and using it is not reported.
local _tool_provider = {
    patchelf = "patchelf",
    readelf  = "binutils",
}

-- Find a tool.
--
-- Resolution order, and why it is this order:
--
--   1. the PAYLOAD  data/xpkgs/<ns>-x-<pkg>/<ver>/bin/<tool>
--   2. the VIEW     subos/<name>/bin, <home>/bin        (reported)
--   3. the HOST     /usr/bin, /usr/local/bin, PATH      (reported)
--
-- The payload comes first because of R6 (xlings/.agents/docs/
-- 2026-08-06-subos-architecture-proposal.md §1.5): when xlings itself needs a
-- tool it must resolve the payload, never the view. The view -- shims under
-- `subos/<name>/bin` -- is a *selection made by the user*: it is mutable, it
-- follows `xlings use`, and a shim reached through PATH re-enters xlings and
-- anchors to whichever home owns the shim.
--
-- This is not a stylistic preference. `patchelf` is the tool that stamps
-- INTERP and RPATH onto every payload we ship, and patchelf versions differ in
-- how they grow the dynamic segment and in `--force-rpath` semantics. Letting
-- a mutable view -- with a silent fallback to whatever `/usr/bin/patchelf` the
-- build machine happens to have -- decide which one runs means the shape of
-- our artifacts is decided by the environment rather than by us.
--
-- The old order was 1) subos bin 2) home bin 3) /usr/bin 4) PATH, with the
-- payload not a candidate at all. In the default configuration every one of
-- those resolves to the same file, which is why it survived: the answers agree
-- by coincidence until a second home, a second version, or a host install of
-- the tool exists.
--
-- Returns { program = "/abs/path/to/tool" } or nil.
local function _find_tool(toolname)
    if _tool_cache[toolname] ~= nil then
        if _tool_cache[toolname] == false then return nil end
        return _tool_cache[toolname]
    end

    local function _accept(p, how)
        local tool = { program = p }
        if how then
            -- Not a debug line. Landing here means the artifact about to be
            -- produced was stamped by a tool we did not choose, and the only
            -- moment that is observable is now.
            _warn(string.format(
                "%s resolved to %s (%s), not to a payload. The package that "
                .. "provides it (%s) is not in this home's store; declare it "
                .. "as a build dep to make this deterministic.",
                toolname, p, how, tostring(_tool_provider[toolname])))
        else
            _info("using " .. toolname .. ": " .. p .. " (payload)")
        end
        _tool_cache[toolname] = tool
        return tool
    end

    -- 1. The payload. One answer, immutable, not reachable through any view.
    --
    -- type(), not truthiness: on a client whose libxpkg predates
    -- tool_payload_dir the field is nil here, but the same probe written as
    -- `if pkginfo.tool_payload_dir then` on a module proxy is true everywhere.
    local provider = _tool_provider[toolname]
    if provider then
        local pkginfo = _LIBXPKG_MODULES and _LIBXPKG_MODULES["pkginfo"]
        if pkginfo and type(pkginfo.tool_payload_dir) == "function" then
            local ok, dir = pcall(pkginfo.tool_payload_dir, provider)
            if ok and dir and dir ~= "" then
                local exe = path.join(dir, "bin", toolname)
                if is_host("windows") then exe = exe .. ".exe" end
                if os.isfile(exe) then return _accept(exe, nil) end
            end
        end
    end

    -- 2. The view. Kept because a home whose store predates this change still
    --    has to work, and because a user may deliberately have put a tool
    --    there -- but it is now reported rather than preferred.
    local sysroot = _RUNTIME and _RUNTIME.subos_sysrootdir
    if sysroot and sysroot ~= "" then
        local p = path.join(sysroot, "bin", toolname)
        if os.isfile(p) then return _accept(p, "subos view") end
    end

    local bin_dir = _RUNTIME and _RUNTIME.bin_dir
    if bin_dir then
        local p = path.join(bin_dir, toolname)
        if os.isfile(p) then return _accept(p, "home bin") end
    end

    -- 3. The host.
    for _, p in ipairs({ "/usr/bin/" .. toolname, "/usr/local/bin/" .. toolname }) do
        if os.isfile(p) then
            return _accept(p, provider and "host" or nil)
        end
    end

    local which_cmd = is_host("windows") and "where" or "which"
    local resolved = _trim(_iorun(which_cmd .. " " .. _shell_quote(toolname)))
    if resolved and resolved ~= "" and os.isfile(resolved) then
        return _accept(resolved, provider and "host PATH" or nil)
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

-- e_machine values the ABI filter names by architecture (see _target_abi).
local _EM_X86_64  = 62      -- 0x3e
local _EM_AARCH64 = 183     -- 0xb7
local _EM_386     = 3
local _EM_ARM     = 40

-- What an ELF file asks of a loader, read from its own headers.
--
-- Returns nil whenever the headers cannot be read in full: too short, not ELF,
-- an unknown class or byte order, a table that runs past the end of the file.
-- nil means UNKNOWN, and an unknown file is patched exactly as it always was --
-- every skip below needs positive evidence. That is also what keeps a caller's
-- stand-in file (an ELF magic and nothing else) on the path it was written for.
--
-- Read here rather than asked of patchelf: one open per file instead of a
-- process per question, and the same answer on a host whose patchelf is not
-- installed yet.
local _PT_DYNAMIC = 2
local _PT_INTERP  = 3
local _DT_NEEDED  = 1
local _MAX_DYNAMIC_BYTES = 1024 * 1024

local function _elf_facts(filepath)
    local f = io.open(filepath, "rb")
    if not f then return nil end
    local hdr = f:read(64)
    if not hdr or #hdr < 52 or hdr:sub(1, 4) ~= "\x7fELF" then f:close(); return nil end
    local class, order = hdr:byte(5), hdr:byte(6)
    if (class ~= 1 and class ~= 2) or (order ~= 1 and order ~= 2) then f:close(); return nil end
    local is64 = class == 2
    if is64 and #hdr < 64 then f:close(); return nil end
    local e = order == 1 and "<" or ">"

    local machine = string.unpack(e .. "I2", hdr, 19)
    local phoff, phentsize, phnum
    if is64 then
        phoff = string.unpack(e .. "I8", hdr, 33)
        phentsize, phnum = string.unpack(e .. "I2I2", hdr, 55)
    else
        phoff = string.unpack(e .. "I4", hdr, 29)
        phentsize, phnum = string.unpack(e .. "I2I2", hdr, 43)
    end
    -- 0xffff is PN_XNUM: the real count lives in a section header. Rare enough
    -- to leave unknown rather than follow.
    if phentsize ~= (is64 and 56 or 32) or phnum == 0xffff then f:close(); return nil end

    local facts = { class = class, machine = machine, interp = false, needed = 0 }
    if phnum == 0 then f:close(); return facts end

    f:seek("set", phoff)
    local phdrs = f:read(phentsize * phnum)
    if not phdrs or #phdrs < phentsize * phnum then f:close(); return nil end

    local dyn_off, dyn_size
    for i = 0, phnum - 1 do
        local at = i * phentsize + 1
        local p_type = string.unpack(e .. "I4", phdrs, at)
        if p_type == _PT_INTERP then
            facts.interp = true
        elseif p_type == _PT_DYNAMIC then
            if is64 then
                dyn_off = string.unpack(e .. "I8", phdrs, at + 8)
                dyn_size = string.unpack(e .. "I8", phdrs, at + 32)
            else
                dyn_off = string.unpack(e .. "I4", phdrs, at + 4)
                dyn_size = string.unpack(e .. "I4", phdrs, at + 16)
            end
        end
    end

    if dyn_off and dyn_size > 0 then
        if dyn_size > _MAX_DYNAMIC_BYTES then f:close(); return nil end
        f:seek("set", dyn_off)
        local dyn = f:read(dyn_size)
        if not dyn or #dyn < dyn_size then f:close(); return nil end
        local entry = is64 and 16 or 8
        local fmt = e .. (is64 and "i8" or "i4")
        for at = 1, #dyn - entry + 1, entry do
            local tag = string.unpack(fmt, dyn, at)
            if tag == 0 then break end
            if tag == _DT_NEEDED then facts.needed = facts.needed + 1 end
        end
    end
    f:close()
    return facts
end

-- The ABI a patched file must belong to: the loader's own, when there is one
-- to read; otherwise this host's, but only for an architecture recognised by
-- name. An unrecognised host yields nil -- no filtering -- because guessing
-- x86_64 there would call every native file foreign and patch nothing.
local function _target_abi(loader)
    if loader and loader ~= "" then
        local facts = _elf_facts(loader)
        if facts then return { class = facts.class, machine = facts.machine } end
    end
    local arch = (os.arch and os.arch()) or ""
    if arch:find("aarch64") or arch:find("arm64") then return { class = 2, machine = _EM_AARCH64 } end
    if arch:find("x86_64") or arch == "x64" then return { class = 2, machine = _EM_X86_64 } end
    if arch:find("i386") or arch == "x86" then return { class = 1, machine = _EM_386 } end
    if arch:find("arm") then return { class = 1, machine = _EM_ARM } end
    return nil
end

-- nil, or why this file must be left exactly as it is.
--
-- Two shapes, both measured (libxpkg#43):
--   * built for another machine -- an npm package's prebuilds carry aarch64 and
--     armv7 binaries next to the x86_64 ones; an x86_64 loader written into
--     them is wrong by construction.
--   * nothing to resolve -- no PT_INTERP and no DT_NEEDED. A static or
--     static-pie program (a Rust musl build, ripgrep) is one; so is a loader
--     itself. `patchelf --set-rpath` on such a file rewrites a layout nothing
--     reads, and the result segfaults before main (exit 139).
local function _skip_reason(filepath, abi)
    local facts = _elf_facts(filepath)
    if not facts then return nil end
    if abi and (facts.machine ~= abi.machine or facts.class ~= abi.class) then
        return string.format("built for another machine (e_machine %d, class %d)",
            facts.machine, facts.class)
    end
    if not facts.interp and facts.needed == 0 then
        return "nothing to resolve (no PT_INTERP, no DT_NEEDED)"
    end
    return nil
end

-- `set{ skip = ... }` / `set{ scan = ... }` entries: paths relative to the
-- install dir, '/'-separated, no wildcards. Normalised once, so "./bin/",
-- "bin/" and "bin" are one entry.
local function _rel_list(v)
    if v == nil then return nil end
    if type(v) == "string" then v = { v } end
    if type(v) ~= "table" then return nil end
    local out = {}
    for _, p in ipairs(v) do
        if type(p) == "string" then
            p = p:gsub("\\", "/"):gsub("^%./", ""):gsub("/+$", "")
            if p ~= "" and p ~= "." then table.insert(out, p) end
        end
    end
    return out
end

local function _relative_to(root, filepath)
    root = tostring(root or ""):gsub("\\", "/"):gsub("/+$", "")
    local p = tostring(filepath):gsub("\\", "/")
    if root ~= "" and p:sub(1, #root + 1) == root .. "/" then
        return p:sub(#root + 2)
    end
    return p
end

-- A skip entry names a file or a directory; a directory covers everything
-- beneath it. Prefix on a path boundary, so "bin/rg" does not cover "bin/rgx".
local function _is_listed(rel, list)
    if not list then return false end
    for _, s in ipairs(list) do
        if rel == s or rel:sub(1, #s + 1) == s .. "/" then return true end
    end
    return false
end

-- The one gate every patching loop asks before touching a file.
local function _should_skip(filepath, install_dir, abi, skip, result)
    if skip and _is_listed(_relative_to(install_dir, filepath), skip) then
        result.skipped = result.skipped + 1
        _info("skip " .. filepath .. ": listed in skip")
        return true
    end
    local why = _skip_reason(filepath, abi)
    if why then
        result.skipped = result.skipped + 1
        _info("skip " .. filepath .. ": " .. why)
        return true
    end
    return false
end

-- Read PT_INTERP existence. Used by the fallback scan / declared-bins
-- paths to discriminate executables from shared libraries WITHOUT relying
-- on filename heuristics (`.so`):
--   has INTERP → executable / PIE binary → set INTERP + RPATH
--   no INTERP  → shared library / static binary → RPATH only (set INTERP
--                would fail with patchelf exit code 1 and emit log noise)
-- Probe is `patchelf --print-interpreter`: empty output means no INTERP
-- segment, non-empty means present. If patchelf isn't found yet
-- (early-bootstrap), assume INTERP present so we don't silently skip
-- legitimate executables; the actual --set-interpreter call would fail
-- harmlessly later anyway.
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

-- Which dynamic tag a search path gets stamped in, and why it is one decision
-- in one place.
--
-- `patchelf --set-rpath` writes DT_RUNPATH by default; `--force-rpath` writes
-- DT_RPATH. The difference is not stylistic:
--
--   DT_RPATH is consulted for EVERY dlopen anywhere in the process, however
--   deep. DT_RUNPATH is consulted only for the object that carries it.
--
-- The graphics stack's load chain is three to four dlopens deep -- glvnd
-- dlopens a vendor, the vendor dlopens its external platform modules, those
-- dlopen their own dependencies -- and only the transitive tag reaches the
-- bottom. Measured on an NVIDIA host: identical path content, DT_RPATH gets
-- GLX, EGL, GLESv2 and headless surfaceless EGL all on the GPU with no
-- environment variable and no change to any recipe; DT_RUNPATH gets llvmpipe.
--
-- EXECUTABLES ONLY. Forcing DT_RPATH on a LIBRARY is measured harmful
-- (xim-pkgindex#593): transitivity is downward too, so a library's RPATH
-- enters every lookup made beneath it, and the NVIDIA EGL interposer stamped
-- that way fails `eglInitialize` outright. PT_INTERP is the predicate -- the
-- same one that already decides whether to set the interpreter.
--
-- WHY THIS FUNCTION EXISTS AT ALL, rather than the flag being inlined twice:
-- before this, the tag was decided by whoever happened to patch last. One
-- recipe in the whole index -- godot -- called `patchelf --force-rpath` by
-- hand to flip what this module stamped, documented exactly why, and was the
-- only program in the stack observed to render on the GPU. 1 of 73 installed
-- executables carried the right tag; 55 of the other 68 already carried the
-- right PATH. The knowledge existed and had no way to reach the other 68.
-- A per-package decision is not a decision, it is 73 chances to be wrong.
local function _rpath_flags(is_executable)
    return is_executable and " --force-rpath" or ""
end

-- Patch directories as executables (interpreter + rpath). Files without
-- PT_INTERP (shared libs that happened to land in a bin dir, static
-- binaries) get rpath-only treatment instead of failing the whole entry.
--
-- Op order: --set-rpath FIRST, then --set-interpreter. patchelf 0.18.0
-- has an order-sensitive corruption bug on compact ELFs (≤ ~280 KB,
-- e.g. ninja 1.12.1 at 273 KB): when --set-interpreter runs first, it
-- extends PT_LOAD and shifts the dynamic section; the subsequent
-- --set-rpath operates on stale offsets and writes through DT_NEEDED,
-- corrupting the binary irreversibly (loader segfaults at execve+1).
-- Note this is patchelf's INTERNAL processing order — passing both
-- flags in a single command also fails because patchelf processes
-- interp before rpath internally regardless of CLI order. The
-- workaround is two separate invocations in reverse order.
-- See docs/plans/2026-05-03-patchelf-order-bug-analysis.md.
local function _patch_elf_executables(patch_tool, dirs, install_dir, loader, rpath, shrink, result, gate)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            if gate(filepath) then goto next_exe end
            local ok = true
            -- Computed BEFORE the rpath, because it now decides the tag as
            -- well as the interpreter. It was already being computed for the
            -- interpreter a few lines down, so this costs nothing when a
            -- loader is set and one `--print-interpreter` per file when not.
            local is_exe = _has_pt_interp(filepath, patch_tool)
            if rpath and rpath ~= "" then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. _rpath_flags(is_exe)
                    .. " " .. _shell_quote(filepath))
            end
            if ok and loader and is_exe then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath))
            end
            if ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
            ::next_exe::
        end
    end
end

-- Patch directories as libraries (rpath only, no interpreter)
local function _patch_elf_libraries(patch_tool, dirs, install_dir, rpath, shrink, result, gate)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            if gate(filepath) then goto next_lib end
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
            ::next_lib::
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

    -- Asked once per file, in every mode below. The ABI comes from the loader
    -- that is about to be written, so "foreign" means "not for this loader".
    local abi = _target_abi(loader)
    local skip = _rel_list(opts.skip)
    local gate = function(filepath)
        return _should_skip(filepath, install_dir, abi, skip, result)
    end

    if bins or libs then
        -- Declarative mode: package already classified bin/lib dirs
        _info(string.format("declared: bins=%s libs=%s loader=%s",
            bins and table.concat(bins, ",") or "nil",
            libs and table.concat(libs, ",") or "nil",
            tostring(loader)))
        _patch_elf_executables(patch_tool, bins or {}, install_dir, loader, rpath, opts.shrink, result, gate)
        _patch_elf_libraries(patch_tool, libs or {}, install_dir, rpath, opts.shrink, result, gate)
    else
        -- Fallback mode: classify each file via PT_INTERP presence so we
        -- don't attempt --set-interpreter on shared libraries (which
        -- legitimately have no INTERP segment, causing patchelf to exit 1
        -- and log noise). Files with INTERP get loader + rpath; files
        -- without get rpath only.
        --
        -- Op order: --set-rpath FIRST, then --set-interpreter. See
        -- _patch_elf_executables comment for why (patchelf 0.18.0 small-
        -- ELF corruption bug) and docs/plans/2026-05-03-patchelf-order-
        -- bug-analysis.md for full analysis.
        _info("fallback scan mode, loader=" .. tostring(loader))
        -- `scan` narrows the walk to the listed paths under the install dir;
        -- without it the whole tree is walked, as before.
        local targets
        local scan = _rel_list(opts.scan)
        if scan then
            targets = {}
            local seen = {}
            for _, rel in ipairs(scan) do
                for _, filepath in ipairs(_collect_targets(path.join(install_dir, rel), opts)) do
                    if not seen[filepath] then
                        seen[filepath] = true
                        table.insert(targets, filepath)
                    end
                end
            end
        else
            targets = _collect_targets(target, opts)
        end
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            if gate(filepath) then goto next_file end
            local any_ok = false
            local has_interp = _has_pt_interp(filepath, patch_tool)

            if rpath and rpath ~= "" then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. _rpath_flags(has_interp)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            end
            if loader and has_interp then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            elseif loader and not has_interp then
                -- Shared library / static binary: skip interp set silently;
                -- still consider it for rpath. Don't penalize the patched
                -- count if rpath alone succeeded above.
                any_ok = true
            end

            if any_ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, opts.shrink, result)
            else
                result.failed = result.failed + 1
            end
            ::next_file::
        end
    end

    if result.skipped > 0 then
        _info(string.format("left %d file(s) untouched", result.skipped))
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
    -- Two sources, both already ABSOLUTE and both decided by the resolver:
    -- what the dep declared (deps_exports), else what the resolver filled in
    -- by convention (resolved_deps.libdirs).
    --
    -- What used to be here is gone: a branch that took the dep's NAME, asked
    -- pkginfo to find it again, and tried {lib64, lib} against whatever came
    -- back. That was a second, independent resolution — and with two versions
    -- of one package installed it answered differently from the one that
    -- chose the interpreter, producing a binary whose loader and libc came
    -- from different payloads. It segfaults before main, and the error names
    -- a GLIBC_PRIVATE symbol rather than anything about versions.
    --
    -- The convention itself did not go away; it moved to the single place
    -- that is entitled to apply it. See
    -- xlings/.agents/docs/2026-08-05-dependency-resolution-single-source.md
    local resolved = (_RUNTIME and type(_RUNTIME.resolved_deps) == "table")
                     and _RUNTIME.resolved_deps or {}
    for _, dep_spec in ipairs(deps_list) do
        local declared = deps_exports[dep_spec]
        local rec      = resolved[dep_spec]
        if declared and declared.libdirs and #declared.libdirs > 0 then
            for _, d in ipairs(declared.libdirs) do _push(d) end
        elseif rec and rec.libdirs and #rec.libdirs > 0 then
            for _, d in ipairs(rec.libdirs) do _push(d) end
        elseif _LIBXPKG_MODULES and _LIBXPKG_MODULES.pkginfo then
            -- Only a client that predates resolved_deps reaches this. Kept so
            -- an older xlings keeps working, and warned so the degraded path
            -- is never silent.
            local dep_name    = dep_spec:gsub("@.*", ""):gsub("^.+:", "")
            local dep_version = dep_spec:find("@", 1, true) and dep_spec:match("@(.+)") or nil
            local dep_dir = _LIBXPKG_MODULES.pkginfo.dep_install_dir(dep_name, dep_version)
            if dep_dir then
                for _, sub in ipairs({"lib64", "lib"}) do
                    local libdir = path.join(dep_dir, sub)
                    if os.isdir(libdir) then _push(libdir); break end
                end
            end
        end
    end

    return values
end

-- Low-level dispatch: pick the right binary-format toolchain.
--   linux   → ELF / patchelf:           --set-interpreter (PT_INTERP) + --set-rpath
--   macosx  → Mach-O / install_name_tool: -add_rpath + dylib path rewrites; opts.loader ignored
--   windows → PE has no INTERP/RPATH analog (DLL search is governed by the
--             Windows loader: same dir → System32 → PATH). No-op + log.
-- Higher-level entry points (M._apply, M.set / M.skip predicate path) bail
-- out earlier on Windows; this dispatch is the last-line guard so direct
-- callers (M.patch_elf_loader_rpath, legacy auto) stay safe too.
function M.patch_elf_loader_rpath(target, opts)
    opts = opts or {}
    local result = { scanned = 0, patched = 0, skipped = 0, failed = 0, shrinked = 0, shrink_failed = 0 }

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
    local result = { scanned = 0, patched = 0, skipped = 0, failed = 0, shrinked = 0, shrink_failed = 0 }

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
-- Narrowing what is patched, without taking the patch over:
--   scan = { "bin", "lib" }        walk only these paths under the install
--                                  dir instead of the whole tree
--   skip = { "resources/rg" }      leave these alone; a directory covers
--                                  everything beneath it
-- Both are paths relative to the install dir, '/'-separated, no wildcards.
--
-- Whatever the params, two kinds of file are never touched, because a
-- patch can only break them: a file built for another machine than the
-- loader being written, and a file with neither PT_INTERP nor DT_NEEDED
-- (static and static-pie programs, a loader itself). Both are reported in
-- the result's `skipped` count.
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
-- Old API: elfpatch.auto({enable, shrink, bins, libs, interpreter, rpath}).
-- Sets `_RUNTIME.elfpatch_legacy_*` flags that `M.apply_auto` reads to
-- preserve the original "loader='subos' default + bins/libs whitelists"
-- behavior. Don't try to remap onto the new `M.set` semantics — they
-- diverge in ways that broke prior consumers (e.g. legacy `auto({enable=true})`
-- without explicit interpreter implicitly meant "use system loader",
-- but `set({})` with no interpreter is a no-op under the new design).
-- Logs once at debug level so verbose users see migration prompts.
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
        if enable_or_opts.enable ~= nil then
            _RUNTIME.elfpatch_legacy_auto = (enable_or_opts.enable == true)
        end
        if enable_or_opts.shrink ~= nil then
            _RUNTIME.elfpatch_legacy_shrink = (enable_or_opts.shrink == true)
        end
        if enable_or_opts.bins        then _RUNTIME.elfpatch_legacy_bins = enable_or_opts.bins end
        if enable_or_opts.libs        then _RUNTIME.elfpatch_legacy_libs = enable_or_opts.libs end
        if enable_or_opts.interpreter then _RUNTIME.elfpatch_legacy_interpreter = enable_or_opts.interpreter end
        if enable_or_opts.rpath       then _RUNTIME.elfpatch_legacy_rpath = enable_or_opts.rpath end
    else
        _RUNTIME.elfpatch_legacy_auto = (enable_or_opts == true)
    end
    return _RUNTIME.elfpatch_legacy_auto
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
    local empty = { scanned = 0, patched = 0, skipped = 0, failed = 0, shrinked = 0, shrink_failed = 0 }
    if not _RUNTIME then return empty end

    -- Cross-platform support matrix:
    --   linux   → ELF + patchelf:        full INTERP + RPATH (predicate path)
    --   macosx  → Mach-O + install_name_tool: RPATH only; INTERP irrelevant
    --             (dyld is the kernel's responsibility, no per-binary loader).
    --             Predicate currently keys off `loader` so it's a no-op on
    --             macosx unless a dep declares one — which is correct since
    --             macOS deps shouldn't declare `loader`. Use elfpatch.set({
    --             rpath = {...} }) explicitly if rpath-only patching needed.
    --   windows → PE: no INTERP, no RPATH analog. DLL search is governed by
    --             Windows loader (same-dir → System32 → PATH); patchelf has
    --             no equivalent. Skip the whole predicate path early.
    if is_host("windows") then
        local log = _get_log()
        if log then log.debug("elfpatch._apply: windows host has no INTERP/RPATH analog; skipping") end
        return empty
    end

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

    -- Predicate resolution. Returns one of:
    --   { loader=<path>, predicate_kind="self"        }  Rule 1: self-patch (opt-in)
    --   { loader=<path>, predicate_kind="single", abi }  Rule 4: single dep with loader
    --   { loader=nil,    predicate_kind="ambiguous"   }  Rule 5: multi-loader → fail-fast
    --   { loader=nil,    predicate_kind="macos-rpath" }  macOS fallback: rpath-only
    --   nil                                              no patch
    --
    -- ▸ Rule 1 (self-patch) is OPT-IN. A loader-provider declaring
    --   exports.runtime.loader is publishing metadata for *consumers* —
    --   it's not asking us to rewrite its own ELF. Auto-self-patch breaks
    --   ld-linux / libc.so.6 program-header invariants (segfaults at
    --   execve+1 with SEGV_MAPERR @ 0x8). The provider's install hook
    --   should pre-relocate its own payload at install time (e.g.
    --   glibc.lua's __relocate rewrites build-host paths). Opt in via
    --   elfpatch.set({ self_patch = true }) only when the package author
    --   has verified the provider is safe to self-patch.
    --
    -- ▸ macOS fallback: Mach-O has no INTERP analog (dyld is the kernel's
    --   responsibility), so deps on macOS shouldn't declare `loader`. But
    --   consumers still need RPATH closure to find dep dylibs. When no
    --   loader candidate exists but at least one dep declared `libdirs`,
    --   fire rpath-only on macOS. Linux deliberately doesn't have this
    --   fallback — patching only RPATH leaves INTERP pointing at
    --   build-host glibc, which segfaults at execve.
    local function _resolve_predicate()
        local user_opts = _RUNTIME.elfpatch_user_opts or {}
        if user_opts.self_patch == true then
            local self_loader = _RUNTIME.self_exports and _RUNTIME.self_exports.loader
            if self_loader and self_loader ~= "" then
                return { loader = self_loader, predicate_kind = "self" }
            end
        end
        local cands = _loader_candidates()
        if #cands == 1 then
            return { loader = cands[1].loader, predicate_kind = "single", abi = cands[1].abi }
        end
        if #cands >= 2 then
            return { loader = nil, predicate_kind = "ambiguous", candidates = cands }
        end
        -- 0 loader candidates. macOS-only fallback to rpath-only path.
        if is_host("macosx") then
            local rt = (_RUNTIME and _RUNTIME.runtime_deps_list) or {}
            local exports = (_RUNTIME and _RUNTIME.deps_exports) or {}
            for _, dep_spec in ipairs(rt) do
                local e = exports[dep_spec]
                if e and e.libdirs and #e.libdirs > 0 then
                    return { loader = nil, predicate_kind = "macos-rpath" }
                end
            end
        end
        return nil
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

    -- An empty loader is only safe to proceed in two cases:
    --   1. macOS: Mach-O has no INTERP, so rpath-only is the natural patch.
    --   2. user_set: caller explicitly chose set({ rpath=... }) without an
    --      interpreter — honor their explicit intent.
    -- On Linux predicate path, an empty loader means we'd leave INTERP
    -- pointing at build-host glibc → segfault at execve. Bail safely.
    if not effective_loader or effective_loader == "" then
        local platform_allows_no_loader = is_host("macosx")
        local user_explicitly_chose_rpath_only = (source == "user_set")
        if not (platform_allows_no_loader or user_explicitly_chose_rpath_only) then
            local log = _get_log()
            if log then log.debug("elfpatch._apply: no loader resolved (source=" .. tostring(source) .. ")") end
            return empty
        end
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

-- Legacy apply path: behaves exactly like the pre-rewrite `apply_auto`
-- (loader = "subos" by default, bins/libs whitelists, etc). Used when
-- the install hook called the deprecated `M.auto({...})` API.
local function _legacy_apply(opts)
    opts = opts or {}
    if not (_RUNTIME and _RUNTIME.elfpatch_legacy_auto) then
        return { scanned = 0, patched = 0, skipped = 0, failed = 0, shrinked = 0, shrink_failed = 0 }
    end

    local target = opts.target or (_RUNTIME and _RUNTIME.install_dir)
    local rpath = opts.rpath
        or (_RUNTIME and _RUNTIME.elfpatch_legacy_rpath)
        or M.closure_lib_paths({
            -- Old behavior: deps_list was the union; legacy callers
            -- expect that closure. Don't switch to runtime_deps_list
            -- here or it'll silently change behavior.
            deps_list = _RUNTIME and _RUNTIME.deps_list,
        })
    local shrink = opts.shrink
    if shrink == nil then
        shrink = _RUNTIME and _RUNTIME.elfpatch_legacy_shrink == true
    end
    local loader = opts.loader
        or (_RUNTIME and _RUNTIME.elfpatch_legacy_interpreter)
        or "subos"

    return M.patch_elf_loader_rpath(target, {
        loader = loader,
        rpath  = rpath,
        shrink = shrink,
        bins   = _RUNTIME and _RUNTIME.elfpatch_legacy_bins,
        libs   = _RUNTIME and _RUNTIME.elfpatch_legacy_libs,
        include_shared_libs = opts.include_shared_libs,
        recurse = opts.recurse,
        strict  = opts.strict,
    })
end

-- xlings's apply_elfpatch_auto bridge. Routes between legacy and new
-- behaviors:
--   1. user_skip            → return (highest priority, both old/new)
--   2. user_override (set)  → new predicate-aware override path
--   3. legacy_auto (auto)   → legacy "loader=subos default" path
--   4. neither              → new predicate-driven default
function M.apply_auto(opts)
    if _RUNTIME and _RUNTIME.elfpatch_user_skip then
        return { scanned = 0, patched = 0, skipped = 0, failed = 0, shrinked = 0, shrink_failed = 0 }
    end
    if _RUNTIME and _RUNTIME.elfpatch_user_override then
        return M._apply()
    end
    if _RUNTIME and _RUNTIME.elfpatch_legacy_auto then
        return _legacy_apply(opts)
    end
    -- Predicate-driven default: only kicks in if a runtime-dep declared
    -- exports.runtime.loader. Otherwise no-op.
    return M._apply()
end

-- Legacy queries used by some packages; map to the legacy state for
-- packages still on M.auto, otherwise to the new override state.
function M.is_auto()
    if _RUNTIME and _RUNTIME.elfpatch_legacy_auto ~= nil then
        return _RUNTIME.elfpatch_legacy_auto == true
    end
    return not (_RUNTIME and _RUNTIME.elfpatch_user_skip)
end
function M.is_shrink()
    if _RUNTIME and _RUNTIME.elfpatch_legacy_shrink ~= nil then
        return _RUNTIME.elfpatch_legacy_shrink == true
    end
    if _RUNTIME and _RUNTIME.elfpatch_user_opts then
        return _RUNTIME.elfpatch_user_opts.shrink == true
    end
    return false
end

-- ─────────────────────────────────────────────────────────────────────
-- Build-path relocation
-- ─────────────────────────────────────────────────────────────────────
--
-- A downloaded prebuilt carries the absolute paths of the machine that built
-- it, baked into text files: linker scripts, .pc files, shell wrappers.
-- Those paths do not exist here, and they leak the build machine's layout
-- into every artifact we ship.
--
-- Recipes have been doing this by hand, and glibc's hand-rolled version got
-- all three parts wrong at once -- which is why this is a shared capability
-- rather than a fourth copy:
--
--   1. It named six files. Enumerate the payload instead. (R7: a list of
--      what someone thought of is not a measurement. glibc's list had four
--      of the five affected files on it and still missed one, and processed
--      the four wrongly.)
--
--   2. Its pattern was `([^%s)]+)/<marker>/lib`. `[^%s)]+` runs LEFTWARD
--      through anything that is not whitespace or `)` -- including variable
--      names and quotes. On the real payload it ate `RTLDLIST="` along with
--      the path, and the `ldd` we ship does not survive `bash -n`. Match an
--      anchored path TOKEN instead: walk back from the marker to a character
--      that cannot occur in a path, and require what is left to be absolute.
--
--   3. It anchored the tail at `/lib`, so the same file's
--      `.../share/locale` was left untouched -- the build path stayed in the
--      artifact, which was the one thing the code existed to prevent. Anchor
--      at the marker and keep whatever follows.
--
-- And it reported success on writing anything at all. Here the rewrite is
-- ASSERTED, not hoped for (R4): afterwards no marker may remain anywhere in
-- the payload, and every rewritten shell script must parse. Either failure
-- raises.
--
--   elfpatch.relocate_build_paths{
--       marker = "fromsource-x-glibc/" .. pkginfo.version(),
--       dir    = pkginfo.install_dir(),   -- default: install_dir
--       to     = pkginfo.install_dir(),   -- default: dir
--   }
--
-- `marker` is the part of the build path that identifies this payload -- it
-- is what makes an absolute path OURS rather than a legitimate reference to
-- /usr or /etc, which must not be touched.

-- Characters that cannot appear inside a path token in the files we rewrite.
-- `:` is included because these strings appear in PATH-like lists; `=` and
-- the quotes because of shell assignments, which is where the old pattern
-- did its damage.
local _PATH_DELIMS = {
    [" "]="", ["\t"]="", ["\n"]="", ["\r"]="", ["\0"]="",
    ["\""]="", ["'"]="", ["`"]="",
    ["("]="", [")"]="", ["{"]="", ["}"]="", ["["]="", ["]"]="",
    ["="]="", [","]="", [";"]="", [":"]="",
    ["<"]="", [">"]="", ["|"]="", ["&"]="", ["*"]="",
}

-- Where the absolute path containing [s,e] starts, or nil if the token that
-- contains the marker is not an absolute path.
--
-- Deliberately NOT a Lua pattern. A pattern that scans leftward is greedy by
-- construction and there is no way to say "stop at the start of the token"
-- without enumerating the stop set anyway -- so enumerate it, and walk.
-- `floor` bounds the walk at the first byte not yet emitted. Without it a
-- marker occurring TWICE inside one path token would walk back past the
-- previous match, and `content:sub(pos, tok - 1)` would then be empty --
-- silently deleting everything between the two occurrences.
local function _abs_token_start(content, s, floor)
    floor = floor or 1
    local i = s - 1
    while i >= floor do
        local c = content:sub(i, i)
        if _PATH_DELIMS[c] then break end
        i = i - 1
    end
    local start = i + 1
    if start < floor then return nil end
    if content:sub(start, start) ~= "/" then return nil end
    return start
end

local function _is_binary(content)
    -- A NUL in the first 8 KiB. Text files we rewrite (scripts, .pc, linker
    -- scripts) have none; ELF has one in byte 5. Cheaper and more portable
    -- than magic-number tables, and wrong only in the safe direction: a
    -- misjudged binary is skipped, not corrupted.
    return content:sub(1, 8192):find("\0", 1, true) ~= nil
end

-- The interpreter to syntax-check a rewritten script with, or nil.
--
-- The script's OWN shebang, not a fixed `sh -n`. glibc's `ldd` is
-- `#! /bin/bash` and uses bash's `$"..."`; checking it with dash would either
-- reject valid input or accept broken input depending on the host's /bin/sh,
-- and a check whose verdict depends on the machine is not a check.
local function _script_checker(filepath, content)
    local shebang = content:sub(1, 256):match("^#!([^\n]*)")
    if shebang then
        local interp = shebang:match("^%s*(%S+)")
        -- `#!/usr/bin/env bash` names the shell in the argument.
        if interp and interp:match("env$") then
            interp = shebang:match("^%s*%S+%s+(%S+)")
        end
        if interp then
            local base = interp:match("([^/]+)$") or interp
            if base == "sh" or base == "bash" or base == "dash"
               or base == "ksh" or base == "zsh" or base == "ash" then
                return base
            end
            return nil    -- python, perl, ... not ours to check
        end
    end
    if filepath:sub(-3) == ".sh" then return "sh" end
    return nil
end

function M.relocate_build_paths(opt)
    opt = opt or {}
    local marker = opt.marker
    if not marker or marker == "" then
        error("elfpatch.relocate_build_paths: `marker` is required -- it is "
              .. "what distinguishes a build path of ours from a legitimate "
              .. "reference to /usr or /etc")
    end

    local pkginfo = _LIBXPKG_MODULES and _LIBXPKG_MODULES["pkginfo"]
    local dir = opt.dir
    if (not dir or dir == "") and pkginfo then dir = pkginfo.install_dir() end
    if not dir or dir == "" or not os.isdir(dir) then
        error("elfpatch.relocate_build_paths: no payload directory to scan ("
              .. tostring(dir) .. ")")
    end
    local to = opt.to
    if not to or to == "" then to = dir end
    to = to:gsub("/+$", "")

    local fs = _LIBXPKG_MODULES and _LIBXPKG_MODULES["fs"]
    if not fs or type(fs.files) ~= "function" then
        error("elfpatch.relocate_build_paths: this client's libxpkg has no "
              .. "recursive file walk; cannot enumerate the payload")
    end

    local files = fs.files(dir, true) or {}
    local scanned, rewritten, occurrences = 0, 0, 0
    local touched_scripts = {}

    local is_symlink = type(fs.is_symlink) == "function"
        and fs.is_symlink or function() return false end

    for _, filepath in ipairs(files) do
        -- Never through a symlink. fs.files reports a symlink to a regular
        -- file as a regular file, and rewriting through one would write
        -- outside the payload -- possibly onto a file another package owns.
        local f = (not is_symlink(filepath)) and io.open(filepath, "rb") or nil
        if f then
            local content = f:read("*a") or ""
            f:close()
            scanned = scanned + 1
            if not _is_binary(content) and content:find(marker, 1, true) then
                local out, pos, hits = {}, 1, 0
                while true do
                    local s, e = content:find(marker, pos, true)
                    if not s then break end
                    local tok = _abs_token_start(content, s, pos)
                    if tok then
                        out[#out + 1] = content:sub(pos, tok - 1)
                        out[#out + 1] = to
                        hits = hits + 1
                        pos = e + 1
                    else
                        -- A relative or already-rewritten occurrence. Copied
                        -- through untouched: rewriting it would invent an
                        -- absolute path where the file deliberately has none.
                        out[#out + 1] = content:sub(pos, e)
                        pos = e + 1
                    end
                end
                out[#out + 1] = content:sub(pos)
                local new_content = table.concat(out)
                if hits > 0 and new_content ~= content then
                    local w = io.open(filepath, "wb")
                    if not w then
                        error("elfpatch.relocate_build_paths: cannot write "
                              .. filepath)
                    end
                    w:write(new_content)
                    w:close()
                    rewritten = rewritten + 1
                    occurrences = occurrences + hits
                    local checker = _script_checker(filepath, new_content)
                    if checker then
                        touched_scripts[#touched_scripts + 1] =
                            { path = filepath, interp = checker }
                    end
                end
            end
        end
    end

    -- ── assert, do not hope (R4) ──────────────────────────────────────
    --
    -- Both checks run over the result, not over the intent. The version this
    -- replaces treated "we wrote something" as success, so "there is still a
    -- build path in the payload" and "we corrupted the file" produced exactly
    -- the same output as a clean run: nothing.

    local leftovers = {}
    for _, filepath in ipairs(files) do
        local f = (not is_symlink(filepath)) and io.open(filepath, "rb") or nil
        if f then
            local content = f:read("*a") or ""
            f:close()
            if not _is_binary(content) then
                -- Every occurrence, not just the first: a file may hold a
                -- deliberately relative reference and an absolute leftover,
                -- and checking only the first would pass on the relative one.
                local pos = 1
                while true do
                    local s, e = content:find(marker, pos, true)
                    if not s then break end
                    if _abs_token_start(content, s) then
                        leftovers[#leftovers + 1] = filepath
                        break
                    end
                    pos = e + 1
                end
            end
        end
    end
    if #leftovers > 0 then
        error(string.format(
            "elfpatch.relocate_build_paths: %d file(s) still contain an "
            .. "absolute build path matching '%s' after relocation: %s",
            #leftovers, marker, table.concat(leftovers, ", ")))
    end

    -- Only where a shell exists to ask. On Windows there is none, and a
    -- payload of shell scripts is not a Windows payload anyway.
    if not is_host("windows") then
        local broken, unchecked = {}, {}
        local have = {}
        for _, s in ipairs(touched_scripts) do
            if have[s.interp] == nil then
                have[s.interp] = _exec_ok("command -v " .. _shell_quote(s.interp))
            end
            if not have[s.interp] then
                -- Not a failure. `_exec_ok` cannot tell "syntax error" from
                -- "command not found", and failing an install because the
                -- machine has no zsh would be a check inventing a defect.
                unchecked[#unchecked + 1] = s.path .. " (no " .. s.interp .. ")"
            elseif not _exec_ok(s.interp .. " -n " .. _shell_quote(s.path)) then
                broken[#broken + 1] = s.path
            end
        end
        if #unchecked > 0 then
            _warn(string.format(
                "rewrote %d script(s) whose interpreter is not on this machine, "
                .. "so they were not re-parsed: %s",
                #unchecked, table.concat(unchecked, ", ")))
        end
        if #broken > 0 then
            error(string.format(
                "elfpatch.relocate_build_paths: rewriting broke %d shell "
                .. "script(s) -- they no longer parse: %s",
                #broken, table.concat(broken, ", ")))
        end
    end

    _info(string.format(
        "relocated %d occurrence(s) of '%s' in %d of %d file(s) -> %s"
        .. " (%d script(s) re-parsed)",
        occurrences, marker, rewritten, scanned, to, #touched_scripts))

    return { scanned = scanned, rewritten = rewritten,
             occurrences = occurrences, scripts = #touched_scripts }
end


-- ─────────────────────────────────────────────────────────────────────
-- host_link_interposer
-- ─────────────────────────────────────────────────────────────────────
--
-- A driver vendor library belongs to the HOST: it is a symlink into
-- /usr/lib/..., it must match the host's kernel module, and we cannot put an
-- RPATH on it because it is not our file. So when it is dlopen'd into one of
-- our processes, its own DT_NEEDED entries have three possible fates:
--
--   1. the SONAME is already loaded  -> reused, automatically ours
--   2. not loaded, but the search path finds the HOST's copy -> two builds of
--      one library in one process, ABI mixed
--   3. not loaded and not findable   -> the vendor fails to load, no GPU
--
-- Our loader's built-in search path cannot exist by construction (AD-5), so
-- (3) is the default outcome; `DEVICE_COUNT=0`. The historical fix was to put
-- our libraries on LD_LIBRARY_PATH, which reaches (1)/(2) and also hands them
-- to every OTHER process in the subos -- including host binaries running on
-- the host loader. That is how `xlings subos use` once returned a /bin/bash
-- that died of SIGSEGV before printing a character.
--
-- This does the same job with a scope of exactly one object. An interposer is
-- a tiny shared object that
--
--   * takes the vendor's SONAME, so whoever asks for it gets this instead;
--   * NEEDs the real vendor by absolute path, so the vendor still loads and
--     `dlsym` on the handle still finds its entry points (dlsym searches the
--     handle's whole dependency tree);
--   * carries DT_RPATH -- not RUNPATH -- naming our payload closure, and
--     DT_RPATH is transitive along the load chain, so the vendor's own
--     DT_NEEDED resolve there.
--
-- Nothing is put on any process-global variable. Measured on a real NVIDIA
-- stack (2026-08-06): with LD_LIBRARY_PATH carrying only the host driver
-- directory, GL_RENDERER came back `NVIDIA GeForce RTX 4080/PCIe/SSE2` and the
-- probe read back the pixel it drew. The same subos with the LD_LIBRARY_PATH
-- approach removed and nothing in its place renders on llvmpipe.
--
-- PRECONDITION, and it is not optional -- also measured:
--
--   > An object produced here may only be loaded by a consumer whose INTERP
--   > points into OUR payload. Host binaries must keep using the host's own
--   > vendor.
--
-- Handing one to a host binary fails as
--   `librt.so.1: undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE`
-- because the RPATH names our glibc while the process's libc is the host's --
-- the loader/libc split, from the one direction the same-source assertion
-- cannot see. Callers arrange this by pointing only OUR vendor-config files at
-- the interposer; see the recipe.
--
--   elfpatch.host_link_interposer{
--       vendor  = "/usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.550.144.03",
--       out     = path.join(pkginfo.install_dir(), "lib", "libEGL_nvidia.so.0"),
--       soname  = "libEGL_nvidia.so.0",     -- default: basename of `out`
--       stub    = "<path to a prebuilt empty .so>",   -- default: from the
--                                                     -- `interposer-stub` dep
--       libdirs = { ... },                  -- default: closure_lib_paths()
--   }
--
-- `libdirs` defaults to the closure the resolver already computed. Do not pass
-- a hand-written list: R7 -- the dependency table this replaces was written by
-- hand and was missing libm, libdrm, libgbm, libgcc_s and libwayland-*, every
-- one of which was silently coming from the host.
function M.host_link_interposer(opt)
    opt = opt or {}
    local vendor = opt.vendor
    local out    = opt.out
    if not vendor or vendor == "" then
        error("elfpatch.host_link_interposer: `vendor` is required (the "
              .. "absolute path of the host's vendor library)")
    end
    if not out or out == "" then
        error("elfpatch.host_link_interposer: `out` is required")
    end
    if not os.isfile(vendor) then
        error("elfpatch.host_link_interposer: vendor not found: " .. vendor
              .. " -- the host does not have this driver installed, and an "
              .. "interposer pointing at a missing file would load as "
              .. "successfully as one pointing at nothing")
    end

    local soname = opt.soname
    if not soname or soname == "" then soname = path.filename(out) end

    -- The stub. Prebuilt and shipped as a package (AD-12): there is no
    -- compiler at install time, and an object cannot be created by patchelf,
    -- only edited.
    local stub = opt.stub
    if not stub or stub == "" then
        local pkginfo = _LIBXPKG_MODULES and _LIBXPKG_MODULES["pkginfo"]
        if pkginfo and type(pkginfo.tool_payload_dir) == "function" then
            local d = pkginfo.tool_payload_dir("interposer-stub")
            if d and d ~= "" then
                local cand = path.join(d, "lib", "interposer-stub.so")
                if os.isfile(cand) then stub = cand end
            end
        end
    end
    if not stub or not os.isfile(stub) then
        error("elfpatch.host_link_interposer: no stub. Declare a dependency "
              .. "on `interposer-stub`, or pass `stub = <path>`. There is no "
              .. "compiler at install time and patchelf edits objects rather "
              .. "than creating them.")
    end

    local libdirs = opt.libdirs
    if not libdirs then
        local closure = M.closure_lib_paths({})
        libdirs = (type(closure) == "table") and closure or {}
    end
    if #libdirs == 0 then
        error("elfpatch.host_link_interposer: the payload closure is empty, "
              .. "so the interposer would resolve the vendor's dependencies "
              .. "from the HOST -- which is what it exists to stop. Check "
              .. "that the package declares its runtime deps.")
    end

    -- The SUBOS view first, the version-pinned payload directories after it.
    --
    -- closure_lib_paths returns the payloads the resolver chose, each an exact
    -- version directory (`.../libX11/1.8.10/lib`), with the subos lib directory
    -- LAST. Written into an ELF that ordering is a snapshot: upgrade libX11 and
    -- every entry above still names 1.8.10, and once that payload is collected
    -- the entry is a dead directory the loader walks past.
    --
    -- The subos lib directory is the one name that does not move. It is a
    -- symlink farm the packages themselves declare into, so it tracks `xlings
    -- use` and re-points on upgrade -- the same role /run/opengl-driver plays on
    -- NixOS, /overrides in pressure-vessel and $SNAP/gpu-2404 in a snap. Every
    -- ecosystem that has to hand a foreign driver a search path converged on a
    -- stable indirection directory, and we already had one at the wrong end of
    -- the list.
    --
    -- The payload directories STAY, as the fallback. The recipe's own concern is
    -- real -- installed into a subos that is short of libX11, the farm would be
    -- quietly missing it -- and with this order the farm answers first while the
    -- payloads still answer at all. The closure assertion below is what turns
    -- "quietly missing" into a named failure either way.
    -- `subos_sysrootdir` is the subos root; its `lib` is the farm that
    -- `sysroot.declare_libs` (and xvm's `lib` node kind) fill. Read from
    -- _RUNTIME rather than re-derived from a path in `libdirs`: the last entry
    -- being the subos directory is a property of closure_lib_paths, not a
    -- contract, and inferring it would break silently if that changed.
    local subosdir = nil
    if _RUNTIME and _RUNTIME.subos_sysrootdir
       and _RUNTIME.subos_sysrootdir ~= "" then
        subosdir = path.join(_RUNTIME.subos_sysrootdir, "lib")
    end
    if subosdir and os.isdir(subosdir) then
        local reordered, seen = { subosdir }, { [subosdir] = true }
        for _, d in ipairs(libdirs) do
            if not seen[d] then seen[d] = true; table.insert(reordered, d) end
        end
        libdirs = reordered
    end

    local tool = _find_tool("patchelf")
    if not tool then
        error("elfpatch.host_link_interposer: patchelf not found")
    end

    local outdir = path.directory(out)
    if outdir and outdir ~= "" and not os.isdir(outdir) then os.mkdir(outdir) end
    os.tryrm(out)
    os.cp(stub, out)

    local rpath = table.concat(libdirs, ":")
    local steps = {
        { "--set-soname " .. _shell_quote(soname), "set-soname" },
        { "--add-needed " .. _shell_quote(vendor), "add-needed" },
        { "--set-rpath " .. _shell_quote(rpath) .. " --force-rpath", "set-rpath" },
    }
    for _, s in ipairs(steps) do
        if not _exec_ok(_shell_quote(tool.program) .. " " .. s[1] .. " "
                        .. _shell_quote(out)) then
            error("elfpatch.host_link_interposer: patchelf " .. s[2]
                  .. " failed on " .. out)
        end
    end

    -- Assert the artifact, not the intent (R4). Three properties, and each one
    -- silently absent produces an interposer that loads fine and does nothing:
    -- a wrong SONAME is simply never asked for; a missing NEEDED yields an
    -- object with no vendor behind it, so dlsym finds no entry point and the
    -- caller reports "no device"; DT_RUNPATH instead of DT_RPATH is not
    -- transitive, so the vendor's own dependencies fall through to the host.
    local dyn = _iorun(_shell_quote(tool.program) .. " --print-soname "
                       .. _shell_quote(out)) or ""
    if not dyn:find(soname, 1, true) then
        error("elfpatch.host_link_interposer: SONAME is '"
              .. _trim(dyn) .. "', expected '" .. soname
              .. "' -- nothing would ever ask for this object")
    end
    local needed = _iorun(_shell_quote(tool.program) .. " --print-needed "
                          .. _shell_quote(out)) or ""
    if not needed:find(vendor, 1, true) then
        error("elfpatch.host_link_interposer: the vendor is not NEEDED by "
              .. out .. " -- dlsym would find no entry point, and the caller "
              .. "would report no device rather than an error")
    end
    local got_rpath = _trim(_iorun(_shell_quote(tool.program) .. " --print-rpath "
                                   .. _shell_quote(out)) or "")
    if got_rpath == "" then
        error("elfpatch.host_link_interposer: no RPATH on " .. out
              .. " -- the vendor's dependencies would resolve from the host")
    end

    -- And the assertion those three were missing: does the RPATH actually
    -- SERVE the vendor?
    --
    -- The three above check the shape of the object we made. All three can hold
    -- while the thing the object exists for does not work: an RPATH naming
    -- directories that do not contain the vendor's DT_NEEDED resolves them from
    -- the host instead, which still renders -- on llvmpipe -- and prints
    -- nothing. That is this repository's recurring failure shape, and it was
    -- sitting inside the function written to prevent it.
    --
    -- Resolved the way the loader would, not by trusting a list: for each of the
    -- vendor's DT_NEEDED entries, look for that exact name in the RPATH
    -- directories in order. Bare SONAMEs only -- an absolute DT_NEEDED needs no
    -- search, and libc/libm come from the same closure as everything else.
    local vneeded = _iorun(_shell_quote(tool.program) .. " --print-needed "
                           .. _shell_quote(vendor)) or ""
    local missing, vtotal = {}, 0
    for line in vneeded:gmatch("[^\n]+") do
        local name = _trim(line)
        if name ~= "" and not name:find("^/") then
            vtotal = vtotal + 1
            local found = false
            for _, d in ipairs(libdirs) do
                if os.isfile(path.join(d, name)) then found = true; break end
            end
            if not found then table.insert(missing, name) end
        end
    end
    if #missing > 0 then
        -- A warning, not an error, and the line between them is what this
        -- ecosystem's convention says: the interposer IS correctly built, and on
        -- a normal host the loader will find these through its own ld.so cache
        -- and the result works. Refusing would break a working install. What
        -- must not happen is that it is INVISIBLE -- inside a sandbox or an
        -- empty-host container there is no cache to fall back to, and the
        -- failure then appears as `no device` from a GL call three layers away.
        _warn(string.format(
            "interposer %s: %d of the vendor's dependencies are not in the "
            .. "payload closure and will resolve from the HOST: %s. GL will "
            .. "work here and fail in a sandbox with no /etc/ld.so.cache. "
            .. "Declare the missing providers as runtime deps of this package.",
            soname, #missing, table.concat(missing, ", ")))
    end

    -- The fraction, not a bare "built". `interposer X -> Y` was true in every
    -- case above including the one where nothing the vendor needs is reachable.
    _info(string.format(
        "interposer %s -> %s (%d closure dir(s), vendor deps resolved %d/%d)",
        soname, vendor, #libdirs, vtotal - #missing, vtotal))
    return { out = out, soname = soname, vendor = vendor, libdirs = libdirs,
             unresolved = missing }
end

return M
