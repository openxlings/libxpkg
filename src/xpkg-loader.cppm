module;

export module mcpplibs.xpkg.loader;
import mcpplibs.xpkg;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;
namespace fs  = std::filesystem;

export namespace mcpplibs::xpkg {

// Where a package index's build script writes its console output, when the
// caller wants it rather than the process's stdout.
//
// pkgindex-build.lua reports progress as a self-refreshing terminal line
// ("\r[i/n] ns::file\033[K"). Written to fd 1 it reaches a file or a pipe as
// carriage returns and escape sequences, and only the caller knows where its
// output goes. With a BuildOutput set, the script's `io.write` and `print` hand
// their text here, unchanged and in order; the caller decides what a line is
// and how to show it. Without one, nothing changes.
using BuildOutput = std::function<void(std::string_view)>;

struct LoaderContext {
    std::string platform;
    std::string arch;
};

struct MetadataInvocation {
    fs::path package;
    LoaderContext context;
};
using MetadataBoundary = std::function<std::expected<Package, std::string>(const MetadataInvocation&)>;
using IndexBuildBoundary = std::function<std::expected<void, std::string>(const fs::path&, const BuildOutput&)>;

struct LoaderBoundaries {
    MetadataBoundary metadata;
    IndexBuildBoundary index_build;
};


} // export namespace mcpplibs::xpkg

