module;

export module mcpplibs.xpkg.fs;
import mcpplibs.xpkg.shell;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;
namespace fs  = std::filesystem;

// The file functions a hook reaches: the module interface's actions and
// queries (fsapi, the `fs` module) and the compatibility layer's os.* ones
// (register_os_funcs). The executor registers them into each Lua state it
// makes (load_stdlib).
export namespace mcpplibs::xpkg::detail {

#if defined(_WIN32)
inline constexpr bool kWindowsHost = true;
#else
inline constexpr bool kWindowsHost = false;
#endif

// A failed action of the compatibility layer, noted for the host while a hook
// runs (compat.lua's _LIBXPKG_CALL_FAILED); what the name returns is unchanged.
void note_call_failure(lua::State* L, const char* call, const std::string& what, const std::string& why) {
    lua::getglobal(L, "_LIBXPKG_CALL_FAILED");
    if (lua::type(L, -1) != lua::TFUNCTION) {
        lua::pop(L, 1);
        return;
    }
    lua::pushstring(L, call);
    lua::pushstring(L, what.c_str());
    lua::pushstring(L, why.c_str());
    if (lua::pcall(L, 3, 0, 0) != lua::OK) lua::pop(L, 1);
}

// The compatibility layer's os.* functions in C++ (std::filesystem), replacing
// compat.lua's shell-based ones. Called after compat.lua runs. Their behaviour
// is frozen (compat.lua's header); a failure of os.cp, os.mv / os.trymv and
// os.mkdir is noted, and returns false as it always did.
void register_os_funcs(lua::State* L) {
    lua::getglobal(L, "os");
    if (lua::type(L, -1) != lua::TTABLE) {
        lua::pop(L, 1);
        lua::newtable(L);
        lua::setglobal(L, "os");
        lua::getglobal(L, "os");
    }

    // os.execute runs through shell::run, the one way a command line reaches
    // the shell (libxpkg#52); the child inherits fd 1 and 2. While a hook runs
    // with ExecutionContext::hook_log, os.execute is replaced by a version that
    // sends the child's output to the log; a command that needs the terminal
    // (system.exec's `tty`) reaches this one under an internal name.
    // Deliberately not an os.* function: recipes should not depend on it.
    lua::pushcfunction(L, shell::lua_execute);
    lua::setfield(L, -2, "execute");
    lua::pushcfunction(L, shell::lua_execute);
    lua::setglobal(L, "_LIBXPKG_EXEC_INHERIT");

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
        if (ec) note_call_failure(L, "os.cp", std::string(src) + " -> " + dst, ec.message());
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
        const auto renameError = ec.message();
        // Cross-device: copy + remove
        ec.clear();
        fs::copy(src, dst,
                 fs::copy_options::recursive |
                 fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing, ec);
        if (ec) {
            note_call_failure(L, "os.mv", std::string(s) + " -> " + d, renameError);
            lua::pushboolean(L, 0);
            return 1;
        }
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
        if (ec) note_call_failure(L, "os.mkdir", p, ec.message());
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

    // os.iorun(cmd) -> string: stdout read through a pipe, stderr discarded.
    // Nothing is appended to the caller's line, so a redirect it carries
    // keeps its meaning and a quoted program path is not re-quoted.
    lua::pushcfunction(L, shell::lua_iorun);
    lua::setfield(L, -2, "iorun");

    lua::pop(L, 1); // pop os table
}

// ---- fs: the actions and queries of the module interface (design 2026-11-27 §9.6) ----
//
// An action states what holds when it returns; when that cannot be made true it
// raises, naming the action, the path and the system's reason, at the recipe's
// line. A query raises only when it cannot answer; "there is none" is an answer.
//
// An action returns true. It never returns false -- a failure raises -- so
// there is nothing to check, and an older recipe's `if not fs.symlink(...)`
// still reads right.
//
// Each Lua function below reads its arguments, calls a C++ function that does
// the work and returns an error message (or nothing), and raises only after
// that function has returned: a Lua error may unwind with longjmp, and no
// object with a destructor is alive by then.
namespace fsapi {

using Error = std::optional<std::string>;

std::string describe(std::string_view action, const fs::path& p, std::string_view why) {
    return std::format("fs.{} {}: {}", action, p.string(), why);
}

bool exists_no_follow(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() != fs::file_type::not_found;
}

// `inner` is `outer` or below it.
bool within(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    const auto a = fs::weakly_canonical(inner, ec);
    const auto b = fs::weakly_canonical(outer, ec);
    auto ai = a.begin();
    for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
        if (ai == a.end() || *ai != *bi) return false;
    }
    return true;
}

fs::path sibling_scratch(const fs::path& p) {
    return p.parent_path() / std::format(".xpkg-{}-{}", p.filename().string(),
        std::chrono::steady_clock::now().time_since_epoch().count());
}

Error copy_tree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    const auto st = fs::symlink_status(src, ec);
    if (fs::is_symlink(st)) fs::copy_symlink(src, dst, ec);
    else if (fs::is_directory(st)) fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    else fs::copy_file(src, dst, ec);
    if (ec) return ec.message();
    return std::nullopt;
}

