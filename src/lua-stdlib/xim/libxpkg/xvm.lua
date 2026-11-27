-- xim.libxpkg.xvm: version management integration (collects ops for C++ processing)
local M = {}

local function _get_log()
    return _LIBXPKG_MODULES and _LIBXPKG_MODULES["log"]
end

_XVM_OPS = _XVM_OPS or {}

-- Where each shell looks for a command's completion, under the SubOS root.
local _COMPLETION_DST = {
    bash = function(name) return "usr/share/bash-completion/completions/" .. name end,
    zsh  = function(name) return "usr/share/zsh/site-functions/_" .. name end,
    fish = function(name) return "usr/share/fish/vendor_completions.d/" .. name .. ".fish" end,
    pwsh = function(name) return "usr/share/powershell/completions/" .. name .. ".ps1" end,
}

local function _completion(name, entry, completion)
    if type(completion) ~= "table" then
        error(string.format("xvm.add(%q): completion is a table of shell = file", name), 3)
    end
    local shells = {}
    for shell in pairs(completion) do table.insert(shells, shell) end
    table.sort(shells)
    for _, shell in ipairs(shells) do
        local src = completion[shell]
        if not _COMPLETION_DST[shell] then
            error(string.format("xvm.add(%q): completion for an unknown shell %q (bash, zsh, fish, pwsh)",
                name, tostring(shell)), 3)
        end
        if type(src) ~= "string" or src == "" or src:match("^[/\\]") or src:match("^%a:")
            or src:match("%.%.") then
            error(string.format("xvm.add(%q): completion.%s is a file of the payload, relative to its root",
                name, shell), 3)
        end
        table.insert(_XVM_OPS, {
            op = "add",
            name = name .. ".completion." .. shell,
            version = entry.version,
            bindir = (_RUNTIME and _RUNTIME.install_dir) or "",
            alias = "", filename = "",
            type = "files",
            binding = entry.binding ~= "" and entry.binding or (name .. "@" .. entry.version),
            src = src,
            dst = _COMPLETION_DST[shell](name),
        })
    end
end

--- Register one entry with the version manager.
-- @param name  target name
-- @param opt   table with:
--   type      "program" (default) | "lib" | "files"
--   version   defaults to the package version
--   bindir    directory holding the artifact (default: install_dir)
--   filename  artifact name inside bindir
--   alias     name it is exposed under
--   binding   "<root>@<version>" -- which release this belongs to
--   args      list of arguments injected ahead of the user's own when the
--             shim dispatches. Use this rather than appending to `alias`:
--             consumers split the alias on its first space, so a path with
--             one in it breaks, and every reader of `alias` then shows a
--             command line where a name belongs.
--   envs      environment variables
--   completion  { bash = src, zsh = src, fish = src, pwsh = src }: the
--             program's shell completion, files of the payload (relative to
--             its root). Each becomes a `files` entry bound to this release
--             (its `binding`, else name@version), placed where that shell
--             looks in the SubOS: usr/share/bash-completion/completions/<name>,
--             usr/share/zsh/site-functions/_<name>,
--             usr/share/fish/vendor_completions.d/<name>.fish,
--             usr/share/powershell/completions/<name>.ps1. It follows `use`,
--             goes with the release and is loaded by xlings' profile.
--
-- For type = "files", the entry describes an asset placed into the subos
-- instead of an artifact to dispatch:
--   src       source, relative to the payload root
--   dst       destination, relative to the subos root
--
-- Both must be relative. A payload is shared between subos and
-- reference-counted, so an absolute destination recorded against it would
-- be correct for exactly one subos and wrong for the rest.
function M.add(name, opt)
    opt = opt or {}
    local entry = {
        op = "add",
        name = name,
        version  = opt.version or (_RUNTIME and _RUNTIME.version) or "",
        bindir   = opt.bindir  or (_RUNTIME and _RUNTIME.install_dir) or "",
        alias    = opt.alias or "",
        type     = opt.type or "",
        filename = opt.filename or "",
        binding  = opt.binding or "",
        src      = opt.src or "",
        dst      = opt.dst or "",
        args     = opt.args or nil,
        envs     = opt.envs or nil,
    }
    local log = _get_log()
    if log then log.debug("xvm add %s version=%s", name, entry.version) end
    table.insert(_XVM_OPS, entry)
    if opt.completion ~= nil then _completion(name, entry, opt.completion) end
end


--- Declare an asset this package places into the subos.
--
-- Sugar over `M.add` for the case where the entry is a file rather than
-- something to dispatch, so the caller does not have to invent a target
-- name: one is derived from the package name. A release may declare several,
-- and each call adds one.
--
-- @param opt  src / dst (both relative, see M.add), plus binding
function M.files(opt)
    opt = opt or {}
    if not opt.src or opt.src == "" then
        error("xvm.files: src is required")
    end
    if not opt.dst or opt.dst == "" then
        error("xvm.files: dst is required")
    end
    local owner = opt.name
        or (_RUNTIME and _RUNTIME.pkg_name)
        or "xvm"
    -- Derived rather than caller-supplied so two declarations from one
    -- package cannot collide on the same target name.
    _XVM_FILES_SEQ = (_XVM_FILES_SEQ or 0) + 1
    local target = string.format("%s.files.%d", owner, _XVM_FILES_SEQ)
    M.add(target, {
        type    = "files",
        src     = opt.src,
        dst     = opt.dst,
        version = opt.version,
        binding = opt.binding,
    })
end

function M.remove(name, version)
    local log = _get_log()
    if log then log.debug("xvm remove %s %s", name, version or "") end
    table.insert(_XVM_OPS, {op = "remove", name = name, version = version or ""})
end

