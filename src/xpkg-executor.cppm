module;

export module mcpplibs.xpkg.executor;
import mcpplibs.xpkg;
import mcpplibs.xpkg.lua_stdlib;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;
namespace fs  = std::filesystem;

export namespace mcpplibs::xpkg {

// Slim per-dep export info pre-resolved by xlings: paths are already
// joined with the dep's install_dir, so the Lua side gets ready-to-use
// absolute paths. Only the fields elfpatch.lua actually needs are
// surfaced — additional `exports.*` capabilities (data, build) will be
// injected via separate _RUNTIME tables when those land.
struct DepExport {
    std::string loader;                       // absolute path or empty
    std::vector<std::string> libdirs;         // absolute paths (already joined)
    std::string abi;                          // e.g. "linux-x86_64-glibc"
};

struct ExecutionContext {
    std::string pkg_name, version, platform, arch;
    fs::path install_file, install_dir;
    fs::path run_dir, xpkg_dir, bin_dir;
    fs::path project_data_dir;  // project-local data root (empty when no project config)
    // `deps_list` retained as the legacy union (runtime ∪ build) for
    // backward compat with old install hooks; new code should consult
    // `runtime_deps_list` / `build_deps_list` to get the split.
    std::vector<std::string> deps_list, args;
    std::vector<std::string> runtime_deps_list;
    std::vector<std::string> build_deps_list;
    // Pre-resolved exports of each runtime dep. Key is the dep spec as
    // it appears in runtime_deps_list (e.g. "xim:glibc@2.39"). Only
    // deps that actually declare exports show up; missing entries mean
    // "this dep declared nothing — fall back to convention".
    std::unordered_map<std::string, DepExport> deps_exports;
    // The current package's own exports (rule 2 in the predicate trigger).
    DepExport self_exports;
    std::string subos_sysrootdir;
};

struct HookResult {
    bool success = false;
    std::string output, error;
    std::string version;  // non-empty when installed() returns a version string
};

struct XvmOp {
    std::string op;         // "add" | "remove" | "headers" | "remove_headers"
    std::string name;
    std::string version;
    std::string bindir;
    std::string alias;
    std::string type;       // "program" | "lib"
    std::string filename;
    std::string binding;
    std::string includedir; // for headers/remove_headers ops
    std::vector<std::pair<std::string, std::string>> envs; // environment variables
};

struct InstallRequest {
    std::string op;      // "install" | "remove"
    std::string target;  // e.g. "scode:linux-headers@5.11.1"
};

enum class HookType { Installed, Build, Install, Config, Uninstall };

} // export namespace mcpplibs::xpkg

