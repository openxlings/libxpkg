-- xim.libxpkg.pkginfo: package info API reading from _RUNTIME global
local M = {}

local function _get_log()
    return _LIBXPKG_MODULES and _LIBXPKG_MODULES["log"]
end

function M.name()         return _RUNTIME and _RUNTIME.pkg_name or nil end
function M.version()      return _RUNTIME and _RUNTIME.version or nil end
function M.install_file() return _RUNTIME and _RUNTIME.install_file or nil end
function M.deps_list()    return (_RUNTIME and _RUNTIME.deps_list) or {} end

local function _ends_with(s, suffix)
    return suffix == "" or s:sub(-#suffix) == suffix
end

local function _parse_namespace(name)
    local ns, bare = name:match("^([^:]+):(.+)$")
    if ns then return ns, bare end
    return nil, name
end

local function _match_store_name(dirname, ns, bare)
    if ns then
        -- namespace specified: exact match "ns-x-bare"
        return dirname == ns .. "-x-" .. bare
    else
        -- no namespace: match "bare" or "*-x-bare"
        return dirname == bare or _ends_with(dirname, "-x-" .. bare)
    end
end

local function _scan_dir(base, ns, bare, dep_version)
    if not base or not os.isdir(base) then return nil end
    local dirs = os.dirs(path.join(base, "*")) or {}
    for _, dep_root in ipairs(dirs) do
        local dirname = path.filename(dep_root)
        if _match_store_name(dirname, ns, bare) then
            local ver = dep_version
            if not ver then
                local vers = os.dirs(path.join(dep_root, "*")) or {}
                table.sort(vers)
                if #vers > 0 then ver = path.filename(vers[#vers]) end
            end
            if ver then
                local install_dir = path.join(dep_root, ver)
                if os.isdir(install_dir) then return install_dir end
            end
        end
    end
    return nil
end

local function _resolve_dep_via_scan(dep_name, dep_version)
    local log = _get_log()
    local ns, bare = _parse_namespace(dep_name)
    if log then log.debug("scan dep=%s ns=%s bare=%s ver=%s",
        dep_name, tostring(ns), bare, tostring(dep_version)) end
    -- 1. Search xpkg_dir (lua package files directory)
    local xpkg_dir = _RUNTIME and _RUNTIME.xpkg_dir
    if log then log.debug("step1 xpkg_dir=%s", tostring(xpkg_dir)) end
    local result = _scan_dir(xpkg_dir, ns, bare, dep_version)
    if result then if log then log.debug("found via step1") end; return result end
    -- 2. Search xpkgs install root (install_dir's grandparent)
    if _RUNTIME and _RUNTIME.install_dir then
        local xpkgs_root = path.directory(path.directory(_RUNTIME.install_dir))
        if log then log.debug("step2 xpkgs_root=%s", tostring(xpkgs_root)) end
        result = _scan_dir(xpkgs_root, ns, bare, dep_version)
        if result then if log then log.debug("found via step2") end; return result end
    end
    -- 3. Search project xpkgs (handles global-pkg depending on project-local pkg)
    local proj_data = _RUNTIME and _RUNTIME.project_data_dir
    if log then log.debug("step3 project_data_dir=%s", tostring(proj_data)) end
    if proj_data and proj_data ~= "" then
        local proj_xpkgs = path.join(proj_data, "xpkgs")
        if log then log.debug("step3 proj_xpkgs=%s exists=%s",
            proj_xpkgs, tostring(os.isdir(proj_xpkgs))) end
        result = _scan_dir(proj_xpkgs, ns, bare, dep_version)
        if result then if log then log.debug("found via step3") end; return result end
    end
    if log then log.debug("scan: not found") end
    return nil
end

-- Try xvm registry: for "ns:name", try "ns-name" first, then bare "name"
local function _resolve_dep_via_xvm(dep_name, dep_version)
    local log = _get_log()
    local ok_xvm, xvm_mod = pcall(require, "xim.libxpkg.xvm")
    if not ok_xvm or not xvm_mod then
        xvm_mod = _LIBXPKG_MODULES and _LIBXPKG_MODULES["xvm"]
    end
    if not xvm_mod then
        if log then log.debug("xvm: module not available") end
        return nil
    end
    local ns, bare = _parse_namespace(dep_name)
    local candidates = ns and {ns .. "-" .. bare, bare} or {bare}
    if log then log.debug("xvm candidates: %s", table.concat(candidates, ", ")) end
    for _, xvm_name in ipairs(candidates) do
        local info = xvm_mod.info(xvm_name, dep_version)
        if log then log.debug("xvm.info(%s) = %s",
            xvm_name, info and ("SPath=" .. tostring(info["SPath"])) or "nil") end
        if info and info["SPath"] and info["SPath"] ~= "" then
            local spath = info["SPath"]
            local pver = (info["Version"] or dep_version or ""):gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1")
            if pver ~= "" then
                local head = spath:match("^(.*)" .. pver)
                if head then
                    return path.join(head:gsub("[/\\]+$", ""), info["Version"] or dep_version)
                end
            end
        end
    end
    if log then log.debug("xvm: not found") end
    return nil
end

function M.dep_install_dir(dep_name, dep_version)
    local result = _resolve_dep_via_scan(dep_name, dep_version)
    if result then return result end
    return _resolve_dep_via_xvm(dep_name, dep_version)
end

function M.install_dir(pkgname, pkgversion)
    if not pkgname then
        return _RUNTIME and _RUNTIME.install_dir or nil
    end
    local dir = M.dep_install_dir(pkgname, pkgversion)
    if dir then return dir end
    local log = _get_log()
    if log then log.error("cannot get install dir for %s@%s",
        tostring(pkgname), tostring(pkgversion or "latest")) end
    return nil
end

-- ─────────────────────────────────────────────────────────────────────
-- build_dep API
-- ─────────────────────────────────────────────────────────────────────
-- Returns metadata about a build-time dep available to the current
-- install hook. Build deps are payloads xlings ensured are present in
-- the xpkgs store but did NOT activate in subos workspace. Use this
-- API instead of relying on PATH / shims when the consumer needs an
-- ABSOLUTE PATH or wants explicit version selection independent of
-- the user's active workspace.
--
--   local gcc = pkginfo.build_dep("gcc")
--   -- gcc.path    : install_dir of the chosen build dep version
--   -- gcc.bin     : <install_dir>/bin
--   -- gcc.version : resolved version string
--
-- Resolution order:
--   1. Env var XLINGS_BUILDDEP_<UPPER_NAME>_PATH (injected by the
--      xlings installer when the consumer's `build` deps were resolved
--      to a concrete version).
--   2. Fallback: scan xpkgs the same way `dep_install_dir` does.
--      Returns highest available version when version is omitted.
--
-- Returns nil if the build dep is not available.
function M.build_dep(dep_name, dep_version)
    local log = _get_log()
    if not dep_name or dep_name == "" then return nil end

    local function _upper(s) return (s:gsub("[^%w]", "_")):upper() end
    local env_key  = "XLINGS_BUILDDEP_" .. _upper(dep_name) .. "_PATH"
    local env_path = os.getenv(env_key)
    local install_dir
    if env_path and env_path ~= "" and os.isdir(env_path) then
        install_dir = env_path
        if log then log.debug("build_dep %s -> %s (via %s)",
            dep_name, env_path, env_key) end
    else
        install_dir = M.dep_install_dir(dep_name, dep_version)
        if log then log.debug("build_dep %s -> %s (via scan)",
            dep_name, tostring(install_dir)) end
    end

    if not install_dir then return nil end

    local resolved_ver = dep_version
    if not resolved_ver or resolved_ver == "" then
        -- The install_dir's leaf is the version (xpkgs/<store>/<ver>).
        resolved_ver = path.filename(install_dir)
    end

    return {
        path    = install_dir,
        bin     = path.join(install_dir, "bin"),
        include = path.join(install_dir, "include"),
        lib     = path.join(install_dir, "lib"),
        version = resolved_ver,
    }
end

-- Convenience: prepend every build dep's `bin/` to PATH for the
-- duration of the callback, then restore. Lets install hooks call
-- bare `gcc` / `patchelf` etc and pick up the build-dep version
-- without the hook needing to splice paths manually. The xlings
-- installer also pre-injects PATH globally for the hook subprocess,
-- so most hooks won't need this — useful only when an install hook
-- spawns sub-processes that need a different PATH.
function M.with_build_deps_on_path(build_dep_names, fn)
    local log = _get_log()
    local original_path = os.getenv("PATH") or ""
    local extra = {}
    for _, n in ipairs(build_dep_names or {}) do
        local d = M.build_dep(n)
        if d and d.bin and os.isdir(d.bin) then
            table.insert(extra, d.bin)
        elseif log then
            log.warn("with_build_deps_on_path: %s not available", n)
        end
    end
    if #extra == 0 then return fn() end
    local new_path = table.concat(extra, path.envsep()) .. path.envsep() .. original_path
    os.setenv("PATH", new_path)
    local ok, err = pcall(fn)
    os.setenv("PATH", original_path)
    if not ok then error(err) end
end

return M