namespace mcpplibs::xpkg::loader_detail {

std::expected<PackageIndex, std::string>
build_index_impl(const fs::path& repo_dir, const std::string& defaultNamespace,
                 const BuildOutput& buildOutput, const LoaderContext& context,
                 const LoaderBoundaries* boundaries);


// Register loader sandbox: no-op import() + defensive stubs for non-standard
// globals. This gives the loader a self-contained pure Lua 5.4 environment
// that does not depend on any xmake runtime. Stubs ensure legacy or
// third-party packages can still be loaded without crashing.
void register_loader_sandbox(
    lua::State* L,
    std::string_view platform,
    std::string_view arch) {
    lua::pushstring(L, std::string(platform).c_str());
    lua::setglobal(L, "__xpkg_platform__");
    lua::pushstring(L, std::string(arch).c_str());
    lua::setglobal(L, "__xpkg_arch__");
    lua::L_dostring(L,
        // no-op import: returns a deep proxy and sets it as a global
        // (matches xmake behavior where import("platform") sets _G.platform)
        // The proxy returns '' for string operations and nested proxies for
        // chained access like platform.get_config_info().rundir
        "do "
        "  local function make_proxy() "
        "    local p = setmetatable({}, { "
        "      __index = function(_, k) "
        "        return make_proxy() "
        "      end, "
        "      __call = function() return make_proxy() end, "
        "      __tostring = function() return '' end, "
        "      __concat = function(a, b) return tostring(a) .. tostring(b) end "
        "    }) "
        "    return p "
        "  end "
        "  import = function(name, ...) "
        "    local proxy = make_proxy() "
        "    if type(name) == 'string' then "
        "      local short = name:match('[^.]+$') or name "
        "      rawset(_G, short, proxy) "
        "    end "
        "    return proxy "
        "  end "
        "end\n"

        // non-standard global stubs
        "function is_host(...) "
        "  for i = 1, select('#', ...) do "
        "    if select(i, ...) == __xpkg_platform__ then return true end "
        "  end "
        "  return false "
        "end\n"
        "function is_arch(...) "
        "  for i = 1, select('#', ...) do "
        "    if select(i, ...) == __xpkg_arch__ then return true end "
        "  end "
        "  return false "
        "end\n"
        "is_plat = is_host\n"
        "_RUNTIME = { platform = __xpkg_platform__, arch = __xpkg_arch__ }\n"
        "format = string.format\n"

        // os extensions (safe defaults)
        "os.host      = function() return __xpkg_platform__ ~= '' and __xpkg_platform__ or 'unknown' end\n"
        "os.arch      = function() return __xpkg_arch__ ~= '' and __xpkg_arch__ or 'unknown' end\n"
        "os.isfile    = os.isfile or function() return false end\n"
        "os.isdir     = os.isdir or function() return false end\n"
        "os.scriptdir = os.scriptdir or function() return '.' end\n"
        "os.dirs      = os.dirs or function() return {} end\n"
        "os.files     = os.files or function() return {} end\n"
        "os.exists    = os.exists or function() return false end\n"
        "os.tryrm     = os.tryrm or function() end\n"
        "os.trymv     = os.trymv or function() end\n"
        "os.iorun     = os.iorun or function() return nil end\n"
        "os.cd        = os.cd or function() end\n"
        "os.mkdir     = os.mkdir or function() end\n"
        "os.sleep     = os.sleep or function() end\n"

        // path module stub (robust to nil args from other stubs)
        "path = path or {}\n"
        "path.join      = path.join or function(...) "
        "  local parts = {} "
        "  for i = 1, select('#', ...) do "
        "    local v = select(i, ...) "
        "    if v ~= nil then parts[#parts+1] = tostring(v) end "
        "  end "
        "  return table.concat(parts, '/') "
        "end\n"
        "path.filename  = path.filename or function(p) return type(p)=='string' and (p:match('[^/\\\\]+$') or p) or '' end\n"
        "path.directory = path.directory or function(p) return type(p)=='string' and (p:match('(.*)[/\\\\]') or '.') or '.' end\n"
        "path.basename  = path.basename or function(p) return type(p)=='string' and (p:match('[^/\\\\]+$') or p) or '' end\n"

        // io extensions
        "io.readfile  = io.readfile or function() return '' end\n"
        "io.writefile = io.writefile or function() end\n"

        // try/catch stub
        "try = try or function(block) pcall(block[1]) end\n"

        // cprint stub
        "cprint = cprint or print\n"

        // string extensions
        "string.replace = string.replace or function(s, old, new) return s:gsub(old, new) end\n"
        "string.split   = string.split or function(s, sep) "
        "  local r = {} "
        "  for m in (s .. sep):gmatch('(.-)' .. sep) do r[#r+1] = m end "
        "  return r "
        "end\n"

        // raise stub
        "raise = raise or function() end\n"

        // noop module-level stubs (return '' so path.join etc. get strings, not nil)
        "runtime = setmetatable({}, { __index = function() return function() return '' end end })\n"
        "system  = setmetatable({}, { __index = function() return function() return '' end end })\n"
        "libxpkg = setmetatable({}, { __index = function() return setmetatable({}, { __index = function() return function() return '' end end }) end })\n"
    );
}

std::unordered_map<std::string, std::string> get_str_map(lua::State* L, int idx, const char* key) {
    std::unordered_map<std::string, std::string> result;
    lua::getfield(L, idx, key);
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::pushnil(L);
        while (lua::next(L, -2)) {
            if (lua::type(L, -2) == lua::TSTRING && lua::type(L, -1) == lua::TSTRING)
                result[lua::tostring(L, -2)] = lua::tostring(L, -1);
            lua::pop(L, 1);
        }
    }
    lua::pop(L, 1);
    return result;
}

std::string get_str(lua::State* L, int idx, const char* key) {
    lua::getfield(L, idx, key);
    std::string r;
    if (lua::type(L, -1) == lua::TSTRING)
        r = lua::tostring(L, -1);
    lua::pop(L, 1);
    return r;
}

bool get_bool(lua::State* L, int idx, const char* key) {
    lua::getfield(L, idx, key);
    bool r = lua::toboolean(L, -1);
    lua::pop(L, 1);
    return r;
}

// A non-negative integer field, or 0. Lenient like the readers above: a
// malformed value reads as absent. The index's own CI rejects it.
int get_nonneg_int(lua::State* L, int idx, const char* key) {
    lua::getfield(L, idx, key);
    int r = 0;
    if (lua::isinteger(L, -1)) {
        auto v = lua::tointeger(L, -1);
        if (v >= 0 && v <= std::numeric_limits<int>::max())
            r = static_cast<int>(v);
    }
    lua::pop(L, 1);
    return r;
}

