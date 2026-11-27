module;

export module mcpplibs.xpkg.migrate;
import mcpplibs.xpkg;
import mcpplibs.xpkg.check;
import std;

// Rewriting a recipe of spec "1" or "2" into spec "3" (design 2026-11-27 §9.6,
// "迁移"): each name of the compatibility layer the interface manifest maps to
// a name of the interface is rewritten where the rewrite keeps what the call
// does; an install() that only moved the extracted archive into the install
// directory is replaced by `strip = 1` (§9.7); what needs a person is listed
// with its line. The recipe becomes spec "3" only when nothing is left: the
// names it writes are the modules', the same in both environments, so a
// recipe that is only partly rewritten still runs as spec 1 or 2.
export namespace mcpplibs::xpkg {

struct MigrationNote {
    int line = 0;         // the recipe's line
    std::string name;     // the name or construct, e.g. "os.dirs"
    std::string what;     // what a person writes instead
};

struct Migration {
    std::string text;               // the recipe, rewritten
    bool spec3 = false;             // declared spec "3": nothing was left
    int rewritten = 0;              // calls rewritten
    bool landing_removed = false;   // the landing install() is gone, `strip = 1` written
    std::vector<std::string> imports_added;   // modules imported for the new names
    std::vector<MigrationNote> left;          // for a person
};

struct MigrateOptions {
    // An index lib (libs/<name>.lua): imported by recipes of every spec, so
    // its names are rewritten and its imports added, and nothing else -- it
    // has no package table, no spec and no landing.
    bool lib = false;
    // Functions of the index's libs that land the download the old way (find
    // the extracted directory, move it into the install directory), named as
    // a recipe calls them (`sysroot.adopt_payload`): a call of one as a
    // statement of install() is that recipe's landing, replaced by `strip`.
    std::vector<std::string> landing_calls;
};

Migration migrate_recipe(std::string_view text, MigrateOptions options = {});

} // export namespace mcpplibs::xpkg

namespace mcpplibs::xpkg::migrate_detail {

bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

int line_of(std::string_view text, std::size_t at) {
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(at, text.size())), '\n'));
}

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}

// A replacement of [from, to) in the text.
struct Edit {
    std::size_t from, to;
    std::string with;
};

