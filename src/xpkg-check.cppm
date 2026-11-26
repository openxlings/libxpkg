module;

export module mcpplibs.xpkg.check;
import mcpplibs.xpkg;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.lua_stdlib;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;
namespace fs  = std::filesystem;

// The format's checker (design 2026-11-27 §9.9): what is wrong with a recipe by
// the rules of the xpkg format, as findings. One implementation, used by every
// host (xlings' `index check`) and by the official index's CI; a rule that
// belongs to one index (its review requirements, its exceptions) stays there.
export namespace mcpplibs::xpkg {

struct Finding {
    std::string code;      // the rule, e.g. "arch-unstated"
    std::string severity;  // "error" | "warning" | "info"
    std::string message;   // what is wrong and what to write instead
    int line = 0;          // the recipe's line, 0 when the finding is about it as a whole
};

// The findings for one recipe file, in the order of the rules below. None is a
// recipe the format accepts.
//
//   load           error    the recipe does not load (its package table cannot be read)
//   spec-missing   warning  no `spec`: read as spec 1
//   spec-newer     error    a spec higher than this library reads
//   spec-old       info     spec 1 or 2; spec 3 is the current one
//   arch-unstated  error (3) / warning  an entry with an artifact on a platform with
//                           two host architectures (linux) that does not list them
//   archs-union    warning  package.archs is not the union of what the entries list
//   compat-name    error (3) / info     a name of the compatibility layer in the code
//   strip          error (3) / warning  a `strip` that is no component count, or one in a
//                           recipe of spec 1 or 2, where the host does not read it
//   build-hook     warning  build() in a recipe of spec 3, which defines installed,
//                           install, config and uninstall
//   dropped        warning  a field the loader dropped (extensions, host_facts)
std::vector<Finding> check_recipe(const fs::path& recipe);

// The findings of every recipe under <index>/pkgs, keyed by the recipe's path,
// and of the index's own declarations (its index-compat.json) under the key "".
std::map<std::string, std::vector<Finding>> check_index(const fs::path& index);

} // export namespace mcpplibs::xpkg

namespace mcpplibs::xpkg::check_detail {

// The recipe's text with comments and string literals blanked to spaces --
// newlines kept, so a position still has its line. The names a recipe uses
// are read from what is left.
std::string code_only(std::string_view text) {
    std::string out(text);
    std::size_t i = 0;
    auto blank = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k)
            if (out[k] != '\n') out[k] = ' ';
    };
    // A long bracket at i: [[ or [=*[; its level, or -1.
    auto long_open = [&](std::size_t at) -> int {
        if (at >= text.size() || text[at] != '[') return -1;
        std::size_t k = at + 1;
        int level = 0;
        while (k < text.size() && text[k] == '=') { ++level; ++k; }
        return (k < text.size() && text[k] == '[') ? level : -1;
    };
    auto long_close = [&](std::size_t from, int level) -> std::size_t {
        const std::string closing = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
        const auto end = text.find(closing, from);
        return end == std::string_view::npos ? text.size() : end + closing.size();
    };
    while (i < text.size()) {
        const char c = text[i];
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            const int level = long_open(i + 2);
            std::size_t end;
            if (level >= 0) end = long_close(i + 2 + static_cast<std::size_t>(level) + 2, level);
            else {
                end = text.find('\n', i);
                if (end == std::string_view::npos) end = text.size();
            }
            blank(i, end);
            i = end;
        } else if (c == '"' || c == '\'') {
            std::size_t k = i + 1;
            while (k < text.size() && text[k] != c && text[k] != '\n') {
                if (text[k] == '\\') ++k;
                ++k;
            }
            blank(i, k + 1);
            i = k + 1;
        } else if (const int level = long_open(i); level >= 0) {
            const auto end = long_close(i + static_cast<std::size_t>(level) + 2, level);
            blank(i, end);
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

int line_of(std::string_view text, std::size_t at) {
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(at, text.size())), '\n'));
}

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The compatibility layer's names and what replaces each, from the interface
// manifest (manifest.lua's `compat`).
std::vector<std::pair<std::string, std::string>> compat_names() {
    static const auto names = [] {
        std::vector<std::pair<std::string, std::string>> out;
        lua::State* L = lua::L_newstate();
        if (!L) return out;
        lua::L_openlibs(L);
        if (lua::L_loadstring(L, std::string(detail::manifest_lua).c_str()) == lua::OK
                && lua::pcall(L, 0, 1, 0) == lua::OK) {
            lua::getfield(L, -1, "compat");
            const auto count = static_cast<lua::Integer>(lua::rawlen(L, -1));
            for (lua::Integer i = 1; i <= count; ++i) {
                lua::rawgeti(L, -1, i);
                lua::rawgeti(L, -1, 1);
                std::string name = lua::type(L, -1) == lua::TSTRING ? lua::tostring(L, -1) : "";
                lua::pop(L, 1);
                lua::rawgeti(L, -1, 2);
                std::string instead = lua::type(L, -1) == lua::TSTRING ? lua::tostring(L, -1) : "";
                lua::pop(L, 2);
                // "import of an unknown module" and the like are not names.
                if (!name.empty() && name.find(' ') == std::string::npos) out.emplace_back(name, instead);
            }
        }
        lua::close(L);
        return out;
    }();
    return names;
}