// move and copy share the destination's rule: `dst` is the final path, its
// parents are created, and one already there is an error unless `replace`.
Error clear_destination(std::string_view action, const fs::path& dst, bool replace) {
    std::error_code ec;
    if (exists_no_follow(dst)) {
        if (!replace) return describe(action, dst, "already exists (pass { replace = true } to replace it)");
        fs::remove_all(dst, ec);
        if (ec) return describe(action, dst, "cannot be replaced: " + ec.message());
    }
    if (dst.has_parent_path()) {
        fs::create_directories(dst.parent_path(), ec);
        if (ec) return describe(action, dst.parent_path(), ec.message());
    }
    return std::nullopt;
}

Error mkdir(const fs::path& p) {
    std::error_code ec, probe;
    fs::create_directories(p, ec);
    if (fs::is_directory(p, probe)) return std::nullopt;
    return describe("mkdir", p, ec ? ec.message() : std::string("exists and is not a directory"));
}

Error remove(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) return describe("remove", p, ec.message());
    if (exists_no_follow(p)) return describe("remove", p, "is still there");
    return std::nullopt;
}

Error move(const fs::path& src, const fs::path& dst, bool replace) {
    if (!exists_no_follow(src)) return describe("move", src, "does not exist");
    // Replacing a directory with something inside it (a top-level directory
    // onto its parent): set the source aside first, or removing the
    // destination would remove it.
    fs::path from = src;
    std::error_code ec;
    if (replace && exists_no_follow(dst) && within(src, dst)) {
        from = sibling_scratch(dst);
        fs::rename(src, from, ec);
        if (ec) return describe("move", src, ec.message());
    }
    if (auto e = clear_destination("move", dst, replace)) return e;
    fs::rename(from, dst, ec);
    if (!ec) return std::nullopt;
    if (ec == std::errc::cross_device_link) {
        if (auto e = copy_tree(from, dst)) {
            std::error_code ignored;
            fs::remove_all(dst, ignored);
            return describe("move", src, "copying to another device: " + *e);
        }
        fs::remove_all(from, ec);
        if (ec) return describe("move", src, "copied, and the source cannot be removed: " + ec.message());
        return std::nullopt;
    }
    return describe("move", src, ec.message() + " (to " + dst.string() + ")");
}

Error copy(const fs::path& src, const fs::path& dst, bool replace) {
    if (!exists_no_follow(src)) return describe("copy", src, "does not exist");
    std::error_code ec;
    if (replace && exists_no_follow(dst) && within(src, dst)) {
        const auto scratch = sibling_scratch(dst);
        if (auto e = copy_tree(src, scratch)) {
            fs::remove_all(scratch, ec);
            return describe("copy", src, *e);
        }
        if (auto e = clear_destination("copy", dst, true)) return e;
        fs::rename(scratch, dst, ec);
        if (ec) return describe("copy", src, ec.message() + " (to " + dst.string() + ")");
        return std::nullopt;
    }
    if (auto e = clear_destination("copy", dst, replace)) return e;
    if (auto e = copy_tree(src, dst)) return describe("copy", src, *e + " (to " + dst.string() + ")");
    return std::nullopt;
}