// Implementation detail (not exported)
namespace mcpplibs::xpkg::detail {

constexpr std::string_view hook_name(HookType h) {
    switch (h) {
        case HookType::Installed:  return "installed";
        case HookType::Build:      return "build";
        case HookType::Install:    return "install";
        case HookType::Config:     return "config";
        case HookType::Uninstall:  return "uninstall";
    }
    return "";
}

// Set a string field on the table at top of stack
void set_string_field(lua::State* L, std::string_view key, std::string_view val) {
    lua::pushstring(L, std::string(val).c_str());
    lua::setfield(L, -2, std::string(key).c_str());
}

// Register C++ std::filesystem implementations of os.isdir and os.dirs.
// Called after the Lua prelude to override the shell-based versions.
void register_os_funcs(lua::State* L) {
    lua::getglobal(L, "os");
    if (lua::type(L, -1) != lua::TTABLE) {
        lua::pop(L, 1);
        lua::newtable(L);
        lua::setglobal(L, "os");
        lua::getglobal(L, "os");
    }

    // os.isdir(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_directory(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "isdir");

    // os.dirs(pattern) -> table of absolute dir paths
    // Accepts "base/*" style pattern; strips trailing /* to get the base directory.
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* pat = lua::tostring(L, 1);
        lua::newtable(L);
        if (!pat) return 1;
        std::string s(pat);
        // Strip trailing /* or \*
        if (s.size() >= 2 && s.back() == '*' &&
            (s[s.size()-2] == '/' || s[s.size()-2] == '\\')) {
            s.pop_back(); s.pop_back();
        }
        while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
            s.pop_back();
        fs::path base(s);
        std::error_code ec;
        if (!fs::is_directory(base, ec)) return 1;
        int idx = 1;
        for (auto& entry : fs::directory_iterator(base, ec)) {
            if (entry.is_directory(ec)) {
                lua::pushstring(L, entry.path().string().c_str());
                lua::rawseti(L, -2, idx++);
            }
        }
        return 1;
    });
    lua::setfield(L, -2, "dirs");

    // os.cd(dir) -> bool
    // Real chdir so that subsequent os.execute / system.exec inherit the new CWD.
    // Safe: hook calls are wrapped in ScopedCurrentDir_ which restores CWD on return.
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::current_path(fs::path(p), ec);
        lua::pushboolean(L, ec ? 0 : 1);
        return 1;
    });
    lua::setfield(L, -2, "cd");

    // os.cp(src, dst) -> bool
    // Mimics `cp -a`: when src is a dir and dst is an existing dir,
    // copies src as a subdirectory of dst (i.e. dst/src_name/...).
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* src = lua::tostring(L, 1);
        const char* dst = lua::tostring(L, 2);
        if (!src || !dst) { lua::pushboolean(L, 0); return 1; }
        fs::path sp(src), dp(dst);
        std::error_code ec;
        // cp -a semantics: dir into existing dir → dst/basename(src)/...
        if (fs::is_directory(sp, ec) && fs::is_directory(dp, ec))
            dp /= sp.filename();
        fs::copy(sp, dp,
                 fs::copy_options::recursive |
                 fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing, ec);
        lua::pushboolean(L, ec ? 0 : 1);
        return 1;
    });
    lua::setfield(L, -2, "cp");

    // os.trymv(src, dst) -> bool  (rename, fallback to copy+remove)
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* s = lua::tostring(L, 1);
        const char* d = lua::tostring(L, 2);
        if (!s || !d) { lua::pushboolean(L, 0); return 1; }
        fs::path src(s), dst(d);
        std::error_code ec;
        // If dst is existing dir, move into it (unix mv semantics)
        if (fs::is_directory(dst, ec)) dst /= src.filename();
        fs::rename(src, dst, ec);
        if (!ec) { lua::pushboolean(L, 1); return 1; }
        // Cross-device: copy + remove
        ec.clear();
        fs::copy(src, dst,
                 fs::copy_options::recursive |
                 fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing, ec);
        if (ec) { lua::pushboolean(L, 0); return 1; }
        fs::remove_all(src, ec);
        lua::pushboolean(L, 1);
        return 1;
    });
    lua::setfield(L, -2, "trymv");
    // Also set os.mv = os.trymv
    lua::getfield(L, -1, "trymv");
    lua::setfield(L, -2, "mv");

    // os.tryrm(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::remove_all(fs::path(p), ec);
        lua::pushboolean(L, 1);
        return 1;
    });
    lua::setfield(L, -2, "tryrm");

    // os.mkdir(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::create_directories(fs::path(p), ec);
        lua::pushboolean(L, ec ? 0 : 1);
        return 1;
    });
    lua::setfield(L, -2, "mkdir");

    // os.isfile(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_regular_file(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "isfile");

    // os.iorun(cmd) -> string  (capture stdout, discard stderr)
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* cmd = lua::tostring(L, 1);
        if (!cmd) { lua::pushstring(L, ""); return 1; }
        // Build a temp path for capturing output
        std::error_code ec;
        auto tmp = fs::temp_directory_path(ec) / ("xpkg_iorun_" +
            std::to_string(std::hash<std::string>{}(std::string(cmd) +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))));
        std::string full(cmd);
#ifdef _WIN32
        full += " 2>nul > \"" + tmp.string() + "\"";
#else
        full += " 2>/dev/null > \"" + tmp.string() + "\"";
#endif
        std::system(full.c_str());
        std::ifstream ifs(tmp);
        std::string out;
        if (ifs.good()) {
            std::ostringstream ss;
            ss << ifs.rdbuf();
            out = ss.str();
        }
        ifs.close();
        fs::remove(tmp, ec);
        lua::pushstring(L, out.c_str());
        return 1;
    });
    lua::setfield(L, -2, "iorun");

    lua::pop(L, 1); // pop os table
}

