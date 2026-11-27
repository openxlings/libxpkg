module;

export module mcpplibs.xpkg.conformance;
import mcpplibs.xpkg;
import mcpplibs.xpkg.lua_stdlib;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;
namespace fs  = std::filesystem;

// The conformance cases of the xpkg format (design 2026-11-27 §9.9): recipes,
// each with what reading it gives and what holds after its hooks run. The
// cases are tests/conformance/*.lua (the format is that directory's README);
// they are embedded, so every implementation runs the same ones: libxpkg's
// tests run each hook through its executor, a host (xlings) installs each
// package through its own install and checks the payload against the same
// expectations. What an expectation means is decided here, once.
export namespace mcpplibs::xpkg::conformance {

// What holds of one path, relative to the install directory.
struct Expectation {
    std::string path;
    std::string is;                    // "file" | "dir" | "symlink" | "absent"
    std::optional<std::string> text;   // a file's exact content
};

// A notice the hook leaves (log.warn, log.error): its level and a part of its text.
struct NoticeExpectation {
    std::string level;
    std::string contains;
};

// One hook run, in order, on the case's one install directory.
struct Step {
    std::string hook;                  // "installed" | "install" | "config" | "uninstall"
    bool ok = true;
    std::string error;                 // with ok = false: a part of the error
    std::vector<Expectation> after;
    std::vector<NoticeExpectation> notices;
    std::size_t call_failures = 0;     // the compatibility layer's failed calls it reports
};

// One resolution of the package's resource.
struct Resolution {
    std::string platform, arch, version;
    std::string url, sha256;           // empty: not checked
    std::optional<int> strip;
    std::string error;                 // non-empty: resolving fails with this in the error
};

struct Case {
    std::string name;                  // the file's stem: "xpm-strip"
    std::string chapter;               // the spec's chapter it is of
    std::string summary;
    std::string package;               // the recipe: pkgs/<first letter>/<package>.lua
    std::map<std::string, std::string> files;   // the index, relative path -> text
    // The loaded package table's fields as text (`fields()`); "error" names a
    // part of the error when the recipe does not load.
    std::map<std::string, std::string> loaded;
    std::string executor_error;        // non-empty: no executor is made; a part of why
    std::vector<Resolution> resolve;
    std::vector<Step> hooks;
    // A host's whole install of the package, on any platform: "installed",
    // "failed" (a hook fails) or "refused" (it is not run); empty: not a case
    // for a host's install. With "installed", `payload` holds after it.
    std::string install;
    std::vector<Expectation> payload;
};

// Every case, sorted by name. Throws std::runtime_error when a case does not
// read: the cases are this library's own, so that is its bug.
const std::vector<Case>& cases();

// Writes the case's index under `root`; returns the recipe's path.
fs::path materialize(const Case& c, const fs::path& root);

// What does not hold in `install_dir`, one line each; empty when all hold.
std::vector<std::string> unmet(const std::vector<Expectation>& expectations, const fs::path& install_dir);

// The specification's text (docs/spec/xpkg.md) of this version of libxpkg: a
// host that publishes a copy checks it against this.
std::string_view specification();

// The loaded package's fields as cases name them: name, spec, type, status,
// description, archs and programs (comma-joined, in order), and
// "platforms" (the xpm's platforms, sorted, comma-joined).
std::map<std::string, std::string> fields(const Package& package);

} // export namespace mcpplibs::xpkg::conformance