std::vector<std::string> get_str_array(lua::State* L, int idx, const char* key) {
    std::vector<std::string> result;
    lua::getfield(L, idx, key);
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::pushnil(L);
        while (lua::next(L, -2)) {
            if (lua::type(L, -1) == lua::TSTRING)
                result.push_back(lua::tostring(L, -1));
            lua::pop(L, 1);
        }
    }
    lua::pop(L, 1);
    return result;
}

PackageType parse_type(const std::string& s) {
    if (s == "script")   return PackageType::Script;
    if (s == "template") return PackageType::Template;
    if (s == "config")   return PackageType::Config;
    if (s == "subos")    return PackageType::Subos;
    return PackageType::Package;
}

PackageStatus parse_status(const std::string& s) {
    if (s == "stable")     return PackageStatus::Stable;
    if (s == "deprecated") return PackageStatus::Deprecated;
    return PackageStatus::Dev;
}

// Parse xpm platform matrix from the package table at idx
PlatformMatrix parse_xpm(lua::State* L, int pkg_idx) {
    PlatformMatrix xpm;
    lua::getfield(L, pkg_idx, "xpm");
    if (lua::type(L, -1) != lua::TTABLE) {
        lua::pop(L, 1);
        return xpm;
    }

    // Iterate platforms
    int xpm_idx = lua::gettop(L);

    // `source` is additive metadata: it supplies a default resource origin
    // without changing the existing platform/version matrix.
    xpm.source = get_str(L, xpm_idx, "source");
    if (xpm.source.empty()) {
        xpm.source_mirrors = get_str_map(L, xpm_idx, "source");
        if (auto it = xpm.source_mirrors.find("GLOBAL");
                it != xpm.source_mirrors.end())
            xpm.source = it->second;
        else if (!xpm.source_mirrors.empty())
            xpm.source = xpm.source_mirrors.begin()->second;
    }

    lua::pushnil(L);
    while (lua::next(L, xpm_idx)) {
        // key = platform name (at -2), value = version table (at -1)
        std::string platform;
        if (lua::type(L, -2) == lua::TSTRING)
            platform = lua::tostring(L, -2);

        if (!platform.empty() && platform != "source"
                && lua::type(L, -1) == lua::TTABLE) {
            int plat_idx = lua::gettop(L);

            auto platform_source = get_str(L, plat_idx, "source");
            if (!platform_source.empty()) {
                xpm.platform_sources[platform] = std::move(platform_source);
            } else {
                auto mirrors = get_str_map(L, plat_idx, "source");
                if (!mirrors.empty()) {
                    if (auto it = mirrors.find("GLOBAL"); it != mirrors.end())
                        xpm.platform_sources[platform] = it->second;
                    else
                        xpm.platform_sources[platform] = mirrors.begin()->second;
                    xpm.platform_source_mirrors[platform] = std::move(mirrors);
                }
            }

            // Parse deps. Two accepted shapes:
            //
            //   Legacy array form:
            //     deps = { "node", "npm" }
            //   → fan out to BOTH runtime_deps and build_deps for that
            //     platform; preserves pre-split behaviour.
            //
            //   New table form:
            //     deps = { runtime = {...}, build = {...} }
            //   → strict separation: runtime entries activate in workspace,
            //     build entries are install-time-only.
            //
            // `xpm.deps[platform]` is set to the union (runtime ∪ build)
            // so legacy consumers reading `deps` keep working.
            auto parse_string_array = [&](int idx) -> std::vector<std::string> {
                std::vector<std::string> out;
                if (lua::type(L, idx) != lua::TTABLE) return out;
                int len = static_cast<int>(lua::rawlen(L, idx));
                for (int i = 1; i <= len; ++i) {
                    lua::rawgeti(L, idx, i);
                    if (lua::type(L, -1) == lua::TSTRING)
                        out.push_back(lua::tostring(L, -1));
                    lua::pop(L, 1);
                }
                return out;
            };

            lua::getfield(L, plat_idx, "deps");
            if (lua::type(L, -1) == lua::TTABLE) {
                int deps_idx = lua::gettop(L);
                // Three shapes, and the third one used to be silently wrong.
                //
                //   deps = { "a", "b" }                    array   → both
                //   deps = { runtime = {...}, build = {...} }  table   → split
                //   deps = { "a", "b", build = {...} }     MIXED   → see below
                //
                // The mixed shape is what an author writes when they have a
                // list of runtime deps and want to add one build-time tool.
                // It parses, it reads exactly like the split form, and the
                // old code took the array branch on it: `build` was dropped
                // on the floor and the ARRAY entries were copied into
                // build_deps instead. Measured 2026-08-06 on
                // nvidia-gl-host-link -- `deps.build = {"xim:patchelf"}`
                // alongside five runtime deps installed no patchelf, and
                // nothing said so.
                //
                // Now: an author who writes `runtime`/`build` at all has
                // opted into the split, so honour it, and fold the array part
                // into runtime (which is what a list beside `build = {...}`
                // can only mean). Array-only keeps the legacy fan-out
                // untouched -- every existing recipe is that shape.
                int len = static_cast<int>(lua::rawlen(L, deps_idx));

                lua::getfield(L, deps_idx, "runtime");
                bool has_runtime_key = lua::type(L, -1) == lua::TTABLE;
                auto rt_key = parse_string_array(lua::gettop(L));
                lua::pop(L, 1);

                lua::getfield(L, deps_idx, "build");
                bool has_build_key = lua::type(L, -1) == lua::TTABLE;
                auto bd = parse_string_array(lua::gettop(L));
                lua::pop(L, 1);

                bool split = has_runtime_key || has_build_key;

                if (!split) {
                    auto v = parse_string_array(deps_idx);
                    xpm.runtime_deps[platform] = v;
                    xpm.build_deps[platform]   = v;
                    xpm.deps[platform]         = v;
                } else {
                    auto rt = rt_key;
                    if (len > 0) {
                        // The array part of a mixed table: runtime deps
                        // written positionally, with `build` beside them.
                        auto arr = parse_string_array(deps_idx);
                        for (auto& d : arr)
                            if (std::find(rt.begin(), rt.end(), d) == rt.end())
                                rt.push_back(d);
                    }

                    xpm.runtime_deps[platform] = rt;
                    xpm.build_deps[platform]   = bd;

                    // Build the union for legacy `deps` field
                    std::vector<std::string> uni = rt;
                    for (auto& d : bd) {
                        if (std::find(uni.begin(), uni.end(), d) == uni.end())
                            uni.push_back(d);
                    }
                    xpm.deps[platform] = std::move(uni);
                }
            }
            lua::pop(L, 1);  // pop deps field

            // Parse inherits if present
            lua::getfield(L, plat_idx, "inherits");
            if (lua::type(L, -1) == lua::TSTRING) {
                xpm.inherits[platform] = lua::tostring(L, -1);
            }
            lua::pop(L, 1);  // pop inherits field

            // Parse exports block. Schema (all sub-fields optional):
            //   exports = {
            //       runtime = {
            //           loader  = "lib64/ld-linux-x86-64.so.2",  -- libc only
            //           libdirs = { "lib64", "lib" },            -- non-default only
            //           abi     = "linux-x86_64-glibc",          -- multi-libc only
            //       },
            //   }
            // Absence of `exports` (or any sub-field) means "use the default
            // convention" — no export declared, predicate falls through.
            lua::getfield(L, plat_idx, "exports");
            if (lua::type(L, -1) == lua::TTABLE) {
                int exports_idx = lua::gettop(L);
                ExportsBlock block;

                lua::getfield(L, exports_idx, "runtime");
                if (lua::type(L, -1) == lua::TTABLE) {
                    int rt_idx = lua::gettop(L);
                    lua::getfield(L, rt_idx, "loader");
                    if (lua::type(L, -1) == lua::TSTRING)
                        block.runtime.loader = lua::tostring(L, -1);
                    lua::pop(L, 1);

                    lua::getfield(L, rt_idx, "libdirs");
                    if (lua::type(L, -1) == lua::TTABLE)
                        block.runtime.libdirs = parse_string_array(lua::gettop(L));
                    lua::pop(L, 1);

                    lua::getfield(L, rt_idx, "abi");
                    if (lua::type(L, -1) == lua::TSTRING)
                        block.runtime.abi = lua::tostring(L, -1);
                    lua::pop(L, 1);
                }
                lua::pop(L, 1);  // pop runtime sub-table

                xpm.exports[platform] = std::move(block);
            }
            lua::pop(L, 1);  // pop exports field

            // Iterate version entries
            lua::pushnil(L);
            while (lua::next(L, plat_idx)) {
                // key = version string (at -2), value = resource (at -1)
                std::string version;
                if (lua::type(L, -2) == lua::TSTRING)
                    version = lua::tostring(L, -2);

                // Skip non-version keys. `exports` is a platform-level table
                // (parsed above) that would otherwise leak in as a bogus
                // version entry; `deps`/`inherits` likewise.
                if (!version.empty() && version != "deps"
                        && version != "inherits" && version != "exports"
                        && version != "source") {
                    PlatformResource res;
                    if (lua::type(L, -1) == lua::TTABLE) {
                        int res_idx = lua::gettop(L);
                        res.url    = get_str(L, res_idx, "url");
                        if (res.url.empty()) {
                            res.mirrors = get_str_map(L, res_idx, "url");
                            if (auto it = res.mirrors.find("GLOBAL"); it != res.mirrors.end())
                                res.url = it->second;
                            else if (!res.mirrors.empty())
                                res.url = res.mirrors.begin()->second;
                        }
                        res.sha256 = get_str(L, res_idx, "sha256");
                        res.ref    = get_str(L, res_idx, "ref");
                        res.revision = get_nonneg_int(L, res_idx, "revision");

                        // ---- V2 multi-arch shapes ----
                        // Scheme C / res: `sha256` is a per-arch TABLE rather
                        // than a string (get_str returned "" for a table).
                        res.sha256_by_arch = get_str_map(L, res_idx, "sha256");
                        // Re-key the per-arch sha256 map to canonical arch names.
                        if (!res.sha256_by_arch.empty()) {
                            std::unordered_map<std::string, std::string> canon;
                            for (auto& [k, v] : res.sha256_by_arch)
                                canon[normalize_arch(k)] = v;
                            res.sha256_by_arch = std::move(canon);
                        }
                        // Optional ${arch_alias} mapping (canonical -> upstream token).
                        {
                            auto alias = get_str_map(L, res_idx, "arch_alias");
                            for (auto& [k, v] : alias)
                                res.arch_alias[normalize_arch(k)] = v;
                        }
                        res.is_res = get_bool(L, res_idx, "res");

                        // Scheme B: per-arch resource map. Detected when the
                        // entry carries no single-arch url/ref/sha256 and no
                        // template/res markers, but has arch-named subtables.
                        if (res.url.empty() && res.ref.empty() && res.sha256.empty()
                                && res.sha256_by_arch.empty() && !res.is_res) {
                            lua::pushnil(L);
                            while (lua::next(L, res_idx)) {
                                if (lua::type(L, -2) == lua::TSTRING
                                        && lua::type(L, -1) == lua::TTABLE) {
                                    std::string canon = normalize_arch(lua::tostring(L, -2));
                                    if (canon == "x86_64" || canon == "aarch64"
                                            || canon == "x86") {
                                        int arch_idx = lua::gettop(L);
                                        ArchResource ar;
                                        ar.url = get_str(L, arch_idx, "url");
                                        if (ar.url.empty()) {
                                            ar.mirrors = get_str_map(L, arch_idx, "url");
                                            if (auto it = ar.mirrors.find("GLOBAL");
                                                    it != ar.mirrors.end())
                                                ar.url = it->second;
                                            else if (!ar.mirrors.empty())
                                                ar.url = ar.mirrors.begin()->second;
                                        }
                                        ar.sha256 = get_str(L, arch_idx, "sha256");
                                        res.archs[canon] = std::move(ar);
                                    }
                                }
                                lua::pop(L, 1);  // pop value, keep key for next()
                            }
                        }
                    } else if (lua::type(L, -1) == lua::TSTRING) {
                        // e.g. "XLINGS_RES" — treat as url placeholder
                        res.url = lua::tostring(L, -1);
                    }
                    xpm.entries[platform][version] = std::move(res);
                }
                lua::pop(L, 1);  // pop value, keep key for next()
            }
        }
        lua::pop(L, 1);  // pop platform value, keep platform key for next()
    }

    lua::pop(L, 1);  // pop xpm table
    return xpm;
}