Error symlink(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    // A link to a directory is a directory link (Windows asks which); a
    // relative target is read from the link's directory.
    const auto resolved = target.is_absolute() ? target : link.parent_path() / target;
    if (fs::is_directory(resolved, ec)) fs::create_directory_symlink(target, link, ec);
    else fs::create_symlink(target, link, ec);
    if (ec) return describe("symlink", link, ec.message() + " (to " + target.string() + ")");
    return std::nullopt;
}

Error write(const fs::path& p, std::string_view text) {
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) return describe("write", p.parent_path(), ec.message());
    }
    if (fs::is_directory(p, ec)) return describe("write", p, "is a directory");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return describe("write", p, "cannot be opened for writing");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return describe("write", p, "the write failed");
    return std::nullopt;
}

Error chmod(const fs::path& p, std::string_view mode) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return describe("chmod", p, "does not exist");
    fs::perms perms = fs::perms::none;
    fs::perm_options how = fs::perm_options::replace;
    if (mode == "+x") {
        perms = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
        how = fs::perm_options::add;
    } else if ((mode.size() == 3 || mode.size() == 4)
               && std::ranges::all_of(mode, [](char c) { return c >= '0' && c <= '7'; })) {
        unsigned value = 0;
        for (char c : mode) value = value * 8 + static_cast<unsigned>(c - '0');
        perms = static_cast<fs::perms>(value);
    } else {
        return describe("chmod", p, std::format("mode \"{}\" is neither octal digits (\"755\") nor \"+x\"", mode));
    }
    if constexpr (kWindowsHost) return std::nullopt;  // permissions are not Windows'
    fs::permissions(p, perms, how, ec);
    if (ec) return describe("chmod", p, ec.message());
    return std::nullopt;
}

Error read(const fs::path& p, std::string& out) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return describe("read", p, "is a directory");
    std::ifstream in(p, std::ios::binary);
    if (!in) return describe("read", p, exists_no_follow(p) ? "cannot be read" : "does not exist");
    std::ostringstream ss;
    ss << in.rdbuf();
    out = std::move(ss).str();
    return std::nullopt;
}

struct Entry { std::string name, path, type; };

Error list(const fs::path& dir, std::vector<Entry>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return describe("list", dir, exists_no_follow(dir) ? "is not a directory" : "does not exist");
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code probe;
        const auto st = it->symlink_status(probe);
        const char* type = fs::is_symlink(st) ? "symlink"
                         : fs::is_directory(st) ? "dir"
                         : fs::is_regular_file(st) ? "file" : "other";
        out.push_back({ it->path().filename().string(), it->path().string(), type });
    }
    if (ec) return describe("list", dir, ec.message());
    std::ranges::sort(out, {}, &Entry::name);
    return std::nullopt;
}

// Push "<recipe>:<line>: <message>" for lua::error to raise.
void push_error(lua::State* L, const std::string& message) {
    lua::L_where(L, 1);
    lua::pushlstring(L, message.data(), message.size());
    lua::concat(L, 2);
}

// One action: raise when it returned an error. The error object is a
// temporary, gone before lua::error runs.
bool failed(lua::State* L, Error&& error) {
    if (!error) return false;
    push_error(L, *error);
    return true;
}

bool replace_option(lua::State* L, int index) {
    if (lua::type(L, index) != lua::TTABLE) return false;
    lua::getfield(L, index, "replace");
    const bool replace = lua::toboolean(L, -1);
    lua::pop(L, 1);
    return replace;
}

const char* path_arg(lua::State* L, int index) { return lua::L_checklstring(L, index, nullptr); }

