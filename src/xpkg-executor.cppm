module;

// Platform headers for the hook log's file layer below. `import std;` does not
// provide them, and the named-module purview forbids including them there.
#if defined(_WIN32)
// windows.h's min/max macros break std::min({...}) in every module that sees
// them, and only Windows ever reports it. Both defines come before the include.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

export module mcpplibs.xpkg.executor;
import mcpplibs.xpkg;
import mcpplibs.xpkg.lua_stdlib;
import mcpplibs.xpkg.shell;
import mcpplibs.xpkg.fs;
import mcpplibs.xpkg.loader;
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

// One runtime dependency, as the RESOLVER settled it — not as the recipe
// spelled it.
//
// This is the record that makes "which version is this dependency" have one
// answer. `DepExport` below cannot serve: it carries only what a dep
// explicitly declared, so a dep that declared nothing is absent from it, and
// absence was defined to mean "fall back to convention" — which is a second
// answerer wearing a different hat. Every runtime dep appears here, declared
// or not.
struct ResolvedDep {
    std::string spec;         // the recipe's own text, e.g. "xim:glibc@>=2.38"
    std::string name;         // canonical, e.g. "xim:glibc"
    std::string version;      // what it resolved TO, e.g. "2.44"
    std::string install_dir;  // absolute payload directory — the authority
    std::vector<std::string> libdirs;  // absolute; convention-filled when the
                                       // dep declared none, so no consumer
                                       // has to re-derive it
    std::string source;       // why this one: "plan" | "pinned-active" | ...
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
    // Keyed by the same spec string as deps_exports. Authoritative for runtime
    // deps resolved in this plan, but not total across separate host dependency
    // domains; those stores are supplied explicitly below.
    std::unordered_map<std::string, ResolvedDep> resolved_deps;
    // Ordered host store roots for dependencies outside this resolver plan.
    // Presence in _RUNTIME marks a modern context even when the vector is empty.
    std::vector<fs::path> dependency_store_roots;
    // The current package's own exports (rule 2 in the predicate trigger).
    DepExport self_exports;
    std::string subos_sysrootdir;
    std::string pkgindex_dir;    // package index repo root (for custom module loading)

    // Where run_hook() sends what a hook's child processes write (0.0.60).
    //
    // Empty (the default) is the behaviour of every release before it: children
    // started by os.execute / system.exec inherit this process's fd 1 and 2,
    // and only the hook's own print / io.write / io.stderr:write are captured
    // into HookResult::output. A caller that owns the terminal or a protocol on
    // stdout has no way to keep a child's output out of it, which is what this
    // field is for.
    //
    // Non-empty: run_hook() truncates the file (creating its directory), writes
    // the line "# <hook> hook of <pkg_name>@<version>", and for the length of
    // the hook
    //   * appends the hook's own print / io.write / io.stderr:write to it, and
    //   * starts every os.execute child with fd 1 and 2 pointing at it,
    // so the file reads in the order things happened. HookResult::output is then
    // the last kMaxHookOutputBytes of that file. system.exec(cmd, { tty = true })
    // and system.run_in_script(script, true) still inherit the terminal.
    //
    // A file that cannot be created or opened never fails the hook: children
    // get the null device, one warning goes through the log module (and so into
    // HookResult::output), and the ring buffer stands in for the file.
    // run_script() and apply_elfpatch_auto() ignore this field.
    fs::path hook_log;
};

inline constexpr std::size_t kMaxHookOutputBytes = 16 * 1024;

// One thing a recipe said to the person installing it: a log.warn or a
// log.error of the hook, its text as formatted, without the output's prefix.
struct HookNotice {
    std::string level;  // "warn" | "error"
    std::string text;
};
// A hook keeps at most this many, each at most this long; what is over is
// counted in one last notice, and the hook's output still has all of it.
inline constexpr std::size_t kMaxHookNotices = 100;
inline constexpr std::size_t kMaxHookNoticeBytes = 2048;

// What the automatic ELF/Mach-O patch did (apply_elfpatch_auto), as facts and
// an outcome, for the host to judge and show (design 2026-11-27 §9.3).
struct ElfpatchResult {
    std::string outcome;   // "done" | "skipped" | "failed"
    std::string reason;    // why it was skipped or failed; empty when done
    int scanned = 0, patched = 0, failed = 0;
    // Files left alone, by why: built for another machine than the loader
    // being written, nothing to resolve (static, a loader), named in `skip`.
    int skipped_foreign = 0, skipped_static = 0, skipped_listed = 0;
    std::string tool_missing;  // the tool files needed and that was not found
};

// A call of the compatibility layer that did not do what it names (os.mv,
// os.cp, os.mkdir, os.exec): it returned false or a failure status as it
// always did, and is reported here (design 2026-11-27 §9.6).
struct HookCallFailure {
    std::string call;  // "os.mv"
    std::string what;  // its arguments, as text: "<src> -> <dst>"
    std::string why;   // the system's reason, or how the command ended
};

struct HookResult {
    bool success = false;
    std::string output, error;
    std::string version;  // non-empty when installed() returns a version string
    // What the recipe said to the user during this hook, in order (design
    // 2026-11-27 §9.2). The output stream is the log; this is the channel.
    std::vector<HookNotice> notices;
    // Set by apply_elfpatch_auto only. `output` keeps its "<scanned> <patched>
    // <failed>" text for hosts that read it.
    std::optional<ElfpatchResult> elfpatch;
    // The compatibility layer's calls that failed during this hook, in order
    // (at most 100).
    std::vector<HookCallFailure> call_failures;
};

struct XvmOp {
    std::string op;         // "add" | "remove" | "headers" | "remove_headers"
                            // | "subos_env"
    std::string name;
    std::string version;
    std::string bindir;
    std::string alias;
    std::string type;       // "program" | "lib" | "files"
    std::string filename;
    std::string binding;
    std::string includedir; // for headers/remove_headers ops

    // type = "files": one asset the package places into the subos.
    //
    // Both ends are relative, and that is a requirement rather than a
    // convention. A payload is shared between subos and reference-counted,
    // so an absolute destination recorded against it would be wrong for
    // every subos but the one that installed it. `src` is relative to the
    // payload root, `dst` to the subos root; the consumer resolves them and
    // rejects anything absolute or escaping.
    //
    // Exists because `includedir` can only say "this one directory becomes
    // sysroot include". It cannot express a destination, an asset that is
    // not a header, or a source and destination that differ in name --
    // openssl's `lib64/` -> `usr/lib/` is all three at once. Without a way
    // to say it, package indexes grow their own file-placing helpers, and
    // the tool managing versions cannot see or undo any of them.
    std::string src;
    std::string dst;

    // Arguments injected ahead of the user's own when a program shim
    // dispatches. Separate from `alias` on purpose: the only way to inject
    // anything today is to append it to the alias string, which consumers
    // then split on the first space. That breaks on any path containing one,
    // and it makes every reader of `alias` -- version listings, diagnostics
    // -- report a command line where a name belongs.
    std::vector<std::string> args;

    // op = "subos_env": one environment variable the subos exports to every
    // process that enters it.
    //
    // Distinct from `envs` above, which is the environment a single program
    // shim sets for itself when dispatched. That scope cannot serve a
    // discovery protocol: the program that has to see LIBGL_DRIVERS_PATH is
    // the user's own binary, which xlings never wraps. Same reason `src`/`dst`
    // exist rather than another meaning piled onto `includedir` -- a new scope
    // needs its own fields, or every reader has to know which op it is looking
    // at before it can trust a value.
    //
    // `mode` carries the recipe's `op = "set"|"prepend"` argument, because
    // `op` itself is already the category the consumer dispatches on.
    std::string var;
    std::string value;
    std::string mode;