// Register a build sandbox: provides real filesystem operations needed by
// pkgindex-build.lua scripts (os.scriptdir, os.files, os.cd, os.execv,
// io.readfile, io.writefile, path.join, path.basename, cprint).
void register_build_sandbox(lua::State* L, const fs::path& script_dir) {
    // Set os.scriptdir to return the actual script directory
    lua::pushstring(L, script_dir.string().c_str());
    lua::setglobal(L, "__xpkg_scriptdir__");
    lua::L_dostring(L,
        "os.scriptdir = function() return __xpkg_scriptdir__ end\n"
    );

    // Real path module
    lua::L_dostring(L,
        "path = path or {}\n"
        "path.join = function(...)\n"
        "  local parts = {}\n"
        "  for i = 1, select('#', ...) do\n"
        "    local v = select(i, ...)\n"
        "    if v ~= nil then parts[#parts+1] = tostring(v) end\n"
        "  end\n"
        "  return table.concat(parts, '/')\n"
        "end\n"
        "path.filename = function(p) return type(p)=='string' and (p:match('[^/\\\\]+$') or p) or '' end\n"
        "path.directory = function(p) return type(p)=='string' and (p:match('(.*)[/\\\\]') or '.') or '.' end\n"
        "path.basename = function(p)\n"
        "  if type(p) ~= 'string' then return '' end\n"
        "  local name = p:match('[^/\\\\]+$') or p\n"
        "  return name:match('(.+)%.[^.]+$') or name\n"
        "end\n"
    );

    // Register C++ implementations of os.files / os.isdir on the os table
    lua::getglobal(L, "os");

    // os.files(pattern) -> table of file paths (recursive)
    // Implemented in C++ via std::filesystem — fully cross-platform.
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* pat = lua::tostring(L, 1);
        lua::newtable(L);
        if (!pat) return 1;

        std::string pattern(pat);
        // Extract base directory (everything before last '/' or '\')
        std::string dirStr = ".";
        auto slash = pattern.rfind('/');
        auto bslash = pattern.rfind('\\');
        if (slash == std::string::npos) slash = bslash;
        else if (bslash != std::string::npos && bslash > slash) slash = bslash;
        if (slash != std::string::npos) dirStr = pattern.substr(0, slash);

        // Extract extension filter (e.g. "*.lua" -> ".lua")
        std::string extFilter;
        auto dot = pattern.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            extFilter = pattern.substr(dot);  // includes the dot
        }

        std::error_code ec;
        fs::path base(dirStr);
        if (!fs::is_directory(base, ec)) return 1;

        int idx = 1;
        for (auto& entry : fs::recursive_directory_iterator(
                base, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            auto& p = entry.path();
            if (!extFilter.empty() && p.extension().string() != extFilter) continue;
            // Use generic_string() for consistent '/' separators across platforms
            lua::pushstring(L, p.generic_string().c_str());
            lua::rawseti(L, -2, idx++);
        }
        return 1;
    });
    lua::setfield(L, -2, "files");

    // os.isdir(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_directory(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "isdir");

    lua::pop(L, 1);  // pop os table

    // os.cd and os.execv are no-ops in the build sandbox.
    // Git reset (clean + checkout) is handled by C++ in run_pkgindex_build()
    // BEFORE the Lua script runs, so the script only needs to do file I/O.
    lua::L_dostring(L,
        "os.cd = function(dir) end\n"
        "os.execv = function(cmd, args) end\n"
    );

    // Real io.readfile / io.writefile
    lua::L_dostring(L,
        "io.readfile = function(filepath)\n"
        "  local f = io.open(filepath, 'r')\n"
        "  if not f then return '' end\n"
        "  local content = f:read('*a')\n"
        "  f:close()\n"
        "  return content\n"
        "end\n"
        "io.writefile = function(filepath, content)\n"
        "  local f = io.open(filepath, 'w')\n"
        "  if not f then return end\n"
        "  f:write(content)\n"
        "  f:close()\n"
        "end\n"
    );

    // cprint stub (just print)
    lua::L_dostring(L,
        "cprint = function(...)\n"
        "  local args = {...}\n"
        "  local fmt = args[1] or ''\n"
        "  -- strip color markers ${...}\n"
        "  fmt = fmt:gsub('%${.-}', '')\n"
        "  if #args > 1 then\n"
        "    print(string.format(fmt, table.unpack(args, 2)))\n"
        "  else\n"
        "    print(fmt)\n"
        "  end\n"
        "end\n"
    );

    // string.endswith
    lua::L_dostring(L,
        "function string:endswith(suffix)\n"
        "  return suffix == '' or self:sub(-#suffix) == suffix\n"
        "end\n"
    );
}