// Each use of a compatibility name in `code`: a dotted name (os.mv, fs.mkdir_p)
// where it is followed by something other than more name; a bare global
// (is_host, format, path) where it is not a field of something else and,
// for `path`, where the recipe does not import the path module.
// The recipe defines `name` itself (`local format`, `local function try`,
// `function raise`): its uses are its own, not the layer's.
bool defines(std::string_view code, std::string_view name) {
    for (const auto* form : {"local ", "local function ", "function "}) {
        const auto needle = std::string(form) + std::string(name);
        for (auto at = code.find(needle); at != std::string_view::npos; at = code.find(needle, at + 1)) {
            const auto end = at + needle.size();
            if (end >= code.size() || !is_name_char(code[end])) return true;
        }
    }
    return false;
}

std::vector<Finding> compat_uses(std::string_view code, bool spec3, bool imports_path) {
    std::vector<Finding> out;
    for (const auto& [name, instead] : compat_names()) {
        if (name == "path" && imports_path) continue;
        if (name.find('.') == std::string::npos && defines(code, name)) continue;
        for (std::size_t at = code.find(name); at != std::string_view::npos; at = code.find(name, at + 1)) {
            const auto end = at + name.size();
            if (at > 0 && (is_name_char(code[at - 1]) || code[at - 1] == '.' || code[at - 1] == ':')) continue;
            if (end < code.size() && is_name_char(code[end])) continue;
            // A bare `path` is the global only where it is then indexed.
            if (name == "path" && (end >= code.size() || code[end] != '.')) continue;
            out.push_back({ .code = "compat-name",
                            .severity = spec3 ? "error" : "info",
                            .message = instead.empty()
                                ? std::format("{} is the compatibility layer's; a recipe of spec 3 has no replacement and does without it", name)
                                : std::format("{} is the compatibility layer's; a recipe of spec 3 writes {}", name, instead),
                            .line = line_of(code, at) });
        }
    }
    std::ranges::sort(out, {}, &Finding::line);
    return out;
}

// The line of the version entry `version` of `platform` in the recipe text, a
// best guess for a message: `["1.2.3"] =` after the platform's name.
int entry_line(std::string_view text, std::string_view platform, std::string_view version) {
    auto p = text.find(std::string(platform) + " =");
    if (p == std::string_view::npos) p = text.find(std::string(platform) + "=");
    const auto from = p == std::string_view::npos ? 0 : p;
    const auto at = text.find("[\"" + std::string(version) + "\"]", from);
    return at == std::string_view::npos ? 0 : line_of(text, at);
}

} // namespace mcpplibs::xpkg::check_detail

