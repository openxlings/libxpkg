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

-- Compare two dotted version strings numerically. Components that are not
-- numbers compare as text, so a date or a hash still orders deterministically.
local function _version_cmp(a, b)
    local ai, bi = a:gmatch("[^.]+"), b:gmatch("[^.]+")
    while true do
        local x, y = ai(), bi()
        if x == nil and y == nil then return 0 end
        local nx, ny = tonumber(x or "0"), tonumber(y or "0")
        if nx and ny then
            if nx ~= ny then return nx < ny and -1 or 1 end
        else
            local sx, sy = x or "", y or ""
            if sx ~= sy then return sx < sy and -1 or 1 end
        end
    end
end

-- Does an installed version satisfy a dependency's version EXPRESSION?
--
-- The expression is what a recipe wrote — `2.39`, `>=2.38`, `>=1.0 <2.0` — and
-- it is not a directory name. Joining it to a path was the bug this replaces:
-- `xpkgs/xim-x-glibc/>=2.39` does not exist, so the scan found nothing and the
-- caller fell back to whatever version the workspace had ACTIVE. Meanwhile
-- xlings computed the interpreter from the version the RESOLVER chose. With
-- two versions of glibc in one home those are different answers, and a binary
-- whose INTERP is one glibc and whose RUNPATH is another segfaults on the
-- first instruction — no diagnostic, because both halves are individually
-- sane.
--
-- Only the operators the resolver supports are handled. An expression this
-- does not understand returns false so the caller keeps its old behaviour
-- rather than matching something arbitrary.
local function _version_satisfies(ver, expr)
    if not expr or expr == "" then return true end
    for token in expr:gmatch("%S+") do
        local op, want = token:match("^([<>=~^]*)(.+)$")
        if want == nil then return false end
        if op == "" or op == "=" or op == "==" then
            if ver ~= want then return false end
        elseif op == ">=" then
            if _version_cmp(ver, want) < 0 then return false end
        elseif op == ">" then
            if _version_cmp(ver, want) <= 0 then return false end
        elseif op == "<=" then
            if _version_cmp(ver, want) > 0 then return false end
        elseif op == "<" then
            if _version_cmp(ver, want) >= 0 then return false end
        elseif op == "^" or op == "~" then
            -- Floor plus a ceiling on the component the operator pins: `^` on
            -- major, `~` on minor.
            if _version_cmp(ver, want) < 0 then return false end
            local keep = (op == "^") and 1 or 2
            local a, b = {}, {}
            for c in ver:gmatch("[^.]+")  do a[#a+1] = c end
            for c in want:gmatch("[^.]+") do b[#b+1] = c end
            for i = 1, keep do
                if (a[i] or "") ~= (b[i] or "") then return false end
            end
        else
            return false
        end
    end
    return true
end

local function _scan_dir(base, ns, bare, dep_version)
    if not base or not os.isdir(base) then return nil end
    local dirs = os.dirs(path.join(base, "*")) or {}
    for _, dep_root in ipairs(dirs) do
        local dirname = path.filename(dep_root)
        if _match_store_name(dirname, ns, bare) then
            -- Exact name first: unchanged for every recipe that pins, and it
            -- also covers a version string that is not semver at all.
            if dep_version then
                local exact = path.join(dep_root, dep_version)
                if os.isdir(exact) then return exact end
            end
            -- Otherwise the highest INSTALLED version that satisfies the
            -- expression — the same choice the resolver makes, which is the
            -- point: these two have to agree.
            local vers = os.dirs(path.join(dep_root, "*")) or {}
            local best = nil
            for _, vdir in ipairs(vers) do
                local v = path.filename(vdir)
                if _version_satisfies(v, dep_version)
                   and (best == nil or _version_cmp(v, best) > 0) then
                    best = v
                end
            end
            if best then
                local install_dir = path.join(dep_root, best)
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

local function _is_exact_store_version(version)
    if type(version) ~= "string" or version == "" or version == "latest"
       or version:find("%s") or version:find("/", 1, true)
       or version:find("\\", 1, true) or version:sub(1, 1) == "."
       or version:sub(-1) == "." or version:find("..", 1, true) then
        return false
    end
    for _, marker in ipairs({"<", ">", "=", "~", "^", "*", "?", "[", "]"}) do
        if version:find(marker, 1, true) then return false end
    end
    for component in version:gmatch("[^.]+") do
        if component:lower() == "x" then return false end
    end
    return true
end

local function _resolve_dep_via_explicit_roots(dep_name, dep_version)
    if not _is_exact_store_version(dep_version) then
        return nil
    end
    local ns, bare = _parse_namespace(dep_name)
    if not ns then return nil end
    for _, root in ipairs((_RUNTIME and _RUNTIME.dependency_store_roots) or {}) do
        local exact = path.join(root, ns .. "-x-" .. bare, dep_version)
        if os.isdir(exact) then return exact end
    end
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

-- Does an installed version answer what the caller asked for?
--
-- The version half of a dep is an EXPRESSION -- `2.39`, `>=2.38`, `^1.2` -- and
-- comparing it to a concrete version as a string makes them unequal. That is
-- the same mistake as openxlings/xlings#481, where `xim:glibc@>=2.38` matched
-- no plan node, so nothing got an RPATH and the package installed reporting
-- success. Omitting the version means "whatever the resolver picked", which is
-- the whole reason a resolver record exists.
local function _version_matches_request(have, want)
    if want == nil or want == "" then return true end
    if have == want then return true end
    return _version_satisfies(have, want)
end

-- The resolver's record for a dependency, if this client sends one.
--
-- type(), not truthiness: an unknown _RUNTIME field is nil here, but the same
-- probe written as `if _RUNTIME.resolved_deps then` on a module proxy is true
-- everywhere — a trap this repo has fallen into twice (subos.env,
-- xim.pkgindex.sysroot).
--
-- Namespaced requests match their exact spec/canonical identity. Bare requests
-- are answered when the record set answers them UNIQUELY.
--
-- Returns the record, or nil plus a reason ("ambiguous") the caller can use to
-- avoid explaining the same failure twice. Extra return values are ignored by
-- every existing caller, so this stays additive.
--
-- 0.0.55 gated the bare branch on `_is_exact_store_version(dep_version)` as
-- well. That precondition disambiguated NOTHING -- the uniqueness guard below
-- was already there and already failed closed -- and its only effect was to
-- reject the unversioned query, which is the shape every recipe in the index
-- actually writes. Measured (openxlings/xlings#524): 6 of the 7 real
-- `dep_install_dir` call sites returned nil, gcc and meson could not install on
-- any cold home at all, and godot silently fell back to the host's GL.
--
-- The distinction that matters: ambiguity is a property of the RECORD SET, not
-- of how specific the question was. `resolved_deps` is a closed table of this
-- package's own declared deps, so "is there a second record with this bare
-- name" is decidable here. That is exactly what makes it different from the
-- explicit store roots below, where refusing to guess IS the right answer.
function M.resolved_dep(dep_name, dep_version)
    local t = _RUNTIME and _RUNTIME.resolved_deps
    if type(t) ~= "table" then return nil end
    local ns, bare = _parse_namespace(dep_name)
    if not ns then
        local candidate, candidate_name = nil, nil
        for spec, rec in pairs(t) do
            local canonical = rec.name or spec:gsub("@.*", "")
            local _, record_bare = _parse_namespace(canonical)
            if record_bare == bare
               and _version_matches_request(rec.version, dep_version) then
                -- Two providers, one bare name: fail closed -- and name them.
                -- "not found" would send the reader looking for a missing
                -- payload when the payload is there twice.
                if candidate then
                    local log = _get_log()
                    if log then
                        log.error("dep_install_dir(%s): ambiguous -- %s and %s "
                                  .. "both provide this name. Pass the "
                                  .. "namespaced coordinate, as declared.",
                                  tostring(dep_name), tostring(candidate_name),
                                  tostring(canonical))
                    end
                    return nil, "ambiguous"
                end
                candidate, candidate_name = rec, canonical
            end
        end
        return candidate
    end

    if dep_version and dep_version ~= "" then
        local exact = t[dep_name .. "@" .. dep_version]
        if exact then return exact end
    end
    for spec, rec in pairs(t) do
        local sname = spec:gsub("@.*", "")
        if sname == dep_name then
            -- Same range rule as the bare branch. Asking for `>=2.38` when the
            -- recipe declared `>=2.39` is a legitimate question with the same
            -- answer; string equality called it a miss.
            if spec == dep_name .. "@" .. (dep_version or "")
               or _version_matches_request(rec.version, dep_version) then
                return rec
            end
        end
    end
    return nil
end

-- Where a dependency actually lives.
--
-- An exact resolver record is authoritative when present. Dependencies from
-- separate host domains are looked up only in the ordered roots the host
-- supplies. Scan/XVM guessing remains solely for old contexts where that roots
-- field is absent, and is noisy on purpose so "we guessed" is never silent.
--
-- Two independent answers is exactly how a binary ends up with its INTERP
-- from one glibc and its RUNPATH from another, which segfaults before main
-- with no diagnostic. See
-- xlings/.agents/docs/2026-08-05-dependency-resolution-single-source.md
function M.dep_install_dir(dep_name, dep_version)
    local rec, why = M.resolved_dep(dep_name, dep_version)
    if rec then
        if rec.install_dir and rec.install_dir ~= ""
           and os.isdir(rec.install_dir) then
            return rec.install_dir
        end
        local log = _get_log()
        if log then
            log.error("dep_install_dir(%s): resolver record points to missing "
                      .. "payload: %s", tostring(dep_name),
                      tostring(rec.install_dir))
        end
        return nil
    end

    local roots = _RUNTIME and _RUNTIME.dependency_store_roots
    if type(roots) == "table" then
        local hit = _resolve_dep_via_explicit_roots(dep_name, dep_version)
        if hit then return hit end

        -- Explicit roots answer an EXACT, NAMESPACED coordinate and nothing
        -- else, on purpose: a bare `zlib` would have to pick between
        -- `compat-x-zlib` and `other-x-zlib`, and guessing is the decoy problem
        -- these roots exist to remove. So nil is the right ANSWER here.
        --
        -- It was the wrong SILENCE. 0.0.55 returned nil for an underspecified
        -- query with no word to the caller, and a recipe cannot tell that from
        -- "the dependency is not installed".
        --
        -- Reaching here now means `resolved_deps` genuinely could not answer:
        -- the dependency was never declared by this package (a transitive dep,
        -- or a payload some hook installed itself). A bare name additionally
        -- cannot be looked up in the roots, because they key on an exact
        -- namespaced coordinate. Say which of the two it is.
        --
        -- `why == "ambiguous"` is the third case, and it already printed a
        -- better message naming both providers -- repeating "a bare name
        -- cannot be resolved" underneath it would point at the wrong fix.
        local ns = _parse_namespace(dep_name)
        local log = _get_log()
        if log and _RUNTIME and _RUNTIME.install_dir and why ~= "ambiguous" then
            if not ns then
                log.error("dep_install_dir(%s): a bare name cannot be resolved "
                          .. "against explicit dependency stores -- it does not "
                          .. "say which namespace. Pass the coordinate as "
                          .. "declared, e.g. \"ns:%s\".",
                          tostring(dep_name), tostring(dep_name))
            elseif dep_version == nil or dep_version == "" then
                -- Namespaced, no version, and no record. "Pass an exact
                -- version" -- what this used to say -- is circular advice: the
                -- caller omitted the version precisely so the resolver's own
                -- choice would be used, and the real problem is that there is
                -- no record to choose from. Name that instead.
                log.error("dep_install_dir(%s): %s is not a declared "
                          .. "dependency of %s, so the resolver has no record "
                          .. "of it. Declare it in `deps`, or use "
                          .. "tool_payload_dir if this hook installed it "
                          .. "itself. Declared here: %s",
                          tostring(dep_name), tostring(dep_name),
                          tostring(M.name() or "this package"),
                          (#M.deps_list() > 0
                             and table.concat(M.deps_list(), ", ") or "<none>"))
            elseif not _is_exact_store_version(dep_version) then
                log.error("dep_install_dir(%s, %s): explicit dependency stores "
                          .. "need an exact version. Omit the version to use "
                          .. "the resolved dependency record instead.",
                          tostring(dep_name), tostring(dep_version))
            end
        end
        return nil
    end

    local result = _resolve_dep_via_scan(dep_name, dep_version)
    if not result then
        result = _resolve_dep_via_xvm(dep_name, dep_version)
    end
    local log = _get_log()
    if log and _RUNTIME and _RUNTIME.install_dir then
        -- Inside an install, a missing roots field means the client predates
        -- the explicit host-store contract.
        -- Outside one there is nothing to miss, so no warning.
        log.warn("dep_install_dir(%s): no explicit store roots, fell back to a "
                 .. "scan -> %s", tostring(dep_name), tostring(result))
    end
    return result
end

function M.install_dir(pkgname, pkgversion)
    if not pkgname then
        return _RUNTIME and _RUNTIME.install_dir or nil
    end
    local dir = M.dep_install_dir(pkgname, pkgversion)
    if dir then return dir end

    -- Say WHY, not "cannot".
    --
    -- "cannot get install dir" names an internal state, and the two causes it
    -- covers point in opposite directions: a path/permission problem, and a
    -- package that is not a dependency here at all. Measured (openxlings/
    -- xlings#487): a macOS install of ollama reported it for
    -- `xim:libcuda-host-link`, a linux-only sentinel that macOS never
    -- resolves -- the hook asked for it unconditionally because its branch
    -- tested `is_host("windows")` when the real distinction was linux. The
    -- message sent the reader looking at paths.
    --
    -- `deps_list` is what the resolver produced FOR THIS PLATFORM, so a name
    -- missing from it is not "unresolvable" -- it is "not a dependency of
    -- this package here".
    local log = _get_log()
    if log then
        local want = tostring(pkgname)
        local bare = want:gsub("^[%w_-]+:", "")
        local declared = false
        for _, d in ipairs(M.deps_list()) do
            local dn = tostring(d):gsub("@.*$", "")
            if dn == want or dn:gsub("^[%w_-]+:", "") == bare then
                declared = true
                break
            end
        end
        if declared then
            log.error("install dir for %s@%s: declared as a dependency of %s "
                      .. "but no payload is on disk -- the dependency install "
                      .. "did not complete",
                      want, tostring(pkgversion or "latest"),
                      tostring(M.name() or "this package"))
        else
            log.error("install dir for %s@%s: NOT a dependency of %s on this "
                      .. "platform (%s). Its deps here are: %s. A package "
                      .. "declared only under another platform's xpm section "
                      .. "is never resolved on this one -- guard the hook that "
                      .. "asks for it with the platform it belongs to.",
                      want, tostring(pkgversion or "latest"),
                      tostring(M.name() or "this package"),
                      tostring(os.host()),
                      (#M.deps_list() > 0
                         and table.concat(M.deps_list(), ", ") or "<none>"))
        end
    end
    return nil
end

-- The payload of a tool libxpkg ITSELF needs, e.g. patchelf for elfpatch.
--
-- This deliberately keeps its compatibility scan separate from
-- dep_install_dir's explicit host-domain roots. A tool of libxpkg is not a
-- dependency of the package being installed -- nothing should have recorded
-- it or supplied its store domain -- so the dependency fallback warning here
-- would be a false report on every install, and one the user cannot act on.
--
-- Returns the install_dir, or nil. Callers must handle nil: on a home where the
-- tool's package is not installed there is no payload answer, and inventing one
-- is exactly the failure this function exists to prevent.
function M.tool_payload_dir(tool_pkg, tool_version)
    if not tool_pkg or tool_pkg == "" then return nil end
    local rec = M.resolved_dep(tool_pkg, tool_version)
    if rec and rec.install_dir and rec.install_dir ~= "" then
        return rec.install_dir
    end
    return _resolve_dep_via_scan(tool_pkg, tool_version)
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
--      to a concrete version). <UPPER_NAME> is the dep's bare name --
--      namespace and @version dropped, then upper-cased with every
--      non-alphanumeric mapped to `_` -- exactly as xlings derives it, so
--      "xim:7zip", "7zip" and "xim:7zip@26.02" all read XLINGS_BUILDDEP_7ZIP_PATH.
--   2. Fallback: scan xpkgs the same way `dep_install_dir` does.
--      Returns highest available version when version is omitted.
--
-- Returns nil if the build dep is not available.
function M.build_dep(dep_name, dep_version)
    local log = _get_log()
    if not dep_name or dep_name == "" then return nil end

    -- Must spell the name the way xlings does when it exports the variable
    -- (installer.cpp): drop from the first '@', then up to and including the
    -- first ':', then upper-case alphanumerics and map the rest to '_'.
    local function _env_name(s)
        local at = s:find("@", 1, true)
        if at then s = s:sub(1, at - 1) end
        local colon = s:find(":", 1, true)
        if colon then s = s:sub(colon + 1) end
        return (s:gsub("[^%w]", "_")):upper()
    end
    local env_key  = "XLINGS_BUILDDEP_" .. _env_name(dep_name) .. "_PATH"
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