// Run pkgindex-build.lua's install() function to generate complete package files.
// Returns true if a build script was found and executed successfully.
// io.write / print that forward to a BuildOutput held as the closure's upvalue.
// The BuildOutput outlives the Lua state (run_pkgindex_build owns both).
int build_output_write_(lua::State* L) {
    auto* out = static_cast<const BuildOutput*>(
        lua::touserdata(L, lua::upvalueindex(1)));
    const int n = lua::gettop(L);
    for (int i = 1; i <= n; ++i) {
        unsigned long long len = 0;
        const char* text = lua::L_tolstring(L, i, &len);
        if (out && text) (*out)(std::string_view(text, static_cast<std::size_t>(len)));
        lua::pop(L, 1);
    }
    return 0;
}

int build_output_print_(lua::State* L) {
    auto* out = static_cast<const BuildOutput*>(
        lua::touserdata(L, lua::upvalueindex(1)));
    const int n = lua::gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        unsigned long long len = 0;
        const char* text = lua::L_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        if (text) line.append(text, static_cast<std::size_t>(len));
        lua::pop(L, 1);
    }
    line += '\n';
    if (out) (*out)(line);
    return 0;
}

bool run_pkgindex_build(const fs::path& repo_dir, const BuildOutput& output) {
    auto build_script = repo_dir / "pkgindex-build.lua";
    if (!fs::exists(build_script)) return false;

    // Reset pkgs dir via git to ensure clean state before the build script
    // modifies files. Use git -C to avoid changing working directory.
    if (fs::exists(repo_dir / ".git")) {
#if defined(_WIN32)
        auto cmd = "git -C \"" + repo_dir.string() + "\" checkout -- pkgs/ 2>nul";
#else
        auto cmd = "git -C \"" + repo_dir.string() + "\" checkout -- pkgs/ 2>/dev/null";
#endif
        std::system(cmd.c_str());
    }

    lua::State* L = lua::L_newstate();
    if (!L) return false;
    lua::L_openlibs(L);

    // Register build sandbox with real filesystem operations
    register_build_sandbox(L, repo_dir);

    if (output) {
        auto* sink = const_cast<BuildOutput*>(&output);
        lua::getglobal(L, "io");
        lua::pushlightuserdata(L, sink);
        lua::pushcclosure(L, build_output_write_, 1);
        lua::setfield(L, -2, "write");
        lua::pop(L, 1);
        lua::pushlightuserdata(L, sink);
        lua::pushcclosure(L, build_output_print_, 1);
        lua::setglobal(L, "print");
    }

    // Execute the build script
    if (lua::L_dofile(L, build_script.string().c_str()) != lua::OK) {
        std::string err = lua::tostring(L, -1);
        lua::close(L);
        return false;
    }

    // Call install() function
    lua::getglobal(L, "install");
    if (lua::type(L, -1) != lua::TFUNCTION) {
        lua::close(L);
        return false;
    }

    int rc = lua::pcall(L, 0, 1, 0);
    bool ok = (rc == lua::OK);
    lua::close(L);
    return ok;
}

} // namespace mcpplibs::xpkg::loader_detail

