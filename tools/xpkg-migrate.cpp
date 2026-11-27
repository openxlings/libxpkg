// xpkg-migrate: rewrite recipes of spec "1" or "2" into spec "3" (design
// 2026-11-27 §9.6). Built with `mcpp build --features tools`.
//
//   xpkg-migrate [--write] [--summary] <recipe.lua | index dir> ...
//
// An index directory brings its libs/ along; a file in a libs/ directory is a
// lib (its names and imports rewritten, nothing else).
//
// Without --write it changes nothing and prints, per recipe, what it would
// rewrite and what is left for a person; with --write it rewrites the files.
// --summary prints only the counts.
import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.migrate;

namespace fs = std::filesystem;
using namespace mcpplibs::xpkg;

int main(int argc, char** argv) {
    bool write = false, summary = false;
    std::vector<fs::path> recipes;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--write") { write = true; continue; }
        if (a == "--summary") { summary = true; continue; }
        if (a == "-h" || a == "--help") {
            std::println("usage: xpkg-migrate [--write] [--summary] <recipe.lua | index dir> ...");
            return 0;
        }
        const fs::path p(a);
        if (fs::is_directory(p)) {
            const auto pkgs = fs::is_directory(p / "pkgs") ? p / "pkgs" : p;
            for (const auto& e : fs::recursive_directory_iterator(pkgs))
                if (e.is_regular_file() && e.path().extension() == ".lua") recipes.push_back(e.path());
            if (pkgs != p && fs::is_directory(p / "libs"))
                for (const auto& e : fs::directory_iterator(p / "libs"))
                    if (e.is_regular_file() && e.path().extension() == ".lua") recipes.push_back(e.path());
        } else {
            recipes.push_back(p);
        }
    }
    std::ranges::sort(recipes);
    int full = 0, partly = 0, untouched = 0, already = 0, landed = 0;
    std::map<std::string, int> reasons;
    for (const auto& r : recipes) {
        std::ifstream in(r, std::ios::binary);
        if (!in) { std::println(std::cerr, "{}: cannot be read", r.string()); return 2; }
        std::ostringstream ss;
        ss << in.rdbuf();
        const auto text = std::move(ss).str();
        if (declared_spec(text) == "3") { ++already; continue; }
        // A file of an index's libs/ is a lib: names and imports only.
        const bool lib = r.parent_path().filename() == "libs";
        const auto m = migrate_recipe(text, { .lib = lib });
        if (m.landing_removed) ++landed;
        if (m.spec3) ++full;
        else if (m.text != text) ++partly;
        else ++untouched;
        for (const auto& n : m.left) ++reasons[n.name];
        if (!summary && (m.text != text || !m.left.empty())) {
            std::println("{}: {}{} call(s) rewritten{}{}", r.string(), m.spec3 ? "spec 3, " : "",
                         m.rewritten, m.landing_removed ? ", landing install() replaced by strip = 1" : "",
                         m.imports_added.empty() ? "" : ", imports added");
            for (const auto& n : m.left) std::println("  line {}: {} -> {}", n.line, n.name, n.what);
        }
        if (write && m.text != text) std::ofstream(r, std::ios::binary) << m.text;
    }
    std::println("{} recipe(s): {} migrated to spec 3, {} partly (left for a person), {} unchanged, {} already spec 3; "
                 "{} landing install() replaced", recipes.size(), full, partly, untouched, already, landed);
    std::vector<std::pair<int, std::string>> top;
    for (const auto& [name, count] : reasons) top.emplace_back(count, name);
    std::ranges::sort(top, std::greater{});
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 12); ++i)
        std::println("  left: {} x{}", top[i].second, top[i].first);
    return 0;
}