namespace mcpplibs::xpkg::conformance {

namespace {

std::string str_at(lua::State* L, int at) {
    if (lua::type(L, at) == lua::TSTRING) return lua::tostring(L, at);
    if (lua::type(L, at) == lua::TNUMBER) return std::to_string(static_cast<long long>(lua::tointeger(L, at)));
    return {};
}

std::string field(lua::State* L, const char* key) {
    lua::getfield(L, -1, key);
    auto v = str_at(L, -1);
    lua::pop(L, 1);
    return v;
}

// The list of expectations at the top of the stack: { { path, is [, text = ...] }, ... }.
std::vector<Expectation> expectations(lua::State* L) {
    std::vector<Expectation> out;
    if (lua::type(L, -1) != lua::TTABLE) return out;
    const auto n = static_cast<lua::Integer>(lua::rawlen(L, -1));
    for (lua::Integer i = 1; i <= n; ++i) {
        lua::rawgeti(L, -1, i);
        Expectation e;
        lua::rawgeti(L, -1, 1); e.path = str_at(L, -1); lua::pop(L, 1);
        lua::rawgeti(L, -1, 2); e.is = str_at(L, -1); lua::pop(L, 1);
        lua::getfield(L, -1, "text");
        if (lua::type(L, -1) == lua::TSTRING) e.text = lua::tostring(L, -1);
        lua::pop(L, 2);
        if (e.path.empty() || (e.is != "file" && e.is != "dir" && e.is != "symlink" && e.is != "absent"))
            throw std::runtime_error("an expectation names a path and file, dir, symlink or absent");
        out.push_back(std::move(e));
    }
    return out;
}

Case read_case(std::string_view name, std::string_view text) {
    lua::State* L = lua::L_newstate();
    if (!L) throw std::runtime_error("no Lua state");
    struct Close { lua::State* L; ~Close() { lua::close(L); } } close{L};
    lua::L_openlibs(L);
    const std::string chunk(text);
    if (lua::L_loadstring(L, chunk.c_str()) != lua::OK || lua::pcall(L, 0, 1, 0) != lua::OK)
        throw std::runtime_error(std::format("conformance case {}: {}", name, str_at(L, -1)));
    if (lua::type(L, -1) != lua::TTABLE)
        throw std::runtime_error(std::format("conformance case {}: returns no table", name));
    Case c;
    c.name = name;
    c.chapter = field(L, "chapter");
    c.summary = field(L, "summary");
    c.package = field(L, "package");
    c.install = field(L, "install");
    c.executor_error = field(L, "executor_error");

    lua::getfield(L, -1, "files");
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::pushnil(L);
        while (lua::next(L, -2) != 0) {
            c.files[str_at(L, -2)] = str_at(L, -1);
            lua::pop(L, 1);
        }
    }
    lua::pop(L, 1);

    lua::getfield(L, -1, "loaded");
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::pushnil(L);
        while (lua::next(L, -2) != 0) {
            c.loaded[str_at(L, -2)] = str_at(L, -1);
            lua::pop(L, 1);
        }
    }
    lua::pop(L, 1);

    lua::getfield(L, -1, "resolve");
    if (lua::type(L, -1) == lua::TTABLE) {
        const auto n = static_cast<lua::Integer>(lua::rawlen(L, -1));
        for (lua::Integer i = 1; i <= n; ++i) {
            lua::rawgeti(L, -1, i);
            Resolution r { .platform = field(L, "platform"), .arch = field(L, "arch"),
                           .version = field(L, "version"), .url = field(L, "url"),
                           .sha256 = field(L, "sha256"), .error = field(L, "error") };
            lua::getfield(L, -1, "strip");
            if (lua::type(L, -1) == lua::TNUMBER) r.strip = static_cast<int>(lua::tointeger(L, -1));
            lua::pop(L, 2);
            c.resolve.push_back(std::move(r));
        }
    }
    lua::pop(L, 1);

    lua::getfield(L, -1, "hooks");
    if (lua::type(L, -1) == lua::TTABLE) {
        const auto n = static_cast<lua::Integer>(lua::rawlen(L, -1));
        for (lua::Integer i = 1; i <= n; ++i) {
            lua::rawgeti(L, -1, i);
            Step s;
            lua::rawgeti(L, -1, 1); s.hook = str_at(L, -1); lua::pop(L, 1);
            lua::getfield(L, -1, "ok");
            s.ok = lua::type(L, -1) == lua::TNIL || lua::toboolean(L, -1);
            lua::pop(L, 1);
            s.error = field(L, "error");
            lua::getfield(L, -1, "after"); s.after = expectations(L); lua::pop(L, 1);
            lua::getfield(L, -1, "call_failures");
            if (lua::type(L, -1) == lua::TNUMBER) s.call_failures = static_cast<std::size_t>(lua::tointeger(L, -1));
            lua::pop(L, 1);
            lua::getfield(L, -1, "notices");
            if (lua::type(L, -1) == lua::TTABLE) {
                const auto m = static_cast<lua::Integer>(lua::rawlen(L, -1));
                for (lua::Integer j = 1; j <= m; ++j) {
                    lua::rawgeti(L, -1, j);
                    NoticeExpectation ne;
                    lua::rawgeti(L, -1, 1); ne.level = str_at(L, -1); lua::pop(L, 1);
                    lua::rawgeti(L, -1, 2); ne.contains = str_at(L, -1); lua::pop(L, 2);
                    s.notices.push_back(std::move(ne));
                }
            }
            lua::pop(L, 2);
            if (s.hook.empty()) throw std::runtime_error(std::format("conformance case {}: a step names no hook", name));
            c.hooks.push_back(std::move(s));
        }
    }
    lua::pop(L, 1);