    std::vector<std::pair<std::string, std::string>> envs; // environment variables
};

// What a host lends the hooks it runs, as functions (design 2026-11-27 §9.7).
// libxpkg carries no implementation of them; a call to a port the host did
// not give is an error of that call, named.
struct HostPorts {
    // Unpack `archive` into the directory `dir` (created when missing),
    // dropping the first `strip` components of every member's path; a member
    // with fewer components is an error, never dropped. An error message, or
    // nothing when done.
    std::function<std::optional<std::string>(const fs::path& archive, const fs::path& dir, int strip)> extract;
};

struct InstallRequest {
    std::string op;      // "install" | "remove" | "admin" (system.admin_step)
    std::string target;  // e.g. "scode:linux-headers@5.11.1"; for "admin": why, then
                         // each word of the command, joined by '\x1f'
};

enum class HookType { Installed, Build, Install, Config, Uninstall };

// 2: HookResult carries notices, the elfpatch result and call failures.
inline constexpr int kHookBoundaryProtocol = 2;
enum class HookAction { Load, RunHook, RunScript, Elfpatch, SetLogLevel };
std::string_view hook_action_name(HookAction action);
std::optional<HookAction> hook_action_from_string(std::string_view name);

// Plain data for an external worker protocol. The consumer supplies transport,
// process isolation and validation of returned effects; this library supplies
// neither a sandbox nor an alternate Lua executor.
struct HookInvocation {
    HookAction action { HookAction::Load };
    fs::path package;
    HookType hook { HookType::Installed };
    ExecutionContext context;
    std::string log_level;
};

struct HookResponse {
    HookResult result;
    std::array<bool, 5> hooks{};  // indexed by HookType, only Load publishes them
    // Cumulative snapshots, exactly as the local executor's accessors return.
    std::vector<XvmOp> xvm_ops;
    std::vector<InstallRequest> install_requests;
};

using ExecutionBoundary = std::function<std::expected<HookResponse, std::string>(const HookInvocation&)>;


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


// _LIBXPKG_ARCHS: the architecture identity table (arch_identities()) for the
// Lua side, keyed by every canonical name and alias: { machine = e_machine,
// bits = 32|64, name = canonical }.
void inject_arch_identities(lua::State* L) {
    lua::newtable(L);
    for (const auto& arch : arch_identities()) {
        auto push_identity = [&] {
            lua::newtable(L);
            lua::pushinteger(L, arch.elf_machine);
            lua::setfield(L, -2, "machine");
            lua::pushinteger(L, arch.elf_bits);
            lua::setfield(L, -2, "bits");
            lua::pushstring(L, std::string(arch.name).c_str());
            lua::setfield(L, -2, "name");
        };
        push_identity();
        lua::setfield(L, -2, std::string(arch.name).c_str());
        for (const auto alias : arch.aliases) {
            push_identity();
            lua::setfield(L, -2, std::string(alias).c_str());
        }
    }
    lua::setglobal(L, "_LIBXPKG_ARCHS");
}