function M.use(name, version)
    -- stub: version switching handled by C++ side
end

-- Load VersionDB from config files (global + project)
local _versions_cache = nil
local function _load_versions()
    if _versions_cache then return _versions_cache end
    local ok_json, json_mod = pcall(require, "xim.libxpkg.json")
    if not ok_json then
        json_mod = _LIBXPKG_MODULES and _LIBXPKG_MODULES["json"]
    end
    if not json_mod then return nil end

    local function load_file(config_path)
        local f = io.open(config_path, "r")
        if not f then return nil end
        local content = f:read("*a"); f:close()
        if not content or content == "" then return nil end
        local ok, data = pcall(json_mod.decode, content)
        if not ok or type(data) ~= "table" then return nil end
        return data.versions or nil
    end

    local merged = {}
    -- 1. Load global versions: prefer XLINGS_HOME, fallback to ~/.xlings
    local xlings_home = os.getenv("XLINGS_HOME")
    if not xlings_home or xlings_home == "" then
        local home = os.getenv("HOME") or os.getenv("USERPROFILE") or ""
        xlings_home = home .. "/.xlings"
    end
    local global_versions = load_file(xlings_home .. "/.xlings.json")
    if global_versions then
        for k, v in pairs(global_versions) do merged[k] = v end
    end
    -- 2. Load project versions (project_data_dir is 2 levels below project root)
    if _RUNTIME and _RUNTIME.project_data_dir and _RUNTIME.project_data_dir ~= "" then
        local project_dir = path.directory(path.directory(_RUNTIME.project_data_dir))
        local project_versions = load_file(path.join(project_dir, ".xlings.json"))
        if project_versions then
            for k, v in pairs(project_versions) do merged[k] = v end
        end
    end
    _versions_cache = merged
    return _versions_cache
end

function M.has(name, version)
    -- Check pending ops first (current session adds)
    for _, entry in ipairs(_XVM_OPS) do
        if entry.op == "add" and entry.name == name then return true end
    end
    -- Check persisted VersionDB
    local versions = _load_versions()
    if not versions then return false end
    local vinfo = versions[name]
    if not vinfo then return false end
    if not version or version == "" then return true end
    if vinfo.versions then
        for ver_key, _ in pairs(vinfo.versions) do
            if ver_key == version then return true end
        end
    end
    return false
end

function M.info(name, version)
    local versions = _load_versions()
    if not versions then return nil end
    local vinfo = versions[name]
    if not vinfo or not vinfo.versions then return nil end

    -- Find matching version
    local vdata = nil
    local matched_version = version or ""
    if version and version ~= "" then
        vdata = vinfo.versions[version]
    end
    if not vdata then
        -- Try first available version
        for ver_key, ver_val in pairs(vinfo.versions) do
            vdata = ver_val
            matched_version = ver_key
            break
        end
    end
    if not vdata then return nil end

    local info_table = {
        Name = name,
        Version = matched_version,
        Type = vinfo.type or "program",
        Program = vinfo.filename or name,
        SPath = vdata.path or "",
        TPath = vdata.path or "",
        Alias = vdata.alias or nil,
        Envs = vdata.envs or nil,
    }
    return info_table
end

-- Deprecated: use log.set_level() instead. Kept for backward compat.
function M.log_tag(enable)
    return enable
end

--- Unified registration of programs, libraries, and headers
-- @param name      package name (root node in binding tree)
-- @param opt       table with:
--   install_dir  install root directory (default _RUNTIME.install_dir)
--   version      version string (default _RUNTIME.version)
--   bindir       programs directory (relative or absolute, default "bin")
--   libdir       library directory (relative or absolute, optional)
--   includedir   header directory (relative or absolute, optional)
--   programs     list of program names (optional)
--   libs         list of library filenames (optional)
function M.setup(name, opt)
    opt = opt or {}
    local install_dir = opt.install_dir or (_RUNTIME and _RUNTIME.install_dir) or ""
    local version = opt.version or (_RUNTIME and _RUNTIME.version) or ""
    local binding = name .. "@" .. version

    local function resolve(dir)
        if not dir then return nil end
        if path.is_absolute(dir) then return dir end
        return path.join(install_dir, dir)
    end

    -- 1. Register root node
    M.add(name)

    -- 2. Batch register programs
    if opt.programs then
        local bindir = resolve(opt.bindir or "bin")
        for _, prog in ipairs(opt.programs) do
            M.add(prog, { bindir = bindir, binding = binding })
        end
    end

    -- 3. Batch register libraries
    if opt.libs then
        local libdir = resolve(opt.libdir or "lib")
        for _, lib in ipairs(opt.libs) do
            M.add(lib, {
                type = "lib", bindir = libdir,
                alias = lib, filename = lib,
                binding = binding,
            })
        end
    end

    -- 4. Header directory -> C++ side creates symlinks to sysroot
    if opt.includedir then
        local includedir = resolve(opt.includedir)
        table.insert(_XVM_OPS, { op = "headers", includedir = includedir })
    end
end

--- Unified unregistration
function M.teardown(name, opt)
    opt = opt or {}
    M.remove(name)
    if opt.programs then
        for _, prog in ipairs(opt.programs) do M.remove(prog) end
    end
    if opt.libs then
        for _, lib in ipairs(opt.libs) do M.remove(lib) end
    end
    if opt.includedir then
        local install_dir = opt.install_dir or (_RUNTIME and _RUNTIME.install_dir) or ""
        local includedir = opt.includedir
        if not path.is_absolute(includedir) then
            includedir = path.join(install_dir, includedir)
        end
        table.insert(_XVM_OPS, { op = "remove_headers", includedir = includedir })
    end
end

return M
