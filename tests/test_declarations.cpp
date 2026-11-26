// What an entry and an index declare, in place of code a hook or a build
// script ran (design 2026-11-27 §9.7, §9.8): `strip`, how the host lands a
// download; `recipes.template`, the hooks an index shares between recipes.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Index {
    fs::path root = fs::temp_directory_path() / ("xpkg-decl-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Index() { fs::create_directories(root / "pkgs" / "p"); }
    ~Index() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path write(const fs::path& rel, std::string_view text) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
        return root / rel;
    }
};

} // namespace

// ---- strip ----

TEST(StripTest, AnEntryOrItsPlatformStatesIt) {
    Index index;
    const auto recipe = index.write("pkgs/p/probe.lua", R"LUA(
package = { spec = "3", name = "probe", xpm = {
    linux = {
        strip = 1,
        ["1.0.0"] = { url = "https://ex/a-1.0.0.tar.gz", sha256 = { x86_64 = "aa" } },
        ["2.0.0"] = { url = "https://ex/a-2.0.0.tar.gz", sha256 = { x86_64 = "bb" }, strip = 0 },
        ["latest"] = { ref = "1.0.0" },
    },
    ubuntu = { inherits = "linux" },
    macosx = { ["1.0.0"] = { url = "https://ex/a.zip", sha256 = "cc" } },
} }
)LUA");
    auto pkg = load_package(recipe);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_TRUE(pkg->warnings.empty());
    EXPECT_FALSE(pkg->xpm.entries.at("linux").contains("strip")) << "strip is not a version";
    auto at = [&](std::string platform, std::string version) {
        auto r = resolve_resource(pkg->xpm, {.name = "probe", .version = version,
                                             .platform = platform, .arch = "x86_64"});
        EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
        return r ? r->strip : -1;
    };
    EXPECT_EQ(at("linux", "1.0.0"), 1);    // the platform's
    EXPECT_EQ(at("linux", "2.0.0"), 0);    // the entry's own wins
    EXPECT_EQ(at("linux", "latest"), 1);   // through the alias
    EXPECT_EQ(at("ubuntu", "1.0.0"), 1);   // inherited with the platform
    EXPECT_EQ(at("macosx", "1.0.0"), 0);   // none stated
}

TEST(StripTest, AValueThatIsNotAComponentCountIsDroppedWithAWarning) {
    Index index;
    const auto recipe = index.write("pkgs/p/probe.lua", R"LUA(
package = { spec = "2", name = "probe", xpm = { linux = {
    ["1.0.0"] = { url = "https://ex/a.tar.gz", sha256 = "aa", strip = -1 },
    ["2.0.0"] = { url = "https://ex/b.tar.gz", sha256 = "bb", strip = "1" },
} } }
)LUA");
    auto pkg = load_package(recipe);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    ASSERT_EQ(pkg->warnings.size(), 2u);
    EXPECT_NE(pkg->warnings[0].find(".strip: not a whole number of path components"), std::string::npos);
    EXPECT_EQ(pkg->xpm.entries.at("linux").at("1.0.0").strip, -1);
}

// ---- recipes.template ----

namespace {

// A sub-index of today: a template every recipe shares, and the build script
// that appended it to every file.
void sub_index(const Index& index, bool declared) {
    index.write("pkgs/p/probe.lua", "package = { name = \"probe\", xpm = { linux = { [\"latest\"] = {} } } }\n");
    index.write("template.lua", R"LUA(
package.namespace = "scode"
package.type = "package"
function install() return true end
function uninstall() return true end
)LUA");
    index.write("pkgindex-build.lua", R"LUA(
package = { name = "pkgindex-update", xpm = { linux = { ["latest"] = {} } } }
function install()
    local root = os.scriptdir()
    local t = io.readfile(root .. "/template.lua")
    local f = root .. "/pkgs/p/probe.lua"
    io.writefile(f, io.readfile(f) .. t)
    return true
end
)LUA");
    if (declared) index.write("index-compat.json", R"({ "recipes": { "template": "template.lua" } })");
}

} // namespace

TEST(IndexTemplateTest, ADeclaredTemplateIsReadWithEveryRecipeAndNoFileIsRewritten) {
    Index index;
    sub_index(index, true);
    const auto before = read(index.root / "pkgs" / "p" / "probe.lua");
    auto built = build_index(index.root, "scode");
    ASSERT_TRUE(built.has_value()) << built.error();
    EXPECT_EQ(read(index.root / "pkgs" / "p" / "probe.lua"), before) << "the build script did not run";
    auto pkg = load_package(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_EQ(pkg->namespace_, "scode") << "the template's top level ran after the recipe";
    auto exec = create_executor(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(exec.has_value()) << exec.error();
    EXPECT_TRUE(exec->has_hook(HookType::Install));
    EXPECT_TRUE(exec->has_hook(HookType::Uninstall));
    auto text = recipe_text(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(text.has_value()) << text.error();
    EXPECT_EQ(*text, before + read(index.root / "template.lua"));
}

// Without the declaration nothing changes: the build script runs.
TEST(IndexTemplateTest, WithoutTheDeclarationTheBuildScriptRuns) {
    Index index;
    sub_index(index, false);
    auto built = build_index(index.root, "scode");
    ASSERT_TRUE(built.has_value()) << built.error();
    EXPECT_NE(read(index.root / "pkgs" / "p" / "probe.lua").find("package.namespace = \"scode\""), std::string::npos);
    auto text = recipe_text(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, read(index.root / "pkgs" / "p" / "probe.lua"));
}

TEST(IndexTemplateTest, ATemplateThatIsNotAFileOfTheIndexIsAnErrorThatNamesIt) {
    Index index;
    sub_index(index, false);
    index.write("index-compat.json", R"({ "recipes": { "template": "../outside.lua" } })");
    auto pkg = load_package(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("recipes.template names ../outside.lua, which is not a file of the index"), std::string::npos)
        << pkg.error();
    index.write("index-compat.json", R"({ "recipes": { "template": "absent.lua" } })");
    pkg = load_package(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("which does not exist"), std::string::npos) << pkg.error();
}

// A recipe of spec "3" and its template are one recipe: the template runs in
// the same environment, under the same rules.
TEST(IndexTemplateTest, ARecipeOfSpec3RunsItsTemplateUnderTheSameRules) {
    Index index;
    index.write("index-compat.json", R"({ "recipes": { "template": "template.lua" } })");
    index.write("pkgs/p/probe.lua", "package = { spec = \"3\", name = \"probe\", xpm = { linux = { [\"latest\"] = {} } } }\n");
    index.write("template.lua", R"LUA(
import("xim.libxpkg.xvm")
package.namespace = "sub"
function config() xvm.add(package.name) return true end
)LUA");
    auto pkg = load_package(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_EQ(pkg->namespace_, "sub");
    auto exec = create_executor(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_TRUE(exec.has_value()) << exec.error();
    EXPECT_TRUE(exec->has_hook(HookType::Config));

    // A template with the compatibility layer's names is not a spec 3 template.
    index.write("template.lua", "package.namespace = os.host()\n");
    pkg = load_package(index.root / "pkgs" / "p" / "probe.lua");
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("host"), std::string::npos) << pkg.error();
}