// Load all xim.libxpkg.* modules into _LIBXPKG_MODULES, then the compatibility layer
bool load_stdlib(lua::State* L, std::string& err_out) {
    // Create empty _LIBXPKG_MODULES table
    lua::newtable(L);
    lua::setglobal(L, "_LIBXPKG_MODULES");
    inject_arch_identities(L);

    // Each module script returns a table; store it into _LIBXPKG_MODULES[name]
    struct ModEntry { const char* name; std::string_view src; };
    const ModEntry mods[] = {
        { "log",        detail::log_lua        },
        { "pkginfo",    detail::pkginfo_lua    },
        { "system",     detail::system_lua     },
        { "subos",      detail::subos_lua      },
        { "xvm",        detail::xvm_lua        },
        { "utils",      detail::utils_lua      },
        { "pkgmanager", detail::pkgmanager_lua },
        { "elfpatch",   detail::elfpatch_lua   },
        { "json",       detail::json_lua       },
        { "base64",     detail::base64_lua     },
        { "path",       detail::path_lua       },
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

    // The compatibility layer: import(), the os/io/global names of spec 1 and 2.
    if (lua::L_loadstring(L, detail::compat_lua.data()) != lua::OK) {
        err_out = "failed to load the compatibility layer: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }
    if (lua::pcall(L, 0, 0, 0) != lua::OK) {
        err_out = "failed to run the compatibility layer: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }

    // Override shell-based os.* with C++ std::filesystem implementations
    register_os_funcs(L);

    // Register C++-backed fs module (symlink, entries, etc.)
    register_fs_module(L);

    // The interface manifest, and the environment of spec "3" built from it.
    if (lua::L_loadstring(L, detail::manifest_lua.data()) != lua::OK
            || lua::pcall(L, 0, 1, 0) != lua::OK) {
        err_out = "failed to load the interface manifest: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }
    lua::setglobal(L, "_LIBXPKG_MANIFEST");
    if (lua::L_loadstring(L, detail::spec3_lua.data()) != lua::OK
            || lua::pcall(L, 0, 0, 0) != lua::OK) {
        err_out = "failed to load the spec 3 environment: " + std::string(lua::tostring(L, -1));
        lua::pop(L, 1);
        return false;
    }

    return true;
}

// While the top level of a recipe of spec "3" runs: a call of a module
// function that is not pure is an error (spec3.lua's
// _LIBXPKG_TOPLEVEL_FORBIDDEN). The top level runs when the package table is
// read too, where nothing is installed; it holds the package table, imports and
// function definitions.
void toplevel_call_hook(lua::State* L, lua::Debug* ar) {
    if (ar->event != lua::HOOKCALL && ar->event != lua::HOOKTAILCALL) return;
    if (!lua::getinfo(L, "f", ar)) return;
    lua::getglobal(L, "_LIBXPKG_TOPLEVEL_FORBIDDEN");
    if (lua::type(L, -1) != lua::TTABLE) {
        lua::pop(L, 2);
        return;
    }
    lua::pushvalue(L, -2);
    lua::rawget(L, -2);
    if (lua::type(L, -1) != lua::TSTRING) {
        lua::pop(L, 3);
        return;
    }
    lua::L_where(L, 1);
    lua::pushvalue(L, -2);
    lua::pushstring(L, " is called at the top level of a recipe of spec \"3\"; "
                       "the top level holds the package table, imports and function definitions -- call it from a hook");
    lua::concat(L, 3);
    lua::error(L);
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
    set_string_field(L, "pkgindex_dir",      ctx.pkgindex_dir);

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
    std::vector<std::string> dependencyStoreRoots;
    dependencyStoreRoots.reserve(ctx.dependency_store_roots.size());
    for (const auto& root : ctx.dependency_store_roots) {
        dependencyStoreRoots.push_back(root.string());
    }
    push_string_array(dependencyStoreRoots, "dependency_store_roots");

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

    // resolved_deps: { [spec] = { name, version, install_dir, libdirs, source } }
    // Authoritative for dependencies represented by this resolver plan, unlike
    // deps_exports. Other host dependency domains are represented by the
    // explicit dependency_store_roots above.
    lua::newtable(L);
    for (auto& [dep_spec, r] : ctx.resolved_deps) {
        lua::newtable(L);
        set_string_field(L, "name",        r.name);
        set_string_field(L, "version",     r.version);
        set_string_field(L, "install_dir", r.install_dir);
        set_string_field(L, "source",      r.source);
        push_string_array(r.libdirs, "libdirs");
        lua::setfield(L, -2, dep_spec.c_str());
    }
    lua::setfield(L, -2, "resolved_deps");

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

constexpr std::string_view HOOK_OUTPUT_TRUNCATED_MARKER =
    "\n[libxpkg: hook output truncated]\n";

// ---- Native file layer: the hook log a child's fd 1/2 are pointed at ----
//
// The child itself is started by shell::run (xpkg-shell.cppm), the one place a
// command line reaches the shell; this block only opens and writes the file.

using NativeFile = shell::NativeFile;
inline const NativeFile kNoFile = shell::kNoFile;

// An append-only handle to `path`, which must already exist. On Windows it is
// inheritable: it becomes the child's stdout and stderr.
NativeFile open_append(const fs::path& path) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    // FILE_APPEND_DATA alone: every write lands at the end of the file, whoever
    // holds the handle, so the parent's and the child's writes stay in order.
    return ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    return ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
#endif
}

void close_file(NativeFile file) {
    if (file == kNoFile) return;
#if defined(_WIN32)
    ::CloseHandle(static_cast<HANDLE>(file));
#else
    ::close(file);
#endif
}

bool write_all(NativeFile file, std::string_view bytes) {
    while (!bytes.empty()) {
#if defined(_WIN32)
        const std::size_t chunk = bytes.size() < (1u << 20) ? bytes.size() : (1u << 20);
        DWORD written = 0;
        if (!::WriteFile(static_cast<HANDLE>(file), bytes.data(), static_cast<DWORD>(chunk),
                         &written, nullptr) ||
            written == 0) {
            return false;
        }
#else
        const ssize_t written = ::write(file, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

std::string printable(const fs::path& path) {
    // path::string() can throw on Windows for a name the ACP cannot spell, and
    // this is only ever used to word a warning.
    try {
        return path.string();
    } catch (...) {
        return "<unprintable path>";
    }
}

// The file one run_hook() writes when ExecutionContext::hook_log is set.
class HookLog {
    fs::path path_;
    NativeFile file_ = kNoFile;
    bool active_ = false;    // a hook is running with a log requested
    bool readable_ = false;  // the file exists and no write to it has failed

public:
    HookLog() = default;
    ~HookLog() { end(); }
    HookLog(const HookLog&) = delete;
    HookLog& operator=(const HookLog&) = delete;

    // Start a log. `active()` is true afterwards even when the file could not
    // be made: the caller asked for the children's output to stay off the
    // terminal, and the null device does that. Returns why the file is
    // unavailable, or an empty string.
    std::string begin(const fs::path& path, std::string_view header) {
        end();
        // Absolute now: a hook may os.cd() away, and the file is read back by
        // this name after it returns.
        std::error_code ec;
        path_ = fs::absolute(path, ec);
        if (ec) path_ = path;
        active_ = true;
        readable_ = false;

        if (path_.has_parent_path()) fs::create_directories(path_.parent_path(), ec);
        {
            std::ofstream out(path_, std::ios::binary | std::ios::trunc);
            out.write(header.data(), static_cast<std::streamsize>(header.size()));
            out.close();
            if (out.fail()) return "cannot create " + printable(path_);
        }
        file_ = open_append(path_);
        if (file_ == kNoFile) return "cannot open " + printable(path_) + " for append";
        readable_ = true;
        return {};
    }

    bool active() const { return active_; }
    // The handle a child's stdout and stderr should be; kNoFile is the null device.
    NativeFile file() const { return file_; }

    // The hook's own output, in the order it happened relative to the children's.
    void append(std::string_view bytes) {
        if (file_ == kNoFile || bytes.empty()) return;
        if (!write_all(file_, bytes)) readable_ = false;
    }

    void end() {
        active_ = false;
        close_file(std::exchange(file_, kNoFile));
    }

    // The last `limit` bytes of the file, or nothing when it cannot stand in for
    // the ring buffer (never created, or a write to it failed). Valid after end().
    std::optional<std::string> tail(std::size_t limit) const {
        if (!readable_) return std::nullopt;
        std::ifstream in(path_, std::ios::binary);
        if (!in) return std::nullopt;
        in.seekg(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(in.tellg());
        if (size < 0) return std::nullopt;
        const auto skip = size > static_cast<std::streamoff>(limit)
                              ? size - static_cast<std::streamoff>(limit)
                              : std::streamoff{0};
        in.seekg(skip);
        std::string bytes(static_cast<std::size_t>(size - skip), '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(in.gcount()));
        return bytes;
    }
};

class HookOutput {
    std::string bytes_;
    bool truncated_ = false;
    HookLog* log_ = nullptr;  // also receives everything appended, while set

    static bool is_continuation_byte_(unsigned char byte) {
        return (byte & 0xc0) == 0x80;
    }

    static std::size_t valid_sequence_size_(std::string_view bytes,
                                            std::size_t offset) {
        const auto lead = static_cast<unsigned char>(bytes[offset]);
        if (lead <= 0x7f) return 1;

        std::size_t size = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            size = 2;
            codePoint = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            size = 3;
            codePoint = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            size = 4;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return 0;
        }

        if (offset + size > bytes.size()) return 0;
        for (std::size_t i = 1; i < size; ++i) {
            const auto byte = static_cast<unsigned char>(bytes[offset + i]);
            if (!is_continuation_byte_(byte)) return 0;
            codePoint = (codePoint << 6) | (byte & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return 0;
        }
        return size;
    }

    static std::string replace_invalid_utf8_(std::string_view bytes) {
        constexpr std::string_view replacement = "\xef\xbf\xbd";
        std::string valid;
        valid.reserve(bytes.size());
        for (std::size_t i = 0; i < bytes.size();) {
            const std::size_t sequenceSize = valid_sequence_size_(bytes, i);
            if (sequenceSize == 0) {
                valid.append(replacement);
                ++i;
            } else {
                valid.append(bytes.substr(i, sequenceSize));
                i += sequenceSize;
            }
        }
        return valid;
    }

public:
    void reset() {
        bytes_.clear();
        truncated_ = false;
    }

    void attach(HookLog* log) { log_ = log; }

    void append(std::string_view bytes) {
        if (bytes.empty()) return;
        if (log_) log_->append(bytes);
        if (bytes.size() >= kMaxHookOutputBytes) {
            truncated_ = truncated_ || !bytes_.empty() ||
                         bytes.size() > kMaxHookOutputBytes;
            bytes_.assign(bytes.substr(bytes.size() - kMaxHookOutputBytes));
            return;
        }
        if (bytes_.size() > kMaxHookOutputBytes - bytes.size()) {
            const std::size_t overflow =
                bytes_.size() + bytes.size() - kMaxHookOutputBytes;
            bytes_.erase(0, overflow);
            truncated_ = true;
        }
        bytes_.append(bytes);
    }

    std::string finish() const {
        std::string output = replace_invalid_utf8_(bytes_);
        bool truncated = truncated_;
        if (output.size() > kMaxHookOutputBytes) {
            std::size_t offset = output.size() - kMaxHookOutputBytes;
            while (offset < output.size() &&
                   is_continuation_byte_(static_cast<unsigned char>(output[offset]))) {
                ++offset;
            }
            output.erase(0, offset);
            truncated = true;
        }
        if (truncated) output.insert(0, HOOK_OUTPUT_TRUNCATED_MARKER);
        return output;
    }
};

HookOutput* hook_output(lua::State* L) {
    return static_cast<HookOutput*>(
        lua::touserdata(L, lua::upvalueindex(1)));
}

void append_lua_value(lua::State* L, HookOutput& output, int index) {
    unsigned long long size = 0;
    const char* value = lua::L_tolstring(L, index, &size);
    if (value) output.append(std::string_view(value, size));
    lua::pop(L, 1);
}

int capture_print(lua::State* L) {
    auto* output = hook_output(L);
    if (!output) return 0;
    const int count = lua::gettop(L);
    for (int i = 1; i <= count; ++i) {
        if (i > 1) output->append("\t");
        append_lua_value(L, *output, i);
    }
    output->append("\n");
    return 0;
}

int capture_io_write(lua::State* L) {
    auto* output = hook_output(L);
    if (output) {
        const int count = lua::gettop(L);
        for (int i = 1; i <= count; ++i) append_lua_value(L, *output, i);
    }
    lua::getglobal(L, "io");
    lua::getfield(L, -1, "stdout");
    lua::remove(L, -2);
    return 1;
}

int capture_stderr_write(lua::State* L) {
    if (lua::rawequal(L, 1, lua::upvalueindex(2))) {
        auto* output = hook_output(L);
        const int count = lua::gettop(L);
        if (output) {
            for (int i = 2; i <= count; ++i) append_lua_value(L, *output, i);
        }
        lua::pushvalue(L, 1);
        return 1;
    }

    const int argumentCount = lua::gettop(L);
    lua::pushvalue(L, lua::upvalueindex(3));
    lua::insert(L, 1);
    lua::call(L, argumentCount, lua::MULTRET);
    return lua::gettop(L);
}

// os.execute while a hook runs with a log: the same call with the same results
// as Lua 5.4's, but the child's fd 1 and 2 are the log. Upvalue 1 is the
// HookLog, upvalue 2 the original os.execute.
int hooked_os_execute(lua::State* L) {
    auto* log = static_cast<HookLog*>(lua::touserdata(L, lua::upvalueindex(1)));
    unsigned long long length = 0;
    // Nothing with a destructor may be live across this call: a bad argument
    // raises through it, exactly as it does in the original.
    const char* cmd = lua::L_optlstring(L, 1, nullptr, &length);
    if (!cmd || !log || !log->active()) {
        // os.execute() with no command asks whether a shell exists, and a
        // reference kept past the hook must behave like the original again.
        const int argumentCount = lua::gettop(L);
        lua::pushvalue(L, lua::upvalueindex(2));
        lua::insert(L, 1);
        lua::call(L, argumentCount, lua::MULTRET);
        return lua::gettop(L);
    }

    const shell::Status status =
        shell::run(std::string_view(cmd, length),
                   { shell::Sink::File, shell::Sink::File, log->file() }).status;
    shell::push_status(L, status);
    return 3;
}

// _LIBXPKG_NOTICES: the list log.warn / log.error append to while a hook runs;
// _LIBXPKG_CALL_FAILURES: the one compat.lua's _LIBXPKG_CALL_FAILED appends to.
void begin_notices(lua::State* L) {
    lua::newtable(L);
    lua::setglobal(L, "_LIBXPKG_NOTICES");
    lua::newtable(L);
    lua::setglobal(L, "_LIBXPKG_CALL_FAILURES");
}

std::vector<mcpplibs::xpkg::HookCallFailure> collect_call_failures(lua::State* L) {
    std::vector<mcpplibs::xpkg::HookCallFailure> failures;
    lua::getglobal(L, "_LIBXPKG_CALL_FAILURES");
    if (lua::type(L, -1) == lua::TTABLE) {
        const auto count = static_cast<lua::Integer>(lua::rawlen(L, -1));
        for (lua::Integer i = 1; i <= count; ++i) {
            lua::rawgeti(L, -1, i);
            if (lua::type(L, -1) == lua::TTABLE) {
                mcpplibs::xpkg::HookCallFailure f;
                auto text = [&](const char* key) {
                    lua::getfield(L, -1, key);
                    std::string v = lua::type(L, -1) == lua::TSTRING ? lua::tostring(L, -1) : "";
                    lua::pop(L, 1);
                    return v;
                };
                f.call = text("call");
                f.what = text("what");
                f.why = text("why");
                failures.push_back(std::move(f));
            }
            lua::pop(L, 1);
        }
    }
    lua::pop(L, 1);
    lua::pushnil(L);
    lua::setglobal(L, "_LIBXPKG_CALL_FAILURES");
    return failures;
}

std::vector<mcpplibs::xpkg::HookNotice> collect_notices(lua::State* L) {
    std::vector<mcpplibs::xpkg::HookNotice> notices;
    lua::getglobal(L, "_LIBXPKG_NOTICES");
    if (lua::type(L, -1) == lua::TTABLE) {
        const auto count = static_cast<std::size_t>(lua::rawlen(L, -1));
        for (std::size_t i = 1; i <= count && notices.size() < mcpplibs::xpkg::kMaxHookNotices; ++i) {
            lua::rawgeti(L, -1, static_cast<lua::Integer>(i));
            if (lua::type(L, -1) == lua::TTABLE) {
                mcpplibs::xpkg::HookNotice notice;
                lua::getfield(L, -1, "level");
                if (lua::type(L, -1) == lua::TSTRING) notice.level = lua::tostring(L, -1);
                lua::pop(L, 1);
                lua::getfield(L, -1, "text");
                if (lua::type(L, -1) == lua::TSTRING) notice.text = lua::tostring(L, -1);
                lua::pop(L, 1);
                if (notice.text.size() > mcpplibs::xpkg::kMaxHookNoticeBytes) {
                    notice.text.resize(mcpplibs::xpkg::kMaxHookNoticeBytes);
                    notice.text += " [...]";
                }
                notices.push_back(std::move(notice));
            }
            lua::pop(L, 1);
        }
        if (count > mcpplibs::xpkg::kMaxHookNotices) {
            notices.back() = {.level = "warn",
                              .text = std::to_string(count - mcpplibs::xpkg::kMaxHookNotices + 1)
                                      + " more lines; the hook's output has them"};
        }
    }
    lua::pop(L, 1);
    lua::pushnil(L);
    lua::setglobal(L, "_LIBXPKG_NOTICES");
    return notices;
}

// ---- The process and extraction functions a hook reaches (design §9.6, §9.7) ----

// The command of system.exec / system.run: a string is one shell line, a list
// is a program and its arguments. Read without raising: what is wrong is
// returned, and system.lua raises it at the recipe's line.
std::expected<shell::Command, std::string> read_command(lua::State* L) {
    shell::Command command;
    if (lua::type(L, 1) == lua::TSTRING) {
        command.line = lua::tostring(L, 1);
    } else if (lua::type(L, 1) == lua::TTABLE) {
        const auto count = static_cast<lua::Integer>(lua::rawlen(L, 1));
        if (count == 0) return std::unexpected("the command list is empty");
        for (lua::Integer i = 1; i <= count; ++i) {
            lua::rawgeti(L, 1, i);
            const int type = lua::type(L, -1);
            if (type != lua::TSTRING && type != lua::TNUMBER) {
                std::string what = lua::L_typename(L, -1);
                lua::pop(L, 1);
                return std::unexpected(std::format("element {} of the command list is a {}, not a string", i, what));
            }
            command.argv.emplace_back(lua::tostring(L, -1));
            lua::pop(L, 1);
        }
    } else {
        return std::unexpected(std::format("the command is a {}, not a string or a list", lua::L_typename(L, 1)));
    }
    if (lua::type(L, 2) == lua::TTABLE) {
        lua::getfield(L, 2, "cwd");
        if (lua::type(L, -1) == lua::TSTRING) command.cwd = lua::tostring(L, -1);
        else if (lua::type(L, -1) != lua::TNIL) {
            lua::pop(L, 1);
            return std::unexpected("cwd is not a string");
        }
        lua::pop(L, 1);
        lua::getfield(L, 2, "env");
        if (lua::type(L, -1) == lua::TTABLE) {
            lua::pushnil(L);
            while (lua::next(L, -2)) {
                if (lua::type(L, -2) != lua::TSTRING || lua::type(L, -1) != lua::TSTRING) {
                    lua::pop(L, 3);
                    return std::unexpected("env maps names to strings");
                }
                command.env.emplace_back(lua::tostring(L, -2), lua::tostring(L, -1));
                lua::pop(L, 1);
            }
        } else if (lua::type(L, -1) != lua::TNIL) {
            lua::pop(L, 1);
            return std::unexpected("env is not a table");
        }
        lua::pop(L, 1);
        // Same order on every run: environment entries are a set.
        std::ranges::sort(command.env);
    }
    return command;
}

bool option_flag(lua::State* L, const char* name) {
    if (lua::type(L, 2) != lua::TTABLE) return false;
    lua::getfield(L, 2, name);
    const bool on = lua::toboolean(L, -1);
    lua::pop(L, 1);
    return on;
}

// _LIBXPKG_SPAWN(cmd, opt) -> status, code, output, message. Never raises:
// status is "exit" | "signal" | "failed" (could not start) | "refused"
// (will not start it as written). The child's stdout and stderr go to the
// hook log while one is kept (the terminal for opt.tty, the result for
// opt.capture's stdout). Upvalue 1 is the executor's HookLog.
int lua_spawn(lua::State* L) {
    auto* log = static_cast<HookLog*>(lua::touserdata(L, lua::upvalueindex(1)));
    const bool capture = option_flag(L, "capture");
    const bool tty = option_flag(L, "tty");
    {
        auto command = read_command(L);
        if (!command) {
            lua::pushstring(L, "refused");
            lua::pushinteger(L, 0);
            lua::pushstring(L, "");
            lua::pushstring(L, command.error().c_str());
            return 4;
        }
        const bool logged = log && log->active() && !tty;
        shell::Streams streams;
        streams.out = capture ? shell::Sink::Capture : (logged ? shell::Sink::File : shell::Sink::Inherit);
        streams.err = logged ? shell::Sink::File : shell::Sink::Inherit;
        streams.file = logged ? log->file() : shell::kNoFile;
        const auto ran = shell::spawn(*command, streams);
        const char* status = "failed";
        switch (ran.status.kind) {
            case shell::Status::Kind::Exit:    status = "exit"; break;
            case shell::Status::Kind::Signal:  status = "signal"; break;
            case shell::Status::Kind::Failed:  status = "failed"; break;
            case shell::Status::Kind::Refused: status = "refused"; break;
        }
        lua::pushstring(L, status);
        lua::pushinteger(L, ran.status.code);
        lua::pushlstring(L, ran.output.data(), ran.output.size());
        if (ran.status.message.empty()) lua::pushnil(L);
        else lua::pushstring(L, ran.status.message.c_str());
    }
    return 4;
}

// fs.extract(archive, dir, { strip = n }): the host's extractor (its port),
// as an action. Upvalue 1 is the executor's HostPorts.
int lua_extract(lua::State* L) {
    auto* ports = static_cast<mcpplibs::xpkg::HostPorts*>(lua::touserdata(L, lua::upvalueindex(1)));
    const char* archive = lua::L_checklstring(L, 1, nullptr);
    const char* dir = lua::L_checklstring(L, 2, nullptr);
    lua::Integer strip = 0;
    bool strip_ok = true;
    if (lua::type(L, 3) == lua::TTABLE) {
        lua::getfield(L, 3, "strip");
        if (lua::isinteger(L, -1)) strip = lua::tointeger(L, -1);
        else if (lua::type(L, -1) != lua::TNIL) strip_ok = false;
        lua::pop(L, 1);
    }
    bool bad = false;
    {
        fsapi::Error error;
        if (!strip_ok || strip < 0 || strip > 64) {
            error = fsapi::describe("extract", archive, "strip is a whole number of path components (0 or more)");
        } else if (!ports || !ports->extract) {
            error = fsapi::describe("extract", archive, "this host gives hooks no extractor");
        } else {
            try {
                if (auto why = ports->extract(archive, dir, static_cast<int>(strip)))
                    error = fsapi::describe("extract", archive, *why);
            } catch (const std::exception& e) {
                error = fsapi::describe("extract", archive, e.what());
            }
        }
        if (error) {
            fsapi::push_error(L, *error);
            bad = true;
        }
    }
    if (bad) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}

// One warning through the log module. Called with capture already installed,
// so it lands in HookResult::output and not on the terminal.
void warn_through_log_module(lua::State* L, const std::string& message) {
    const int top = lua::gettop(L);
    lua::getglobal(L, "_LIBXPKG_MODULES");
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::getfield(L, -1, "log");
        if (lua::type(L, -1) == lua::TTABLE) {
            lua::getfield(L, -1, "warn");
            if (lua::type(L, -1) == lua::TFUNCTION) {
                lua::pushstring(L, "%s");
                lua::pushstring(L, message.c_str());
                lua::pcall(L, 2, 0, 0);
            }
        }
    }
    lua::settop(L, top);
}

class HookCapture {
    lua::State* L_ = nullptr;
    HookOutput& output_;
    HookLog* log_ = nullptr;
    int osExecuteRef_ = 0;
    int printRef_ = 0;
    int ioRef_ = 0;
    int ioWriteRef_ = 0;
    int stderrRef_ = 0;
    int stderrMethodsRef_ = 0;
    int stderrWriteRef_ = 0;

    void restore_() {
        if (!L_) return;

        if (osExecuteRef_ != 0) {
            lua::getglobal(L_, "os");
            if (lua::type(L_, -1) == lua::TTABLE) {
                lua::rawgeti(L_, lua::REGISTRYINDEX, osExecuteRef_);
                lua::setfield(L_, -2, "execute");
            }
            lua::pop(L_, 1);
            lua::L_unref(L_, lua::REGISTRYINDEX, osExecuteRef_);
            osExecuteRef_ = 0;
        }
        if (log_) {
            output_.attach(nullptr);
            log_->end();
        }

        lua::rawgeti(L_, lua::REGISTRYINDEX, ioRef_);
        lua::rawgeti(L_, lua::REGISTRYINDEX, ioWriteRef_);
        lua::setfield(L_, -2, "write");
        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrRef_);
        lua::setfield(L_, -2, "stderr");
        lua::setglobal(L_, "io");

        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrMethodsRef_);
        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrWriteRef_);
        lua::setfield(L_, -2, "write");
        lua::pop(L_, 1);

        lua::rawgeti(L_, lua::REGISTRYINDEX, printRef_);
        lua::setglobal(L_, "print");
        lua::L_unref(L_, lua::REGISTRYINDEX, printRef_);
        lua::L_unref(L_, lua::REGISTRYINDEX, ioRef_);
        lua::L_unref(L_, lua::REGISTRYINDEX, ioWriteRef_);
        lua::L_unref(L_, lua::REGISTRYINDEX, stderrRef_);
        lua::L_unref(L_, lua::REGISTRYINDEX, stderrMethodsRef_);
        lua::L_unref(L_, lua::REGISTRYINDEX, stderrWriteRef_);
        L_ = nullptr;
    }

public:
    // `log` is null for a hook that asked for none: nothing below changes then.
    HookCapture(lua::State* L, HookOutput& output, HookLog* log = nullptr)
        : L_(L), output_(output), log_(log) {
        output_.reset();
        output_.attach(log_);

        lua::getglobal(L_, "print");
        printRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);
        lua::getglobal(L_, "io");
        ioRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);

        lua::rawgeti(L_, lua::REGISTRYINDEX, ioRef_);
        lua::getfield(L_, -1, "write");
        ioWriteRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);
        lua::getfield(L_, -1, "stderr");
        stderrRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);

        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrRef_);
        lua::getmetatable(L_, -1);
        lua::getfield(L_, -1, "__index");
        stderrMethodsRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);
        lua::pop(L_, 2);

        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrMethodsRef_);
        lua::getfield(L_, -1, "write");
        stderrWriteRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);
        lua::pop(L_, 1);

        lua::pushlightuserdata(L_, &output_);
        lua::pushcclosure(L_, capture_print, 1);
        lua::setglobal(L_, "print");

        lua::pushlightuserdata(L_, &output_);
        lua::pushcclosure(L_, capture_io_write, 1);
        lua::setfield(L_, -2, "write");

        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrMethodsRef_);
        lua::pushlightuserdata(L_, &output_);
        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrRef_);
        lua::rawgeti(L_, lua::REGISTRYINDEX, stderrWriteRef_);
        lua::pushcclosure(L_, capture_stderr_write, 3);
        lua::setfield(L_, -2, "write");
        lua::pop(L_, 2);

        if (log_) {
            lua::getglobal(L_, "os");
            if (lua::type(L_, -1) == lua::TTABLE) {
                lua::getfield(L_, -1, "execute");
                osExecuteRef_ = lua::L_ref(L_, lua::REGISTRYINDEX);
                lua::pushlightuserdata(L_, log_);
                lua::rawgeti(L_, lua::REGISTRYINDEX, osExecuteRef_);
                lua::pushcclosure(L_, hooked_os_execute, 2);
                lua::setfield(L_, -2, "execute");
            }
            lua::pop(L_, 1);
        }
    }

    ~HookCapture() { restore_(); }

    HookCapture(const HookCapture&) = delete;
    HookCapture& operator=(const HookCapture&) = delete;

    std::string finish() {
        restore_();
        if (log_) {
            // The file holds the children's output as well as the hook's own,
            // in order; the ring buffer only ever saw the latter.
            if (auto tail = log_->tail(kMaxHookOutputBytes + 1)) {
                output_.reset();
                output_.append(*tail);
            }
        }
        return output_.finish();
    }
};

} // namespace mcpplibs::xpkg::detail