namespace mcpplibs::xpkg {

std::vector<Finding> check_recipe(const fs::path& recipe) {
    using namespace check_detail;
    std::vector<Finding> out;
    std::string text;
    {
        std::ifstream in(recipe, std::ios::binary);
        if (!in) return { { .code = "load", .severity = "error", .message = recipe.string() + ": cannot be read" } };
        std::ostringstream ss;
        ss << in.rdbuf();
        text = std::move(ss).str();
    }
    auto loaded = load_package(recipe);
    if (!loaded) {
        out.push_back({ .code = "load", .severity = "error", .message = loaded.error() });
        // What the code uses is still worth saying when the cause is one of them.
        const bool spec3 = text.find("spec = \"3\"") != std::string::npos;
        for (auto& f : compat_uses(code_only(text), spec3, text.find("xim.libxpkg.path") != std::string::npos))
            if (spec3) out.push_back(std::move(f));
        return out;
    }
    const auto& pkg = *loaded;
    const auto spec = spec_number(pkg.spec);
    const bool spec3 = spec && *spec == 3;
    if (pkg.spec.empty()) {
        out.push_back({ .code = "spec-missing", .severity = "warning",
                        .message = "no spec: read as spec 1; a new recipe writes spec = \"3\"" });
    } else if (!spec) {
        out.push_back({ .code = "spec-missing", .severity = "warning",
                        .message = std::format("spec \"{}\" names no spec: read as spec 1", pkg.spec) });
    } else if (*spec > kHighestSpec) {
        out.push_back({ .code = "spec-newer", .severity = "error",
                        .message = std::format("spec \"{}\" is newer than this checker reads (the highest is {})", pkg.spec, kHighestSpec) });
        return out;
    } else if (*spec < 3) {
        out.push_back({ .code = "spec-old", .severity = "info",
                        .message = std::format("spec \"{}\": spec 3 is the current one (no compatibility layer, the host lands the download)", pkg.spec) });
    }

    // Architectures: an entry with an artifact names the ones it ships, on a
    // platform with two host architectures (design §5.3; today Linux).
    std::set<std::string> listed;
    bool anyEnumerates = false;
    std::vector<std::string> platforms;
    for (const auto& [platform, _] : pkg.xpm.entries) platforms.push_back(platform);
    std::ranges::sort(platforms);
    for (const auto& platform : platforms) {
        const auto& versions = pkg.xpm.entries.at(platform);
        std::vector<std::string> keys;
        for (const auto& [version, _] : versions) keys.push_back(version);
        std::ranges::sort(keys);
        for (const auto& version : keys) {
            const auto& entry = versions.at(version);
            if (!entry.ref.empty()) continue;
            const auto archs = entry_architectures(entry);
            if (!archs.empty()) anyEnumerates = true;
            listed.insert(archs.begin(), archs.end());
            if (platform != "linux" || pkg.type == PackageType::Subos) continue;
            if (declares_no_download_source(pkg.xpm, &entry, platform)) continue;
            const auto evidence = classify_arch_evidence(pkg, &entry);
            if (evidence == ArchEvidence::Weak || evidence == ArchEvidence::None) {
                out.push_back({ .code = "arch-unstated", .severity = spec3 ? "error" : "warning",
                                .message = std::format(
                                    "xpm.{}[\"{}\"]: one artifact and no architecture named; list it for the ones it is "
                                    "(sha256 = {{ x86_64 = \"<hash>\" }}, the same hash under each architecture when it "
                                    "has no machine code)", platform, version),
                                .line = entry_line(text, platform, version) });
            }
        }
    }
    if (anyEnumerates) {
        std::set<std::string> declared;
        for (const auto& a : pkg.archs) declared.insert(normalize_arch(a));
        std::vector<std::string> missing, extra;
        std::ranges::set_difference(listed, declared, std::back_inserter(missing));
        std::ranges::set_difference(declared, listed, std::back_inserter(extra));
        if (!missing.empty() || !extra.empty()) {
            std::string what;
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
                return s;
            };
            if (!missing.empty()) what += "entries list " + join(missing) + " and archs does not";
            if (!extra.empty()) what += std::string(what.empty() ? "" : "; ") + "archs lists " + join(extra) + " and no entry does";
            out.push_back({ .code = "archs-union", .severity = "warning",
                            .message = "package.archs is the union of what the entries list: " + what });
        }
    }

    // The names the code uses.
    const auto code = code_only(text);
    for (auto& f : compat_uses(code, spec3, text.find("xim.libxpkg.path") != std::string::npos))
        out.push_back(std::move(f));

    // strip, and what the loader dropped.
    bool stripStated = !pkg.xpm.platform_strip.empty();
    for (const auto& [_, versions] : pkg.xpm.entries)
        for (const auto& [__, entry] : versions)
            if (entry.strip >= 0) stripStated = true;
    for (const auto& w : pkg.warnings) {
        const bool isStrip = w.find(".strip:") != std::string::npos;
        out.push_back({ .code = isStrip ? "strip" : "dropped",
                        .severity = isStrip && spec3 ? "error" : "warning", .message = w });
    }
    if (stripStated && !spec3) {
        out.push_back({ .code = "strip", .severity = "warning",
                        .message = "strip is read for recipes of spec 3; a recipe of spec 1 or 2 is landed by its install hook" });
    }
    if (spec3) {
        for (std::size_t at = code.find("function build"); at != std::string::npos; at = code.find("function build", at + 1)) {
            const auto end = at + std::string_view("function build").size();
            if (end < code.size() && is_name_char(code[end])) continue;
            out.push_back({ .code = "build-hook", .severity = "warning",
                            .message = "build() is not a hook of spec 3 (installed, install, config, uninstall)",
                            .line = line_of(code, at) });
        }
    }
    return out;
}

std::map<std::string, std::vector<Finding>> check_index(const fs::path& index) {
    std::map<std::string, std::vector<Finding>> out;
    if (auto d = index_declarations(index); !d) {
        out[""].push_back({ .code = "load", .severity = "error", .message = d.error() });
    }
    std::error_code ec;
    std::vector<fs::path> recipes;
    for (fs::recursive_directory_iterator it(index / "pkgs", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".lua") recipes.push_back(it->path());
    if (ec && !fs::exists(index / "pkgs"))
        out[""].push_back({ .code = "load", .severity = "error", .message = (index / "pkgs").string() + ": no recipes here" });
    std::ranges::sort(recipes);
    for (const auto& r : recipes) {
        auto findings = check_recipe(r);
        if (!findings.empty()) out[r.string()] = std::move(findings);
    }
    return out;
}

} // namespace mcpplibs::xpkg