export namespace mcpplibs::xpkg {


std::expected<Package, std::string>
load_package(const fs::path& pkg_path, const LoaderContext& context) {
    if (!fs::exists(pkg_path))
        return std::unexpected("file not found: " + pkg_path.string());

    lua::State* L = lua::L_newstate();
    if (!L) return std::unexpected("failed to create lua state");
    lua::L_openlibs(L);

    // Register loader sandbox (pure Lua 5.4, no xmake dependency)
    loader_detail::register_loader_sandbox(
        L, context.platform, normalize_arch(context.arch));

    if (lua::L_dofile(L, pkg_path.string().c_str()) != lua::OK) {
        std::string err = lua::tostring(L, -1);
        lua::close(L);
        return std::unexpected("lua error: " + err);
    }

    lua::getglobal(L, "package");
    if (lua::type(L, -1) != lua::TTABLE) {
        lua::close(L);
        return std::unexpected("'package' global not found in " + pkg_path.string());
    }

    int pkg_idx = lua::gettop(L);
    Package p;
    p.spec        = loader_detail::get_str(L, pkg_idx, "spec");
    p.name        = loader_detail::get_str(L, pkg_idx, "name");
    p.description = loader_detail::get_str(L, pkg_idx, "description");
    p.type        = loader_detail::parse_type(
                        loader_detail::get_str(L, pkg_idx, "type"));
    p.status      = loader_detail::parse_status(
                        loader_detail::get_str(L, pkg_idx, "status"));
    p.namespace_  = loader_detail::get_str(L, pkg_idx, "namespace");
    p.homepage    = loader_detail::get_str(L, pkg_idx, "homepage");
    p.repo        = loader_detail::get_str(L, pkg_idx, "repo");
    p.docs        = loader_detail::get_str(L, pkg_idx, "docs");
    p.xvm_enable  = loader_detail::get_bool(L, pkg_idx, "xvm_enable");
    p.authors     = loader_detail::get_str_array(L, pkg_idx, "authors");
    p.maintainers = loader_detail::get_str_array(L, pkg_idx, "maintainers");
    p.licenses    = loader_detail::get_str_array(L, pkg_idx, "licenses");
    p.categories  = loader_detail::get_str_array(L, pkg_idx, "categories");
    p.keywords    = loader_detail::get_str_array(L, pkg_idx, "keywords");
    p.archs       = loader_detail::get_str_array(L, pkg_idx, "archs");
    p.programs    = loader_detail::get_str_array(L, pkg_idx, "programs");
    p.xpm         = loader_detail::parse_xpm(L, pkg_idx);

    lua::close(L);
    return p;
}

std::expected<Package, std::string>
load_package(const fs::path& pkg_path) {
    return load_package(pkg_path, {});
}

// External metadata execution never evaluates the recipe in the calling process.
// An empty/failing boundary is refused, never a request to use the local loader.
std::expected<Package, std::string>
load_package(const fs::path& pkg_path, const LoaderContext& context, MetadataBoundary boundary);

std::expected<PackageIndex, std::string>
build_index(const fs::path& repo_dir, const std::string& defaultNamespace,
            const BuildOutput& buildOutput) {
    return loader_detail::build_index_impl(repo_dir, defaultNamespace, buildOutput, {}, nullptr);
}

// Both executable paths (metadata and pkgindex-build.lua) cross the supplied
// boundaries. Unlike the legacy builder, a boundary failure fails the build.
std::expected<PackageIndex, std::string>
build_index(const fs::path& repo_dir, const std::string& defaultNamespace,
            const LoaderContext& context, const LoaderBoundaries& boundaries,
            const BuildOutput& buildOutput = {});

std::expected<PackageIndex, std::string>
build_index(const fs::path& repo_dir, const std::string& defaultNamespace = "") {
    return build_index(repo_dir, defaultNamespace, BuildOutput{});
}

std::expected<IndexRepos, std::string>
load_index_repos(const fs::path&) {
    return std::unexpected("load_index_repos: not yet implemented");
}

} // export namespace mcpplibs::xpkg