// ---- PackageExecutor ----

export namespace mcpplibs::xpkg {

class PackageExecutor {
    lua::State* L_   = nullptr;
    fs::path    pkg_ ;
    ExecutionBoundary boundary_;
    HookResponse boundaryResponse_;
    ExecutionContext boundaryContext_;
    std::string boundaryError_;
    HookResult execute_external_(HookAction action, const ExecutionContext& ctx,
                                 HookType hook = HookType::Installed, std::string_view logLevel = {});
    std::unique_ptr<detail::HookOutput> hookOutput_ =
        std::make_unique<detail::HookOutput>();
    std::unique_ptr<detail::HookLog> hookLog_ =
        std::make_unique<detail::HookLog>();
    // Heap-held like the two above: the Lua closures registered in the
    // constructor keep its address, and the executor may be moved.
    std::unique_ptr<HostPorts> ports_ = std::make_unique<HostPorts>();
    // The registry reference of a spec "3" recipe's environment table, where
    // its hooks are; 0 for a recipe that runs in the global environment.
    int recipeEnv_ = 0;

    // A value the recipe defined (a hook, xpkg_main), pushed.
    void push_recipe_value_(std::string_view name) const {
        if (recipeEnv_ != 0) {
            lua::rawgeti(L_, lua::REGISTRYINDEX, recipeEnv_);
            lua::getfield(L_, -1, std::string(name).c_str());
            lua::remove(L_, -2);
        } else {
            lua::getglobal(L_, std::string(name).c_str());
        }
    }