    lua::getfield(L, -1, "payload");
    c.payload = expectations(L);
    lua::pop(L, 1);

    if (c.package.empty() || c.chapter.empty())
        throw std::runtime_error(std::format("conformance case {}: names no package or chapter", name));
    if (!c.install.empty() && c.install != "installed" && c.install != "failed" && c.install != "refused")
        throw std::runtime_error(std::format("conformance case {}: install is installed, failed or refused", name));
    return c;
}

std::string_view type_name(PackageType t) {
    switch (t) {
        case PackageType::Package:  return "package";
        case PackageType::Script:   return "script";
        case PackageType::Template: return "template";
        case PackageType::Config:   return "config";
        case PackageType::Subos:    return "subos";
    }
    return "package";
}

std::string_view status_name(PackageStatus s) {
    switch (s) {
        case PackageStatus::Dev:        return "dev";
        case PackageStatus::Stable:     return "stable";
        case PackageStatus::Deprecated: return "deprecated";
    }
    return "dev";
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

} // namespace

const std::vector<Case>& cases() {
    static const std::vector<Case> all = [] {
        std::vector<Case> out;
        for (const auto& [name, text] : detail::conformance_cases_lua) out.push_back(read_case(name, text));
        std::ranges::sort(out, {}, &Case::name);
        return out;
    }();
    return all;
}

fs::path materialize(const Case& c, const fs::path& root) {
    for (const auto& [rel, text] : c.files) {
        const auto file = root / fs::path(rel);
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    return root / "pkgs" / c.package.substr(0, 1) / (c.package + ".lua");
}

std::vector<std::string> unmet(const std::vector<Expectation>& expectations, const fs::path& install_dir) {
    std::vector<std::string> out;
    for (const auto& e : expectations) {
        const auto p = install_dir / fs::path(e.path);
        std::error_code ec;
        const auto st = fs::symlink_status(p, ec);
        const bool exists = !ec && st.type() != fs::file_type::not_found;
        std::string actual = !exists ? "absent"
            : st.type() == fs::file_type::symlink ? "symlink"
            : st.type() == fs::file_type::directory ? "dir"
            : st.type() == fs::file_type::regular ? "file" : "other";
        if (actual != e.is) {
            out.push_back(std::format("{}: is {}, expected {}", e.path, actual, e.is));
            continue;
        }
        if (e.text && actual == "file") {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str() != *e.text)
                out.push_back(std::format("{}: holds {:?}, expected {:?}", e.path, ss.str(), *e.text));
        }
    }
    return out;
}

std::string_view specification() { return detail::specification_md; }

std::map<std::string, std::string> fields(const Package& package) {
    std::map<std::string, std::string> out;
    out["name"] = package.name;
    out["spec"] = package.spec;
    out["description"] = package.description;
    out["type"] = std::string(type_name(package.type));
    out["status"] = std::string(status_name(package.status));
    out["archs"] = joined(package.archs);
    out["programs"] = joined(package.programs);
    std::vector<std::string> platforms;
    for (const auto& [platform, _] : package.xpm.entries) platforms.push_back(platform);
    std::ranges::sort(platforms);
    out["platforms"] = joined(platforms);
    return out;
}

} // namespace mcpplibs::xpkg::conformance