int lua_is_dir(lua::State* L) {
    std::error_code ec;
    lua::pushboolean(L, fs::is_directory(fs::path(path_arg(L, 1)), ec) ? 1 : 0);
    return 1;
}
int lua_mkdir(lua::State* L) {
    const char* p = path_arg(L, 1);
    if (failed(L, mkdir(p))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_remove(lua::State* L) {
    const char* p = path_arg(L, 1);
    if (failed(L, remove(p))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_move(lua::State* L) {
    const char* src = path_arg(L, 1);
    const char* dst = path_arg(L, 2);
    if (failed(L, move(src, dst, replace_option(L, 3)))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_copy(lua::State* L) {
    const char* src = path_arg(L, 1);
    const char* dst = path_arg(L, 2);
    if (failed(L, copy(src, dst, replace_option(L, 3)))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_symlink(lua::State* L) {
    const char* target = path_arg(L, 1);
    const char* link = path_arg(L, 2);
    if (failed(L, symlink(target, link))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_write(lua::State* L) {
    const char* p = path_arg(L, 1);
    unsigned long long size = 0;
    const char* text = lua::L_checklstring(L, 2, &size);
    if (failed(L, write(p, std::string_view(text, size)))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_chmod(lua::State* L) {
    const char* p = path_arg(L, 1);
    const char* mode = lua::L_checklstring(L, 2, nullptr);
    if (failed(L, chmod(p, mode))) return lua::error(L);
    lua::pushboolean(L, 1);
    return 1;
}
int lua_read(lua::State* L) {
    const char* p = path_arg(L, 1);
    bool bad = false;
    {
        std::string content;
        if (auto error = read(p, content)) {
            push_error(L, *error);
            bad = true;
        } else {
            lua::pushlstring(L, content.data(), content.size());
        }
    }
    if (bad) return lua::error(L);
    return 1;
}
int lua_list(lua::State* L) {
    const char* p = path_arg(L, 1);
    bool bad = false;
    {
        std::vector<Entry> entries;
        if (auto error = list(p, entries)) {
            push_error(L, *error);
            bad = true;
        } else {
            lua::newtable(L);
            int i = 1;
            for (const auto& e : entries) {
                lua::newtable(L);
                lua::pushstring(L, e.name.c_str());
                lua::setfield(L, -2, "name");
                lua::pushstring(L, e.path.c_str());
                lua::setfield(L, -2, "path");
                lua::pushstring(L, e.type.c_str());
                lua::setfield(L, -2, "type");
                lua::rawseti(L, -2, i++);
            }
        }
    }
    if (bad) return lua::error(L);
    return 1;
}

struct Function { const char* name; lua::CFunction func; };
constexpr Function kFunctions[] = {
    {"is_dir", lua_is_dir}, {"read", lua_read}, {"list", lua_list},
    {"mkdir", lua_mkdir}, {"remove", lua_remove}, {"move", lua_move}, {"copy", lua_copy},
    {"symlink", lua_symlink}, {"write", lua_write}, {"chmod", lua_chmod},
};

} // namespace fsapi

// Register C++-backed fs module into _LIBXPKG_MODULES["fs"].
// Provides filesystem primitives that are missing from the os.* layer:
// symlink creation/reading, file/dir enumeration, single-file copy, etc.
// All functions use std::filesystem — no shell commands.
void register_fs_module(lua::State* L) {
    lua::newtable(L);  // the fs module table

    // The actions and queries of the module's interface (fsapi above): each
    // action states what holds when it returns and raises otherwise.
    for (const auto& reg : fsapi::kFunctions) {
        lua::pushcfunction(L, reg.func);
        lua::setfield(L, -2, reg.name);
    }

    // fs.readlink(path) -> string | nil
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushnil(L); return 1; }
        std::error_code ec;
        auto target = fs::read_symlink(fs::path(p), ec);
        if (ec) { lua::pushnil(L); return 1; }
        lua::pushstring(L, target.string().c_str());
        return 1;
    });
    lua::setfield(L, -2, "readlink");

    // fs.is_symlink(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_symlink(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "is_symlink");

    // fs.exists(path) -> bool  (follows symlinks)
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::exists(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "exists");

    // fs.is_directory(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_directory(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "is_directory");

    // fs.is_file(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        lua::pushboolean(L, fs::is_regular_file(fs::path(p), ec) ? 1 : 0);
        return 1;
    });
    lua::setfield(L, -2, "is_file");

    // fs.mkdir_p(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::create_directories(fs::path(p), ec);
        lua::pushboolean(L, ec ? 0 : 1);
        return 1;
    });
    lua::setfield(L, -2, "mkdir_p");

    // fs.remove_all(path) -> bool
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::remove_all(fs::path(p), ec);
        lua::pushboolean(L, 1);
        return 1;
    });
    lua::setfield(L, -2, "remove_all");

    // fs.copy_file(src, dst) -> bool  (single file, overwrite)
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* src = lua::tostring(L, 1);
        const char* dst = lua::tostring(L, 2);
        if (!src || !dst) { lua::pushboolean(L, 0); return 1; }
        std::error_code ec;
        fs::copy_file(fs::path(src), fs::path(dst),
                      fs::copy_options::overwrite_existing, ec);
        lua::pushboolean(L, ec ? 0 : 1);
        return 1;
    });
    lua::setfield(L, -2, "copy_file");

    // fs.entries(dir) -> table of {path, name, type}
    // type: "file", "directory", "symlink", "other"
    // Non-recursive. Returns full paths.
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* dir = lua::tostring(L, 1);
        lua::newtable(L);
        if (!dir) return 1;
        std::error_code ec;
        fs::path dp(dir);
        if (!fs::is_directory(dp, ec)) return 1;
        int idx = 1;
        for (auto& entry : fs::directory_iterator(dp, ec)) {
            lua::newtable(L);
            lua::pushstring(L, entry.path().string().c_str());
            lua::setfield(L, -2, "path");
            lua::pushstring(L, entry.path().filename().string().c_str());
            lua::setfield(L, -2, "name");
            const char* etype = "other";
            if (entry.is_symlink(ec))          etype = "symlink";
            else if (entry.is_regular_file(ec)) etype = "file";
            else if (entry.is_directory(ec))    etype = "directory";
            lua::pushstring(L, etype);
            lua::setfield(L, -2, "type");
            lua::rawseti(L, -2, idx++);
        }
        return 1;
    });
    lua::setfield(L, -2, "entries");

    // fs.files(dir, recursive?) -> table of file paths
    // recursive: optional bool, default false
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* dir = lua::tostring(L, 1);
        bool recursive = lua::toboolean(L, 2);
        lua::newtable(L);
        if (!dir) return 1;
        std::error_code ec;
        fs::path dp(dir);
        if (!fs::is_directory(dp, ec)) return 1;
        int idx = 1;
        if (recursive) {
            for (auto& entry : fs::recursive_directory_iterator(dp, ec)) {
                if (entry.is_regular_file(ec)) {
                    lua::pushstring(L, entry.path().string().c_str());
                    lua::rawseti(L, -2, idx++);
                }
            }
        } else {
            for (auto& entry : fs::directory_iterator(dp, ec)) {
                if (entry.is_regular_file(ec)) {
                    lua::pushstring(L, entry.path().string().c_str());
                    lua::rawseti(L, -2, idx++);
                }
            }
        }
        return 1;
    });
    lua::setfield(L, -2, "files");

    // fs.dirs(dir) -> table of subdirectory paths (non-recursive)
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* dir = lua::tostring(L, 1);
        lua::newtable(L);
        if (!dir) return 1;
        std::error_code ec;
        fs::path dp(dir);
        if (!fs::is_directory(dp, ec)) return 1;
        int idx = 1;
        for (auto& entry : fs::directory_iterator(dp, ec)) {
            if (entry.is_directory(ec)) {
                lua::pushstring(L, entry.path().string().c_str());
                lua::rawseti(L, -2, idx++);
            }
        }
        return 1;
    });
    lua::setfield(L, -2, "dirs");

    // fs.basename(path) -> string
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushstring(L, ""); return 1; }
        lua::pushstring(L, fs::path(p).filename().string().c_str());
        return 1;
    });
    lua::setfield(L, -2, "basename");

    // fs.dirname(path) -> string
    lua::pushcfunction(L, [](lua::State* L) -> int {
        const char* p = lua::tostring(L, 1);
        if (!p) { lua::pushstring(L, ""); return 1; }
        lua::pushstring(L, fs::path(p).parent_path().string().c_str());
        return 1;
    });
    lua::setfield(L, -2, "dirname");

    // Store into _LIBXPKG_MODULES["fs"]
    lua::getglobal(L, "_LIBXPKG_MODULES");
    lua::insert(L, -2);       // stack: [modules, fs_table]
    lua::setfield(L, -2, "fs");  // modules["fs"] = fs_table
    lua::pop(L, 1);           // pop modules
}

} // export namespace mcpplibs::xpkg::detail