    // The functions whose state is this executor's: the process layer (it
    // writes to the hook log) and the host's extractor.
    void register_host_functions_() {
        lua::pushlightuserdata(L_, hookLog_.get());
        lua::pushcclosure(L_, detail::lua_spawn, 1);
        lua::setglobal(L_, "_LIBXPKG_SPAWN");
        lua::getglobal(L_, "_LIBXPKG_MODULES");
        if (lua::type(L_, -1) == lua::TTABLE) {
            lua::getfield(L_, -1, "fs");
            if (lua::type(L_, -1) == lua::TTABLE) {
                lua::pushlightuserdata(L_, ports_.get());
                lua::pushcclosure(L_, detail::lua_extract, 1);
                lua::setfield(L_, -2, "extract");
            }
            lua::pop(L_, 1);
        }
        lua::pop(L_, 1);
    }

public:
    explicit PackageExecutor(lua::State* L, fs::path pkg)
        : L_(L), pkg_(std::move(pkg)) {
        if (L_) register_host_functions_();
    }

    // What the host lends this package's hooks (fs.extract). A boundary
    // executor's hooks run elsewhere: the worker sets its own (HookWorker).
    void set_host_ports(HostPorts ports) { *ports_ = std::move(ports); }

    // Run the recipe's file: in the global environment with the compatibility
    // layer (spec "1", "2", none), or in an environment of its own (spec "3",
    // spec3.lua) under the top-level rule. An error message, or nothing.
    // The index's template (index-compat.json's recipes.template) runs after
    // the file, in the same environment, as if written at its end.
    std::optional<std::string> load_recipe_(const fs::path& file, bool spec3) {
        auto declared = recipe_index_declarations(file);
        if (!declared) return declared.error();
        const fs::path recipe_template = declared->recipe_template;
        if (!spec3) {
            if (lua::L_dofile(L_, file.string().c_str()) == lua::OK
                    && (recipe_template.empty() || lua::L_dofile(L_, recipe_template.string().c_str()) == lua::OK))
                return std::nullopt;
            std::string err = lua::tostring(L_, -1);
            lua::pop(L_, 1);
            return err;
        }
        lua::getglobal(L_, "_LIBXPKG_SPEC3_ENV");
        if (lua::pcall(L_, 0, 1, 0) != lua::OK || lua::type(L_, -1) != lua::TTABLE) {
            std::string err = lua::type(L_, -1) == lua::TSTRING ? lua::tostring(L_, -1) : "no environment";
            lua::pop(L_, 1);
            return "the spec 3 environment: " + err;
        }
        recipeEnv_ = lua::L_ref(L_, lua::REGISTRYINDEX);
        if (lua::L_loadfilex(L_, file.string().c_str(), "t") != lua::OK) {
            std::string err = lua::tostring(L_, -1);
            lua::pop(L_, 1);
            return err;
        }
        lua::rawgeti(L_, lua::REGISTRYINDEX, recipeEnv_);
        lua::setupvalue(L_, -2, 1);  // the chunk's _ENV
        lua::sethook(L_, detail::toplevel_call_hook, lua::MASKCALL, 0);
        int status = lua::pcall(L_, 0, 0, 0);
        if (status == lua::OK && !recipe_template.empty()) {
            status = lua::L_loadfilex(L_, recipe_template.string().c_str(), "t");
            if (status == lua::OK) {
                lua::rawgeti(L_, lua::REGISTRYINDEX, recipeEnv_);
                lua::setupvalue(L_, -2, 1);
                status = lua::pcall(L_, 0, 0, 0);
            }
        }
        lua::sethook(L_, nullptr, 0, 0);
        if (status == lua::OK) return std::nullopt;
        std::string err = lua::tostring(L_, -1) ? lua::tostring(L_, -1) : "error";
        lua::pop(L_, 1);
        return err;
    }