// The ')' that closes the '(' at `open` in blanked code, or npos.
std::size_t closing(std::string_view code, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
        if (code[i] == '(' || code[i] == '{' || code[i] == '[') ++depth;
        else if (code[i] == ')' || code[i] == '}' || code[i] == ']') {
            if (--depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

// The top-level commas of the argument list between open and close.
std::vector<std::size_t> top_commas(std::string_view code, std::size_t open, std::size_t close) {
    std::vector<std::size_t> out;
    int depth = 0;
    for (std::size_t i = open + 1; i < close; ++i) {
        const char c = code[i];
        if (c == '(' || c == '{' || c == '[') ++depth;
        else if (c == ')' || c == '}' || c == ']') --depth;
        else if (c == ',' && depth == 0) out.push_back(i);
    }
    return out;
}

// The call at `at` begins a statement: what comes before it ends one (the
// start of the text, `;`, a closing bracket, a string, a name, or a keyword
// that opens a block: `then`, `do`, `else`, `repeat`, `end`), not one that
// asks for a value (an operator, `=`, `,`, an opening bracket, `return`, a
// condition's keyword). `code` is `text` blanked; a string or comment between
// the last code and the call is read from `text`.
bool is_statement(std::string_view text, std::string_view code, std::size_t at) {
    std::size_t p = at;
    while (p > 0 && (code[p - 1] == ' ' || code[p - 1] == '\t' || code[p - 1] == '\n' || code[p - 1] == '\r')) --p;
    for (std::size_t k = p; k < at; ++k) {
        if (text[k] == '-' && k + 1 < at && text[k + 1] == '-') {
            // A comment: a long one to its closing bracket, else to its line's end.
            std::size_t b = k + 2, level = 0;
            if (b < at && text[b] == '[') {
                std::size_t e = b + 1;
                while (e < at && text[e] == '=') { ++level; ++e; }
                if (e < at && text[e] == '[') {
                    const auto close = text.find("]" + std::string(level, '=') + "]", e + 1);
                    k = close == std::string_view::npos ? at : close + level + 1;
                    continue;
                }
            }
            while (k < at && text[k] != '\n') ++k;
            continue;
        }
        if (text[k] == '"' || text[k] == '\'' || text[k] == '[') return true;   // a string ends the expression
    }
    if (p == 0) return true;
    const char c = code[p - 1];
    if (c == ';' || c == ')' || c == ']' || c == '}') return true;
    if (!name_char(c)) return false;
    std::size_t w = p;
    while (w > 0 && name_char(code[w - 1])) --w;
    const auto word = code.substr(w, p - w);
    for (const std::string_view value : {"and", "or", "not", "return", "if", "elseif", "while", "until", "in", "local"})
        if (word == value) return false;
    return true;
}

// Each use of `name` in the code: a whole dotted name, not a field of something else.
std::vector<std::size_t> uses(std::string_view code, std::string_view name) {
    std::vector<std::size_t> out;
    for (auto at = code.find(name); at != std::string_view::npos; at = code.find(name, at + 1)) {
        const auto end = at + name.size();
        if (at > 0 && (name_char(code[at - 1]) || code[at - 1] == '.' || code[at - 1] == ':')) continue;
        if (end < code.size() && name_char(code[end])) continue;
        out.push_back(at);
    }
    return out;
}

bool defines(std::string_view code, std::string_view name) {
    for (const auto* form : {"local ", "local function ", "function "}) {
        const auto needle = std::string(form) + std::string(name);
        for (auto at = code.find(needle); at != std::string_view::npos; at = code.find(needle, at + 1)) {
            const auto end = at + needle.size();
            if (end >= code.size() || !name_char(code[end])) return true;
        }
    }
    return false;
}

// A name renamed where it is called, and the module the new name is in.
struct Rename { std::string_view old_name, new_name, module; };
constexpr Rename kRenames[] = {
    {"os.isfile", "fs.is_file", "fs"},       {"os.isdir", "fs.is_dir", "fs"},
    {"fs.is_directory", "fs.is_dir", "fs"},  {"os.tryrm", "fs.remove", "fs"},
    {"fs.remove_all", "fs.remove", "fs"},    {"os.remove", "fs.remove", "fs"},
    {"os.mkdir", "fs.mkdir", "fs"},          {"fs.mkdir_p", "fs.mkdir", "fs"},
    {"fs.copy_file", "fs.copy", "fs"},
    {"io.readfile", "fs.read", "fs"},        {"io.writefile", "fs.write", "fs"},
    {"os.exec", "system.exec", "system"},    {"os.host", "pkginfo.platform", "pkginfo"},
    {"os.arch", "pkginfo.arch", "pkginfo"},  {"fs.basename", "path.filename", "path"},
    {"fs.dirname", "path.directory", "path"}, {"pkgmanager.uninstall", "pkgmanager.remove", "pkgmanager"},
    {"format", "string.format", ""},         {"raise", "error", ""},
    {"cprint", "log.info", "log"},
};

// What a person writes for a name no rewrite keeps the meaning of.
struct Manual { std::string_view name, what; };
constexpr Manual kManual[] = {
    {"try", "pcall(function() ... end): the failure it tolerates, written out"},
    {"os.cd", "system.exec(cmd, { cwd = dir })"},
    {"os.setenv", "system.exec(cmd, { env = { NAME = value } })"},
    {"os.addenv", "system.exec(cmd, { env = { NAME = value } })"},
    {"io.open", "fs.read / fs.write"},
    {"io.popen", "system.run(cmd).out, or system.exec(cmd, { capture = true })"},
    {"os.dirs", "fs.list(dir): entries { name, path, type }, no glob"},
    {"fs.files", "fs.list(dir), keeping type == \"file\""},
    {"fs.dirs", "fs.list(dir), keeping type == \"dir\""},
    {"fs.entries", "fs.list(dir)"},
    {"os.sleep", "nothing: a hook does not wait"},
    {"os.tmpname", "a path under pkginfo.install_dir() or the hook's own directory"},
    {"system.run_in_script", "system.exec; system.admin_step for a step only an administrator takes"},
    {"system.unix_api", "nothing: remove it"},
    {"log.set_level", "nothing: remove it"},
    {"log.get_level", "nothing: remove it"},
    {"xvm.log_tag", "nothing: remove it"},
    {"elfpatch.auto", "elfpatch.set(opt) or elfpatch.skip()"},
    {"elfpatch.is_auto", "nothing: the host decides"},
    {"json.loadfile", "json.decode(fs.read(path))"},
    {"json.savefile", "fs.write(path, json.encode(value))"},
    {"utils.filepath_to_absolute", "path.join"},
    {"utils.try_download_and_check", "an entry's url and sha256: the host downloads"},
    {"utils.input_args_process", "a lib of the index"},
    {"os.iorun", "system.run(cmd).out; system.exec(cmd, { capture = true }) when its failure should fail the hook"},
    {"os.execute", "system.run(cmd); system.exec(cmd) when its failure should fail the hook"},
    {"os.trymv", "pcall(fs.move, src, dst)"},
    // os.mv and os.cp put src INTO dst when dst is a directory, and replace
    // what is there; fs.move and fs.copy take dst as the final path and
    // refuse one that exists. Which one a call meant is not in its text.
    {"os.mv", "fs.move(src, final_path [, { replace = true }]) -- into a directory: path.join(dir, name)"},
    {"os.cp", "fs.copy(src, final_path [, { replace = true }]) -- into a directory: path.join(dir, name)"},
    {"os.rename", "fs.move(src, final_path [, { replace = true }])"},
    {"is_host", "pkginfo.platform() == \"<platform>\""},
};

// The function `function <name>()` at the top level: [start of its line, end of its `end`).
std::optional<std::pair<std::size_t, std::size_t>> function_span(std::string_view code, std::string_view name) {
    const auto head = std::string("function ") + std::string(name) + "(";
    std::size_t at = std::string_view::npos;
    for (auto p = code.find(head); p != std::string_view::npos; p = code.find(head, p + 1)) {
        if (p > 0 && code[p - 1] != '\n') continue;
        at = p;
        break;
    }
    if (at == std::string_view::npos) return std::nullopt;
    int depth = 0;
    std::size_t i = at;
    while (i < code.size()) {
        if (!name_char(code[i])) { ++i; continue; }
        std::size_t j = i;
        while (j < code.size() && name_char(code[j])) ++j;
        const auto word = code.substr(i, j - i);
        if (word == "function" || word == "if" || word == "do" || word == "repeat") ++depth;
        else if (word == "end" || word == "until") {
            if (--depth == 0) {
                std::size_t stop = j;
                if (stop < code.size() && code[stop] == '\n') ++stop;
                return std::pair{at, stop};
            }
        }
        i = j;
    }
    return std::nullopt;
}

// The lines of [from, to) in text, with their offsets.
std::vector<std::pair<std::size_t, std::string_view>> lines_in(std::string_view text, std::size_t from, std::size_t to) {
    std::vector<std::pair<std::size_t, std::string_view>> out;
    std::size_t at = from;
    while (at < to) {
        auto nl = text.find('\n', at);
        if (nl == std::string_view::npos || nl > to) nl = to;
        out.emplace_back(at, text.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

// The statements of a function's body, from blanked code (strings and
// comments are spaces): each with its span (whole lines) and its text without
// spaces. A line continues into the next when the next starts with `:` or
// `.`, or this one ends with `(`, `,`, `..`, `=`, `and` or `or`.
struct Statement { std::size_t from, to; std::string code; };

std::vector<Statement> statements(std::string_view code, std::size_t from, std::size_t to) {
    std::vector<Statement> out;
    const auto compact = [](std::string_view s) {
        std::string r;
        for (const char c : s) if (c != ' ' && c != '\t' && c != '\r') r += c;
        return r;
    };
    bool join = false;
    for (const auto& [off, line] : lines_in(code, from, to)) {
        const auto c = compact(line);
        const auto end = std::min(off + line.size() + 1, to);
        if (c.empty()) continue;
        if (join || (!out.empty() && (c.starts_with(':') || (c.starts_with('.') && !c.starts_with(".."))))) {
            out.back().code += c;
            out.back().to = end;
        } else {
            out.push_back({ off, end, c });
        }
        const auto& last = out.back().code;
        join = last.ends_with('(') || last.ends_with(',') || last.ends_with("..") || last.ends_with('=')
            || last.ends_with("and") || last.ends_with("or");
    }
    return out;
}

bool landing_source(std::string_view s) {
    // pkginfo.install_file():replace(...), or :gsub(...), in blanked code.
    return s.starts_with("pkginfo.install_file()")
        && (s.find(":replace(") != std::string_view::npos || s.find(":gsub(") != std::string_view::npos);
}

// A directory the archive extracted into, named relative to the hook's
// working directory: string literals (blank in code), `..`, the version and
// the name -- `"libbsd-" .. pkginfo.version()`.
bool relative_extracted(std::string_view s) {
    std::string rest(s);
    for (const auto* part : {"pkginfo.version()", "pkginfo.name()", "package.name", ".."}) {
        for (auto at = rest.find(part); at != std::string::npos; at = rest.find(part)) rest.erase(at, std::strlen(part));
    }
    return rest.empty();
}

// The landing of an install(): the statements that find the extracted
// directory, remove the install directory and move the one into the other.
struct Landing {
    std::pair<std::size_t, std::size_t> function;   // the whole install()
    std::vector<std::pair<std::size_t, std::size_t>> remove;  // its landing statements
    bool whole = false;                              // nothing else is left in it
};

std::optional<Landing> landing_install(std::string_view code, const std::vector<std::string>& landing_calls) {
    const auto span = function_span(code, "install");
    if (!span) return std::nullopt;
    const auto body = statements(code, span->first, span->second);
    if (body.size() < 3) return std::nullopt;
    Landing l { .function = *span };
    std::set<std::string> sources, dirs{"pkginfo.install_dir()"};
    bool removed = false, moved = false, other = false;
    // `name` as a whole word of compact code.
    const auto names = [](std::string_view c, const std::string& name) {
        for (auto at = c.find(name); at != std::string_view::npos; at = c.find(name, at + 1)) {
            const bool before = at == 0 || !name_char(c[at - 1]);
            const bool after = at + name.size() >= c.size() || !name_char(c[at + name.size()]);
            if (before && after) return true;
        }
        return false;
    };
    const auto names_source = [&](std::string_view c) {
        return std::ranges::any_of(sources, [&](const std::string& v) { return names(c, v); });
    };
    const auto is_tryrm = [&](const std::string& c) {
        return c.starts_with("os.tryrm(") && c.ends_with(")") && dirs.contains(c.substr(9, c.size() - 10));
    };
    const auto is_move = [&](const std::string& c) {
        if (!c.starts_with("os.mv(") || !c.ends_with(")")) return false;
        const auto inner = c.substr(6, c.size() - 7);
        const auto comma = inner.rfind(',');
        if (comma == std::string::npos || !dirs.contains(inner.substr(comma + 1))) return false;
        const auto src = inner.substr(0, comma);
        return sources.contains(src) || landing_source(src) || relative_extracted(src);
    };
    // A test of the extracted directory alone: `[not] os.isdir(src)`,
    // `fs.is_dir(src)` or `os.exists(src)`, in compact code.
    const auto directory_test = [&](std::string_view cond) {
        if (cond.starts_with("not")) cond.remove_prefix(3);
        for (const std::string_view f : {"os.isdir(", "fs.is_dir(", "os.exists("}) {
            if (cond.starts_with(f) && cond.ends_with(")"))
                return sources.contains(std::string(cond.substr(f.size(), cond.size() - f.size() - 1)));
        }
        return false;
    };
    std::set<std::size_t> taken;   // statements removed as a guard's
    for (std::size_t k = 1; k + 1 < body.size(); ++k) {
        if (taken.contains(k)) continue;
        const auto& st = body[k];
        const auto& c = st.code;
        // A guard on the extracted directory -- `if os.isdir(src) then` the
        // landing `end`, or `if not ... then raise(...) end` -- goes with it:
        // under the host's landing the directory is never there to test.
        if (c.starts_with("if") && c.ends_with("then") && names_source(c)) {
            // A condition that also tests a version or a platform decides more
            // than where the directory is: for a person.
            if (!directory_test(std::string_view(c).substr(2, c.size() - 6))) return std::nullopt;
            int depth = 1;
            std::size_t j = k + 1;
            bool inner_ok = true;
            for (; j + 1 < body.size(); ++j) {
                const auto& ic = body[j].code;
                if (ic == "end") { if (--depth == 0) break; continue; }
                if (ic.ends_with("then") || ic.ends_with("do")) { ++depth; inner_ok = false; continue; }
                if (is_tryrm(ic)) { removed = true; continue; }
                if (is_move(ic)) { moved = true; continue; }
                if (ic.starts_with("raise(") || ic.starts_with("error(")) continue;
                inner_ok = false;
            }
            if (depth != 0 || !inner_ok) return std::nullopt;
            for (std::size_t m = k; m <= j; ++m) { taken.insert(m); l.remove.emplace_back(body[m].from, body[m].to); }
            continue;
        }
        if (c.starts_with("local") && c.find('=') != std::string::npos && c.find(',') > c.find('=')) {
            const auto eq = c.find('=');
            const auto var = c.substr(5, eq - 5);
            const auto value = std::string_view(c).substr(eq + 1);
            if (value == "pkginfo.install_dir()") { dirs.insert(var); continue; }
            if (landing_source(value)) { sources.insert(var); l.remove.emplace_back(st.from, st.to); continue; }
            // A value read from the download or the extracted directory: a
            // use of it this does not rewrite (the landing would take the
            // variable away), for a person.
            if (value.find("install_file()") != std::string_view::npos || names_source(value)) return std::nullopt;
            other = true;
            continue;
        }
        if (is_tryrm(c)) {
            removed = true;
            l.remove.emplace_back(st.from, st.to);
            continue;
        }
        if (c.starts_with("os.mv(") && c.ends_with(")")) {
            if (!is_move(c)) return std::nullopt;
            moved = true;
            l.remove.emplace_back(st.from, st.to);
            continue;
        }
        // A lib function the index names as a landing: the whole of it, when
        // it is called alone -- arguments could name another source or
        // destination than the host's landing, which is for a person to read.
        if (std::ranges::any_of(landing_calls, [&](const std::string& f) { return c.starts_with(f + "("); })) {
            if (!std::ranges::any_of(landing_calls, [&](const std::string& f) { return c == f + "()"; }))
                return std::nullopt;
            removed = moved = true;
            l.remove.emplace_back(st.from, st.to);
            continue;
        }
        if (c == "returntrue") continue;
        // Anything else that looks for the download or the extracted
        // directory is a landing this does not know.
        if (c.find("install_file()") != std::string::npos || names_source(c)) return std::nullopt;
        other = true;
    }
    if (!removed || !moved) return std::nullopt;
    l.whole = !other;
    return l;
}

// The spans of the recipe's functions: `function name(`, `local function name(`.
std::vector<std::pair<std::size_t, std::size_t>> function_spans(std::string_view code) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (auto at = code.find("function "); at != std::string_view::npos; at = code.find("function ", at + 1)) {
        if (at > 0 && name_char(code[at - 1])) continue;
        auto bol = code.rfind('\n', at);
        bol = bol == std::string_view::npos ? 0 : bol + 1;
        const auto before = trim(code.substr(bol, at - bol));
        if (!before.empty() && before != "local") continue;
        const auto paren = code.find('(', at);
        if (paren == std::string_view::npos) continue;
        const auto name = trim(code.substr(at + 9, paren - at - 9));
        if (name.empty() || std::ranges::any_of(name, [](char c) { return !name_char(c) && c != '.' && c != ':'; })) continue;
        int depth = 0;
        for (std::size_t i = at; i < code.size();) {
            if (!name_char(code[i])) { ++i; continue; }
            std::size_t j = i;
            while (j < code.size() && name_char(code[j])) ++j;
            const auto word = code.substr(i, j - i);
            if (word == "function" || word == "if" || word == "do" || word == "repeat") ++depth;
            else if ((word == "end" || word == "until") && --depth == 0) { out.emplace_back(at, j); break; }
            i = j;
        }
    }
    return out;
}

// The tables of xpm's platforms: the offset just after each `<platform> = {`
// line's end, and the indentation of the line after it; or the platforms
// whose value is not a table literal.
struct PlatformTables {
    std::vector<std::pair<std::size_t, std::string>> insert_at;
    std::vector<std::string> not_literal;
};

PlatformTables platform_tables(std::string_view code) {
    PlatformTables out;
    auto xpm = code.find("xpm");
    while (xpm != std::string_view::npos && ((xpm > 0 && name_char(code[xpm - 1])) || (xpm + 3 < code.size() && name_char(code[xpm + 3]))))
        xpm = code.find("xpm", xpm + 1);
    if (xpm == std::string_view::npos) return out;
    const auto open = code.find('{', xpm);
    if (open == std::string_view::npos) return out;
    const auto close = closing(code, open);
    if (close == std::string_view::npos) return out;
    int depth = 0;
    for (std::size_t i = open + 1; i < close; ++i) {
        const char c = code[i];
        if (c == '{' || c == '(' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ')' || c == ']') { --depth; continue; }
        if (depth != 0 || !name_char(c) || name_char(code[i - 1])) continue;
        std::size_t j = i;
        while (j < close && name_char(code[j])) ++j;
        const auto key = code.substr(i, j - i);
        std::size_t k = j;
        while (k < close && (code[k] == ' ' || code[k] == '\t')) ++k;
        if (k >= close || code[k] != '=' || (key != "linux" && key != "macosx" && key != "windows")) { i = j - 1; continue; }
        ++k;
        while (k < close && (code[k] == ' ' || code[k] == '\t')) ++k;
        if (code[k] != '{') { out.not_literal.emplace_back(key); i = j - 1; continue; }
        auto eol = code.find('\n', k);
        if (eol == std::string_view::npos || eol > close) { out.not_literal.emplace_back(key); i = j - 1; continue; }
        // The indentation of the table's first line of code (a comment line is blank here).
        std::string indent;
        for (auto line = eol + 1; line < close;) {
            const auto next = code.find_first_not_of(" \t", line);
            if (next == std::string_view::npos || next >= close) break;
            if (code[next] != '\n') { indent = std::string(code.substr(line, next - line)); break; }
            line = next + 1;
        }
        out.insert_at.emplace_back(eol + 1, indent);
        i = j - 1;
    }
    return out;
}

// The library's modules a recipe imports.
std::set<std::string> imported(std::string_view text) {
    std::set<std::string> out;
    const std::string_view head = "import(\"xim.libxpkg.";
    for (auto at = text.find(head); at != std::string_view::npos; at = text.find(head, at + 1)) {
        const auto from = at + head.size();
        const auto end = text.find('"', from);
        if (end != std::string_view::npos) out.insert(std::string(text.substr(from, end - from)));
    }
    return out;
}

} // namespace mcpplibs::xpkg::migrate_detail

namespace mcpplibs::xpkg {

Migration migrate_recipe(std::string_view source, MigrateOptions options) {
    using namespace migrate_detail;
    Migration m;
    std::string text(source);
    std::string code = recipe_code(text);
    std::set<std::string> need;
    std::vector<Edit> edits;
    const auto note = [&](std::size_t at, std::string_view name, std::string_view what) {
        m.left.push_back({ .line = line_of(code, at), .name = std::string(name), .what = std::string(what) });
    };
    const auto taken = [&](std::size_t from, std::size_t to) {
        return std::ranges::any_of(edits, [&](const Edit& e) { return from < e.to && e.from < to; });
    };

    // 1. The landing install().
    std::optional<Landing> landing = options.lib ? std::nullopt : landing_install(code, options.landing_calls);
    std::vector<std::pair<std::size_t, std::size_t>> gone;   // removed spans
    if (landing) {
        const auto tables = platform_tables(code);
        if (!tables.not_literal.empty() || tables.insert_at.empty()) {
            note(landing->function.first, "install()", "the host lands the download: delete the landing in install() and "
                 "write `strip = 1` on each platform's table (a platform's versions are a shared table here)");
            landing.reset();
        } else {
            if (landing->whole) gone.push_back(landing->function);
            else gone = landing->remove;
            for (const auto& [from, to] : gone) edits.push_back({ from, to, "" });
            for (const auto& [at, indent] : tables.insert_at) edits.push_back({ at, at, indent + "strip = 1,\n" });
            m.landing_removed = true;
        }
    }

    // 2. The calls.
    const auto in_landing = [&](std::size_t at) {
        return std::ranges::any_of(gone, [&](const auto& g) { return at >= g.first && at < g.second; });
    };
    for (const auto& r : kRenames) {
        if (r.old_name.find('.') == std::string_view::npos && defines(code, r.old_name)) continue;
        for (const auto at : uses(code, r.old_name)) {
            if (in_landing(at)) continue;
            const auto after = at + r.old_name.size();
            const auto paren = code.find_first_not_of(" ", after);
            // A bare global (format, raise, cprint) is the layer's only where it is called.
            if (r.old_name.find('.') == std::string_view::npos && (paren == std::string::npos || code[paren] != '(')) continue;
            edits.push_back({ at, after, std::string(r.new_name) });
            if (!r.module.empty()) need.insert(std::string(r.module));
            ++m.rewritten;
        }
    }
    // os.trymv(a, b) -> pcall(fs.move, a, b); os.iorun(x) -> system.run(x).out (both never
    // fail: os.iorun answers a failed command with its output, as a query does);
    // os.execute(x) as a statement -> system.run(x) (its status was not read);
    // is_host("p") -> pkginfo.platform() == "p".
    // The call's own parenthesis, right after the name; npos when the name is
    // used as a value (pcall(os.iorun, cmd), type(os.iorun)).
    const auto call_paren = [&](std::size_t at, std::string_view name) {
        const auto p = code.find_first_not_of(" \t", at + name.size());
        return p != std::string::npos && code[p] == '(' ? p : std::string::npos;
    };
    for (const auto at : uses(code, "os.trymv")) {
        if (in_landing(at)) continue;
        const auto open = call_paren(at, "os.trymv");
        if (open == std::string::npos || !is_statement(text, code, at)) { note(at, "os.trymv", "pcall(fs.move, src, dst)"); continue; }
        edits.push_back({ at, open + 1, "pcall(fs.move, " });
        need.insert("fs");
        ++m.rewritten;
    }
    for (const auto at : uses(code, "os.iorun")) {
        if (in_landing(at)) continue;
        const auto open = call_paren(at, "os.iorun");
        const auto close = open == std::string::npos ? open : closing(code, open);
        if (close == std::string::npos || !top_commas(code, open, close).empty()) { note(at, "os.iorun", "system.run(cmd).out"); continue; }
        edits.push_back({ at, open + 1, "system.run(" });
        // As a statement its output was dropped, and `x.out` alone is no Lua
        // statement: system.run(x), like os.execute's.
        if (!is_statement(text, code, at)) edits.push_back({ close + 1, close + 1, ".out" });
        need.insert("system");
        ++m.rewritten;
    }
    for (const auto at : uses(code, "os.execute")) {
        if (in_landing(at)) continue;
        if (!is_statement(text, code, at) || call_paren(at, "os.execute") == std::string::npos) {
            note(at, "os.execute", "system.run(cmd) where its result is used");
            continue;
        }
        edits.push_back({ at, at + std::string_view("os.execute").size(), "system.run" });
        need.insert("system");
        ++m.rewritten;
    }
    if (!defines(code, "is_host")) {
        for (const auto at : uses(code, "is_host")) {
            const auto open = code.find_first_not_of(" ", at + 7);
            if (open == std::string::npos || code[open] != '(') {
                note(at, "is_host", "pkginfo.platform() == \"<platform>\"");
                continue;
            }
            const auto close = closing(code, open);
            if (close == std::string::npos || !top_commas(code, open, close).empty()) { note(at, "is_host", "pkginfo.platform() == \"<platform>\", one per platform"); continue; }
            const auto arg = trim(std::string_view(text).substr(open + 1, close - open - 1));
            edits.push_back({ at, close + 1, "(pkginfo.platform() == " + arg + ")" });
            need.insert("pkginfo");
            ++m.rewritten;
        }
    }
    // Commands that run chmod, tar, unzip or sudo.
    {
        const std::array<std::pair<std::string_view, std::string_view>, 4> commands{{
            {"\"chmod ", "fs.chmod(path, mode)"},
            {"\"tar ", "the host lands the download (strip); fs.extract for an archive inside it"},
            {"\"unzip ", "the host lands the download (strip); fs.extract for an archive inside it"},
            {"sudo ", "system.admin_step(argv, why): the host asks once"},
        }};
        for (const auto& [needle, what] : commands)
            for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
                if (code[at] != ' ' || in_landing(at)) continue;   // in a string or a comment? only strings matter
                // `code` blanks strings and comments alike; a comment has `--` before it on its line.
                const auto bol = text.rfind('\n', at);
                const auto line_text = std::string_view(text).substr(bol == std::string::npos ? 0 : bol + 1, at - (bol == std::string::npos ? 0 : bol + 1));
                if (line_text.find("--") != std::string_view::npos) continue;
                note(at, trim(needle).substr(trim(needle).starts_with("\"") ? 1 : 0), what);
            }
    }
    // The names no rewrite keeps the meaning of, where they are left.
    for (const auto& [name, what] : kManual) {
        if (name == "os.iorun" || name == "os.execute" || name == "os.trymv" || name == "is_host") continue;
        if (name.find('.') == std::string_view::npos && defines(code, name)) continue;
        for (const auto at : uses(code, name)) {
            if (in_landing(at) || taken(at, at + name.size())) continue;
            if (name.find('.') == std::string_view::npos) {
                const auto p = code.find_first_not_of(" ", at + name.size());
                if (p == std::string::npos || (code[p] != '(' && code[p] != '{')) continue;
            }
            note(at, name, what);
        }
    }
    // An install() that still lands the download itself: in spec 3 the host
    // has placed it before install() runs, so this code would undo that.
    if (!landing && !options.lib) {
        if (const auto span = function_span(code, "install")) {
            const std::array<std::string_view, 3> landing_marks{"install_file()", "os.tryrm(pkginfo.install_dir())", "os.mv("};
            for (const auto mark : landing_marks) {
                const auto at = code.find(mark, span->first);
                if (at == std::string::npos || at >= span->second) continue;
                note(at, "install()", "spec 3 lands the download before install(): delete the code that finds, moves or "
                     "extracts it, write `strip` on the entries, keep what only adjusts the payload");
                break;
            }
        }
    }
    // The top level calls only pure names in spec 3: it also runs when the
    // recipe is read and nothing is installed.
    {
        const auto spans = function_spans(code);
        const auto in_function = [&](std::size_t at) {
            return std::ranges::any_of(spans, [&](const auto& s) { return at >= s.first && at < s.second; });
        };
        const std::array<std::string_view, 9> calls{"pkginfo.", "system.", "fs.", "xvm.", "subos.", "pkgmanager.",
                                                    "elfpatch.", "io.", "os."};
        for (const auto prefix : calls) {
            for (const auto at : uses(code, prefix.substr(0, prefix.size() - 1))) {
                if (in_function(at) || at + prefix.size() > code.size() || code[at + prefix.size() - 1] != '.') continue;
                std::size_t e = at + prefix.size();
                while (e < code.size() && name_char(code[e])) ++e;
                const auto name = code.substr(at, e - at);
                if (name == "os.time" || name == "os.date" || name == "os.clock" || name == "os.getenv") continue;
                note(at, name, "the top level calls only pure names in spec 3: move this into a hook, or state it in "
                     "the entry (sha256 per architecture, arch_alias)");
            }
        }
    }

    // The global path: imported.
    const auto has = imported(text);
    if (!has.contains("path"))
        for (const auto at : uses(code, "path"))
            if (at + 4 < code.size() && code[at + 4] == '.' && !defines(code, "path")) { need.insert("path"); break; }

    // 3. Apply the edits, last first.
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.from > b.from || (a.from == b.from && a.to > b.to); });
    for (const auto& e : edits) text.replace(e.from, e.to - e.from, e.with);

    // 4. Imports.
    for (const auto& mod : need)
        if (!has.contains(mod)) m.imports_added.push_back(mod);
    if (!m.imports_added.empty()) {
        std::string lines;
        for (const auto& mod : m.imports_added) lines += "import(\"xim.libxpkg." + mod + "\")\n";
        const auto last = text.rfind("\nimport(\"xim.libxpkg.");
        std::size_t at;
        if (last != std::string::npos) {
            at = text.find('\n', last + 1);
            at = at == std::string::npos ? text.size() : at + 1;
        } else {
            const auto fn = text.find("\nfunction ");
            at = fn == std::string::npos ? text.size() : fn + 1;
            lines += "\n";
        }
        text.insert(at, lines);
    }

    // 5. spec "3" when nothing is left.
    std::ranges::sort(m.left, [](const MigrationNote& a, const MigrationNote& b) {
        return std::tie(a.line, a.name) < std::tie(b.line, b.name);
    });
    const auto dup = std::ranges::unique(m.left, [](const MigrationNote& a, const MigrationNote& b) {
        return a.line == b.line && a.name == b.name;
    });
    m.left.erase(dup.begin(), dup.end());
    if (m.left.empty() && !options.lib) {
        const auto rcode = recipe_code(text);
        const auto declared = declared_spec(text);
        bool done = false;
        if (declared) {
            for (auto at = rcode.find("spec"); at != std::string::npos; at = rcode.find("spec", at + 1)) {
                if ((at > 0 && name_char(rcode[at - 1])) || (at + 4 < rcode.size() && name_char(rcode[at + 4]))) continue;
                const auto q = text.find('"', at);
                const auto q2 = q == std::string::npos ? q : text.find('"', q + 1);
                if (q2 == std::string::npos) break;
                text.replace(q, q2 - q + 1, "\"3\"");
                done = true;
                break;
            }
        } else if (const auto pkg = rcode.find("package"); pkg != std::string::npos) {
            const auto open = rcode.find('{', pkg);
            const auto eol = open == std::string::npos ? open : text.find('\n', open);
            if (eol != std::string::npos) {
                const auto next = text.find_first_not_of(" \t", eol + 1);
                text.insert(eol + 1, text.substr(eol + 1, next - eol - 1) + "spec = \"3\",\n");
                done = true;
            }
        }
        m.spec3 = done;
    }
    m.text = std::move(text);
    return m;
}

} // namespace mcpplibs::xpkg