// Load all xim.libxpkg.* modules into _LIBXPKG_MODULES table, then run prelude
bool load_stdlib(lua::State* L, std::string& err_out) {
    // Create empty _LIBXPKG_MODULES table
    lua::newtable(L);
    lua::setglobal(L, "_LIBXPKG_MODULES");

    // Each module script returns a table; store it into _LIBXPKG_MODULES[name]
    struct ModEntry { const char* name; std::string_view src; };
    const ModEntry mods[] = {
        { "log",        detail::log_lua        },
        { "pkginfo",    detail::pkginfo_lua    },
        { "system",     detail::system_lua     },
        { "xvm",        detail::xvm_lua        },
        { "utils",      detail::utils_lua      },
        { "pkgmanager", detail::pkgmanager_lua },
        { "elfpatch",   detail::elfpatch_lua   },
        { "json",       detail::json_lua       },
        { "base64",     detail::base64_lua     },
    };

    for (auto& m : mods) {
        // Load and compile the module source
        if (lua::L_loadstring(L, m.src.data()) != lua::OK) {
            err_out = std::string("failed to compile module ") + m.name + ": "
                    + lua::tostring(L, -1);
            lua::pop(L, 1);
            return false;
        }
        // Execute the chunk, requesting 1 return value (the module table)
        if (lua::pcall(L, 0, 1, 0) != lua::OK) {
            err_out = std::string("failed to run module ") + m.name + ": "
                    + lua::tostring(L, -1);
            lua::pop(L, 1);
            return false;
        }
        // Stack: [module_table]
        // Store into _LIBXPKG_MODULES[name]
        lua::getglobal(L, "_LIBXPKG_MODULES");  // stack: [module_table, modules]
        lua::insert(L, -2);                      // stack: [modules, module_table]
        lua::setfield(L, -2, m.name);            // modules[name] = module_table; stack: [modules]
        lua::pop(L, 1);                          // stack: []
    }

    // Load prelude: defines import(), os.*, path.*, etc.
    if (lua::L_loadstring(L, detail::prelude_lua.data()) != lua::OK) {
        err_out = "failed to load prelude: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }
    if (lua::pcall(L, 0, 0, 0) != lua::OK) {
        err_out = "failed to run prelude: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }

    // Override shell-based os.* with C++ std::filesystem implementations
    register_os_funcs(L);

    return true;
}

// Inject ExecutionContext into Lua as _RUNTIME global table
void inject_context(lua::State* L, const mcpplibs::xpkg::ExecutionContext& ctx) {
    lua::newtable(L);

    set_string_field(L, "pkg_name",        ctx.pkg_name);
    set_string_field(L, "version",          ctx.version);
    set_string_field(L, "platform",         ctx.platform);
    set_string_field(L, "arch",             ctx.arch);
    set_string_field(L, "install_file",     ctx.install_file.string());
    set_string_field(L, "install_dir",      ctx.install_dir.string());
    set_string_field(L, "run_dir",          ctx.run_dir.string());
    set_string_field(L, "xpkg_dir",          ctx.xpkg_dir.string());
    set_string_field(L, "bin_dir",           ctx.bin_dir.string());
    set_string_field(L, "project_data_dir",  ctx.project_data_dir.string());
    set_string_field(L, "subos_sysrootdir",  ctx.subos_sysrootdir);

    auto push_string_array = [&](const std::vector<std::string>& v, const char* field) {
        lua::newtable(L);
        for (int i = 0; i < (int)v.size(); ++i) {
            lua::pushstring(L, v[i].c_str());
            lua::rawseti(L, -2, i + 1);
        }
        lua::setfield(L, -2, field);
    };
    push_string_array(ctx.deps_list,         "deps_list");
    push_string_array(ctx.runtime_deps_list, "runtime_deps_list");
    push_string_array(ctx.build_deps_list,   "build_deps_list");

    // deps_exports: { [dep_spec] = { loader, libdirs, abi }, ... }
    // Only deps that declared exports show up here.
    lua::newtable(L);
    for (auto& [dep_spec, e] : ctx.deps_exports) {
        lua::newtable(L);
        set_string_field(L, "loader", e.loader);
        set_string_field(L, "abi",    e.abi);
        push_string_array(e.libdirs, "libdirs");
        lua::setfield(L, -2, dep_spec.c_str());
    }
    lua::setfield(L, -2, "deps_exports");

    // self_exports: same shape as a single deps_exports entry. Empty
    // strings/arrays when the current package didn't declare exports.
    lua::newtable(L);
    set_string_field(L, "loader", ctx.self_exports.loader);
    set_string_field(L, "abi",    ctx.self_exports.abi);
    push_string_array(ctx.self_exports.libdirs, "libdirs");
    lua::setfield(L, -2, "self_exports");

    // args as array table
    lua::newtable(L);
    for (int i = 0; i < (int)ctx.args.size(); ++i) {
        lua::pushstring(L, ctx.args[i].c_str());
        lua::rawseti(L, -2, i + 1);
    }
    lua::setfield(L, -2, "args");

    lua::setglobal(L, "_RUNTIME");
}

} // namespace mcpplibs::xpkg::detail

// ---- PackageExecutor ----

export namespace mcpplibs::xpkg {

class PackageExecutor {
    lua::State* L_   = nullptr;
    fs::path    pkg_ ;

public:
    explicit PackageExecutor(lua::State* L, fs::path pkg)
        : L_(L), pkg_(std::move(pkg)) {}

    ~PackageExecutor() {
        if (L_) { lua::close(L_); L_ = nullptr; }
    }

    PackageExecutor(const PackageExecutor&)            = delete;
    PackageExecutor& operator=(const PackageExecutor&) = delete;

    PackageExecutor(PackageExecutor&& o) noexcept
        : L_(std::exchange(o.L_, nullptr)), pkg_(std::move(o.pkg_)) {}

    PackageExecutor& operator=(PackageExecutor&& o) noexcept {
        if (this != &o) {
            if (L_) lua::close(L_);
            L_   = std::exchange(o.L_, nullptr);
            pkg_ = std::move(o.pkg_);
        }
        return *this;
    }

    bool has_hook(HookType hook) const {
        auto name = detail::hook_name(hook);
        lua::getglobal(L_, std::string(name).c_str());
        bool found = (lua::type(L_, -1) == lua::TFUNCTION);
        lua::pop(L_, 1);
        return found;
    }

    HookResult run_hook(HookType hook, const ExecutionContext& ctx) {
        // Inject context before each hook call
        detail::inject_context(L_, ctx);

        auto name = detail::hook_name(hook);
        lua::getglobal(L_, std::string(name).c_str());

        if (lua::type(L_, -1) != lua::TFUNCTION) {
            lua::pop(L_, 1);
            return HookResult{ .success = false,
                               .error   = "hook not found: " + std::string(name) };
        }

        HookResult result;
        if (lua::pcall(L_, 0, 1, 0) == lua::OK) {
            int t = lua::type(L_, -1);
            if (t == lua::TBOOLEAN) {
                result.success = lua::toboolean(L_, -1);
            } else if (t == lua::TSTRING) {
                result.version = lua::tostring(L_, -1);
                result.success = !result.version.empty();
            } else {
                // nil or anything else: treat as success (hook ran without error)
                result.success = true;
            }
            lua::pop(L_, 1);
        } else {
            result.success = false;
            result.error   = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        }
        return result;
    }

    HookResult check_installed(const ExecutionContext& ctx) {
        return run_hook(HookType::Installed, ctx);
    }

    // Run elfpatch.apply_auto() if the install hook set elfpatch_auto flag.
    // Returns {scanned, patched, failed} counts. Safe to call unconditionally.
    HookResult apply_elfpatch_auto() {
        constexpr const char* script = R"__LUA__(
            local ep = _LIBXPKG_MODULES and _LIBXPKG_MODULES["elfpatch"]
            if not ep then return "no-ep 0 0 0" end
            local r = ep.apply_auto()
            return tostring(r.scanned) .. " " .. tostring(r.patched) .. " " .. tostring(r.failed)
        )__LUA__";
        HookResult result;
        if (lua::L_loadstring(L_, script) != lua::OK) {
            result.success = false;
            result.error = lua::tostring(L_, -1);
            lua::pop(L_, 1);
            return result;
        }
        if (lua::pcall(L_, 0, 1, 0) == lua::OK) {
            result.success = true;
            if (lua::type(L_, -1) == lua::TSTRING)
                result.output = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        } else {
            result.success = false;
            result.error = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        }
        return result;
    }

    std::vector<XvmOp> xvm_operations() {
        std::vector<XvmOp> ops;
        lua::getglobal(L_, "_XVM_OPS");
        if (lua::type(L_, -1) != lua::TTABLE) {
            lua::pop(L_, 1);
            return ops;
        }
        int len = (int)lua::rawlen(L_, -1);
        for (int i = 1; i <= len; ++i) {
            lua::rawgeti(L_, -1, i);
            if (lua::type(L_, -1) == lua::TTABLE) {
                XvmOp op;
                auto read_field = [&](const char* key) -> std::string {
                    lua::getfield(L_, -1, key);
                    std::string val;
                    if (lua::type(L_, -1) == lua::TSTRING)
                        val = lua::tostring(L_, -1);
                    lua::pop(L_, 1);
                    return val;
                };
                op.op         = read_field("op");
                op.name       = read_field("name");
                op.version    = read_field("version");
                op.bindir     = read_field("bindir");
                op.alias      = read_field("alias");
                op.type       = read_field("type");
                op.filename   = read_field("filename");
                op.binding    = read_field("binding");
                op.includedir = read_field("includedir");

                // Read envs table (key-value pairs)
                lua::getfield(L_, -1, "envs");
                if (lua::type(L_, -1) == lua::TTABLE) {
                    lua::pushnil(L_);
                    while (lua::next(L_, -2)) {
                        if (lua::type(L_, -2) == lua::TSTRING &&
                            lua::type(L_, -1) == lua::TSTRING) {
                            op.envs.emplace_back(
                                lua::tostring(L_, -2),
                                lua::tostring(L_, -1));
                        }
                        lua::pop(L_, 1);
                    }
                }
                lua::pop(L_, 1);

                ops.push_back(std::move(op));
            }
            lua::pop(L_, 1);
        }
        lua::pop(L_, 1);
        return ops;
    }

    // Run the script's xpkg_main() function with arguments.
    HookResult run_script(const ExecutionContext& ctx) {
        detail::inject_context(L_, ctx);

        lua::newtable(L_);
        lua::setglobal(L_, "_XVM_OPS");
        lua::newtable(L_);
        lua::setglobal(L_, "_INSTALL_REQUESTS");

        lua::getglobal(L_, "xpkg_main");
        if (lua::type(L_, -1) != lua::TFUNCTION) {
            lua::pop(L_, 1);
            return HookResult{ .success = false,
                               .error = "xpkg_main not found in script" };
        }

        int nargs = static_cast<int>(ctx.args.size());
        for (auto& arg : ctx.args) {
            lua::pushstring(L_, arg.c_str());
        }

        HookResult result;
        if (lua::pcall(L_, nargs, 0, 0) == lua::OK) {
            result.success = true;
        } else {
            result.success = false;
            result.error = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        }
        return result;
    }

    // Set log level for Lua scripts: "debug", "info", "warn", "error", "silent"
    void set_log_level(std::string_view level) {
        std::string script = std::string("local log = _LIBXPKG_MODULES and _LIBXPKG_MODULES['log']; ")
            + "if log and log.set_level then log.set_level('" + std::string(level) + "') end";
        if (lua::L_loadstring(L_, script.c_str()) == lua::OK) {
            lua::pcall(L_, 0, 0, 0);
        } else {
            lua::pop(L_, 1);
        }
    }

    std::vector<InstallRequest> install_requests() {
        std::vector<InstallRequest> reqs;
        lua::getglobal(L_, "_INSTALL_REQUESTS");
        if (lua::type(L_, -1) != lua::TTABLE) {
            lua::pop(L_, 1);
            return reqs;
        }
        int len = (int)lua::rawlen(L_, -1);
        for (int i = 1; i <= len; ++i) {
            lua::rawgeti(L_, -1, i);
            if (lua::type(L_, -1) == lua::TTABLE) {
                InstallRequest req;
                lua::getfield(L_, -1, "op");
                if (lua::type(L_, -1) == lua::TSTRING) req.op = lua::tostring(L_, -1);
                lua::pop(L_, 1);
                lua::getfield(L_, -1, "target");
                if (lua::type(L_, -1) == lua::TSTRING) req.target = lua::tostring(L_, -1);
                lua::pop(L_, 1);
                reqs.push_back(std::move(req));
            }
            lua::pop(L_, 1);
        }
        lua::pop(L_, 1);
        return reqs;
    }
};

// Factory
std::expected<PackageExecutor, std::string>
create_executor(const fs::path& pkg_path) {
    if (!fs::exists(pkg_path)) {
        return std::unexpected("package file not found: " + pkg_path.string());
    }

    lua::State* L = lua::L_newstate();
    if (!L) return std::unexpected("failed to create lua state");

    lua::L_openlibs(L);

    std::string err;
    if (!detail::load_stdlib(L, err)) {
        lua::close(L);
        return std::unexpected(err);
    }

    if (lua::L_dofile(L, pkg_path.string().c_str()) != lua::OK) {
        err = lua::tostring(L, -1);
        lua::close(L);
        return std::unexpected("failed to load package: " + err);
    }

    return PackageExecutor(L, pkg_path);
}

} // export namespace mcpplibs::xpkg