    PackageExecutor(fs::path pkg, ExecutionBoundary boundary, HookResponse loaded);

    ~PackageExecutor() {
        if (L_) { lua::close(L_); L_ = nullptr; }
    }

    PackageExecutor(const PackageExecutor&)            = delete;
    PackageExecutor& operator=(const PackageExecutor&) = delete;

    PackageExecutor(PackageExecutor&& o) noexcept
        : L_(std::exchange(o.L_, nullptr)),
          pkg_(std::move(o.pkg_)),
          boundary_(std::move(o.boundary_)),
          boundaryResponse_(std::move(o.boundaryResponse_)),
          boundaryContext_(std::move(o.boundaryContext_)),
          boundaryError_(std::move(o.boundaryError_)),
          hookOutput_(std::move(o.hookOutput_)),
          hookLog_(std::move(o.hookLog_)),
          ports_(std::move(o.ports_)),
          recipeEnv_(std::exchange(o.recipeEnv_, 0)) {}

    PackageExecutor& operator=(PackageExecutor&& o) noexcept {
        if (this != &o) {
            if (L_) lua::close(L_);
            L_   = std::exchange(o.L_, nullptr);
            pkg_ = std::move(o.pkg_);
            boundary_ = std::move(o.boundary_);
            boundaryResponse_ = std::move(o.boundaryResponse_);
            boundaryContext_ = std::move(o.boundaryContext_);
            boundaryError_ = std::move(o.boundaryError_);
            hookOutput_ = std::move(o.hookOutput_);
            hookLog_ = std::move(o.hookLog_);
            ports_ = std::move(o.ports_);
            recipeEnv_ = std::exchange(o.recipeEnv_, 0);
        }
        return *this;
    }

    bool has_hook(HookType hook) const {
        if (boundary_) return boundaryResponse_.hooks.at(static_cast<std::size_t>(hook));
        push_recipe_value_(detail::hook_name(hook));
        bool found = (lua::type(L_, -1) == lua::TFUNCTION);
        lua::pop(L_, 1);
        return found;
    }

    HookResult run_hook(HookType hook, const ExecutionContext& ctx) {
        if (boundary_) return execute_external_(HookAction::RunHook, ctx, hook);
        // Inject context before each hook call
        detail::inject_context(L_, ctx);

        auto name = detail::hook_name(hook);
        push_recipe_value_(name);

        if (lua::type(L_, -1) != lua::TFUNCTION) {
            lua::pop(L_, 1);
            return HookResult{ .success = false,
                               .error   = "hook not found: " + std::string(name) };
        }

        detail::HookLog* log = nullptr;
        std::string logProblem;
        if (!ctx.hook_log.empty()) {
            log = hookLog_.get();
            logProblem = log->begin(
                ctx.hook_log,
                "# " + std::string(name) + " hook of " + ctx.pkg_name + "@" +
                    ctx.version + "\n");
        }
        detail::HookCapture capture(L_, *hookOutput_, log);
        if (!logProblem.empty()) {
            detail::warn_through_log_module(
                L_, "hook output log unavailable (" + logProblem +
                        "); output of child processes is discarded");
        }
        // After the host's own warning above: notices are what the recipe says.
        detail::begin_notices(L_);
        HookResult result;
        if (lua::pcall(L_, 0, 1, 0) == lua::OK) {
            int t = lua::type(L_, -1);
            if (t == lua::TBOOLEAN) {
                result.success = lua::toboolean(L_, -1);
                if (!result.success) {
                    result.error = std::string(name) + " hook returned false";
                }
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
            if (const char* error = lua::tostring(L_, -1)) {
                result.error = error;
            }
            lua::pop(L_, 1);
        }

        if (!result.success && result.error.empty()) {
            result.error = std::string(name) + " hook failed";
        }
        result.notices = detail::collect_notices(L_);
        result.call_failures = detail::collect_call_failures(L_);
        result.output = capture.finish();

        return result;
    }

    // Auto-stamp wrapper packages (linux-headers etc.) whose install hook
    // returns success but leaves install_dir empty because the real
    // payload lives elsewhere (subos sysroot via config(), or a
    // sub-dependency). Without something in install_dir, xlings's catalog
    // probe (`is_directory && !is_empty`) flags the package as not
    // installed on every dependent install, re-running this hook + the
    // expensive config hook every time. Drop a tiny `.xim-installed`
    // stamp so install_dir is non-empty post-install. Authors no longer
    // need to remember to write this themselves.
    //
    // CRITICAL: Call this AFTER all install paths complete (run_hook +
    // any consumer-side stage_extracted_payload / default_install
    // fallback). If invoked from inside run_hook, the stamp is written
    // before the consumer's "is install_dir empty?" check, which would
    // suppress the extracted-payload fallback used by packages whose
    // install hook silently fails (e.g. tarballs without a top-level
    // dir, where the hook's `os.mv(extracted_dir, install_dir)` no-ops
    // because extracted_dir doesn't exist).
    //
    // Policy: only writes when (a) install_dir exists or can be created,
    // (b) install_dir is empty. If anything else has populated it, we
    // don't touch it.
    void apply_install_stamp_if_empty(const ExecutionContext& ctx) {
        if (ctx.install_dir.empty()) return;
        std::error_code ec;
        fs::create_directories(ctx.install_dir, ec);
        ec.clear();
        bool dir_exists = fs::exists(ctx.install_dir, ec)
                       && fs::is_directory(ctx.install_dir, ec);
        ec.clear();
        if (!dir_exists || !fs::is_empty(ctx.install_dir, ec)) return;
        auto stamp_path = ctx.install_dir / ".xim-installed";
        std::ofstream out(stamp_path);
        if (!out.is_open()) return;
        // Key=value INI for human-readable + grep-friendly access.
        // schema=1 reserves room to evolve the format later.
        out << "schema = 1\n"
            << "name = "       << ctx.pkg_name << "\n"
            << "version = "    << ctx.version  << "\n"
            << "platform = "   << ctx.platform << "\n";
    }

    HookResult check_installed(const ExecutionContext& ctx) {
        return run_hook(HookType::Installed, ctx);
    }

    // Run elfpatch.apply_auto() after the install hook: the payload's loader
    // and search paths, as the recipe and its dependencies declare them.
    // `elfpatch` holds what it did and its outcome; `output` is the
    // "<scanned> <patched> <failed>" text. Safe to call unconditionally.
    HookResult apply_elfpatch_auto() {
        if (boundary_) return execute_external_(HookAction::Elfpatch, boundaryContext_);
        constexpr const char* script = R"__LUA__(
            local ep = _LIBXPKG_MODULES and _LIBXPKG_MODULES["elfpatch"]
            if not ep then return nil end
            return ep.apply_auto()
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
            if (lua::type(L_, -1) == lua::TTABLE) {
                ElfpatchResult r;
                auto integer = [&](const char* key) {
                    lua::getfield(L_, -1, key);
                    const int v = lua::isinteger(L_, -1) ? static_cast<int>(lua::tointeger(L_, -1)) : 0;
                    lua::pop(L_, 1);
                    return v;
                };
                auto text = [&](const char* key) {
                    lua::getfield(L_, -1, key);
                    std::string v = lua::type(L_, -1) == lua::TSTRING ? lua::tostring(L_, -1) : "";
                    lua::pop(L_, 1);
                    return v;
                };
                r.outcome = text("outcome");
                if (r.outcome.empty()) r.outcome = "done";
                r.reason = text("reason");
                r.scanned = integer("scanned");
                r.patched = integer("patched");
                r.failed = integer("failed");
                r.skipped_foreign = integer("skipped_foreign");
                r.skipped_static = integer("skipped_static");
                r.skipped_listed = integer("skipped_listed");
                r.tool_missing = text("tool_missing");
                result.output = std::format("{} {} {}", r.scanned, r.patched, r.failed);
                result.elfpatch = std::move(r);
            } else {
                result.output = "no-ep 0 0 0";
            }
            lua::pop(L_, 1);
        } else {
            result.success = false;
            result.error = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        }
        return result;
    }

    std::vector<XvmOp> xvm_operations() {
        if (boundary_) return boundaryResponse_.xvm_ops;
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
                op.var        = read_field("var");
                op.value      = read_field("value");
                op.mode       = read_field("mode");
                op.src        = read_field("src");
                op.dst        = read_field("dst");

                // Read args array (ordered; empty when absent)
                lua::getfield(L_, -1, "args");
                if (lua::type(L_, -1) == lua::TTABLE) {
                    for (int i = 1;; ++i) {
                        lua::rawgeti(L_, -1, i);
                        if (lua::type(L_, -1) != lua::TSTRING) {
                            lua::pop(L_, 1);
                            break;
                        }
                        op.args.emplace_back(lua::tostring(L_, -1));
                        lua::pop(L_, 1);
                    }
                }
                lua::pop(L_, 1);

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
        if (boundary_) return execute_external_(HookAction::RunScript, ctx);
        detail::inject_context(L_, ctx);

        lua::newtable(L_);
        lua::setglobal(L_, "_XVM_OPS");
        lua::newtable(L_);
        lua::setglobal(L_, "_INSTALL_REQUESTS");

        push_recipe_value_("xpkg_main");
        if (lua::type(L_, -1) != lua::TFUNCTION) {
            lua::pop(L_, 1);
            return HookResult{ .success = false,
                               .error = "xpkg_main not found in script" };
        }

        int nargs = static_cast<int>(ctx.args.size());
        for (auto& arg : ctx.args) {
            lua::pushstring(L_, arg.c_str());
        }

        detail::begin_notices(L_);
        HookResult result;
        if (lua::pcall(L_, nargs, 0, 0) == lua::OK) {
            result.success = true;
        } else {
            result.success = false;
            result.error = lua::tostring(L_, -1);
            lua::pop(L_, 1);
        }
        result.notices = detail::collect_notices(L_);
        result.call_failures = detail::collect_call_failures(L_);
        return result;
    }

    // Set log level for Lua scripts: "debug", "info", "warn", "error", "silent"
    void set_log_level(std::string_view level) {
        if (boundary_) {
            auto result = execute_external_(HookAction::SetLogLevel, boundaryContext_, HookType::Installed, level);
            if (!result.success) boundaryError_ = result.error;
            return;
        }
        std::string script = std::string("local log = _LIBXPKG_MODULES and _LIBXPKG_MODULES['log']; ")
            + "if log and log.set_level then log.set_level('" + std::string(level) + "') end";
        if (lua::L_loadstring(L_, script.c_str()) == lua::OK) {
            lua::pcall(L_, 0, 0, 0);
        } else {
            lua::pop(L_, 1);
        }
    }

    std::vector<InstallRequest> install_requests() {
        if (boundary_) return boundaryResponse_.install_requests;
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
    // The recipe's spec chooses its environment, so it is read from the
    // recipe's text, not by running it: its top level runs once, in the
    // environment its spec chooses (a top level with effects -- a file
    // written, a program started -- must not run first somewhere else).
    std::string declaredSpec;
    {
        std::ifstream in(pkg_path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        declaredSpec = declared_spec(text.str()).value_or("");
    }
    const auto spec = spec_number(declaredSpec);
    if (spec && *spec > kHighestSpec) {
        return std::unexpected(std::format(
            "{}: spec \"{}\" is newer than this libxpkg reads (the highest is {})",
            pkg_path.string(), declaredSpec, kHighestSpec));
    }
    const bool spec3 = spec && *spec == 3;

    lua::State* L = lua::L_newstate();
    if (!L) return std::unexpected("failed to create lua state");

    lua::L_openlibs(L);

    std::string err;
    if (!detail::load_stdlib(L, err)) {
        lua::close(L);
        return std::unexpected(err);
    }

    // Derive pkgindex root from pkg_path for top-level import("xim.pkgindex.*").
    // Package files live at <pkgindex>/pkgs/<letter>/<name>.lua — 3 levels up.
    {
        auto pkgindex = pkg_path.parent_path().parent_path().parent_path();
        std::error_code ec;
        if (fs::is_directory(pkgindex / "libs", ec)) {
            lua::pushstring(L, pkgindex.string().c_str());
            lua::setglobal(L, "_PKGINDEX_DIR");
        }
    }

    // Before the recipe runs: its top level may already start a program.
    PackageExecutor executor(L, pkg_path);
    if (auto failed = executor.load_recipe_(pkg_path, spec3)) {
        return std::unexpected("failed to load package: " + *failed);
    }
    return executor;
}


// The external boundary runs even the package's top-level Lua in the worker.
// An empty or failing boundary is an error; it never falls back to local Lua.
std::expected<PackageExecutor, std::string>
create_executor(const fs::path& pkg_path, ExecutionBoundary boundary);

// Use only INSIDE the consumer's isolated worker. One instance is kept for
// one package, so recipe globals and cumulative effects survive hook calls.
class HookWorker {
    std::optional<PackageExecutor> executor_;
    fs::path package_;
    HostPorts ports_;

public:
    // What the hooks this worker runs are lent (the worker is the host).
    void set_host_ports(HostPorts ports) { ports_ = std::move(ports); }
    std::expected<HookResponse, std::string> dispatch(const HookInvocation& invocation);
};

} // export namespace mcpplibs::xpkg
