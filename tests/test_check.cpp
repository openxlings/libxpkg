// The format's checker (design 2026-11-27 §9.9): each rule finds what it is
// for, and the template of the format finds nothing.
#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.check;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

struct Index {
    fs::path root = fs::temp_directory_path() / ("xpkg-check-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Index() { fs::create_directories(root / "pkgs" / "p"); }
    ~Index() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path recipe(std::string_view text) const {
        auto file = root / "pkgs" / "p" / "probe.lua";
        std::ofstream(file, std::ios::binary) << text;
        return file;
    }
};

std::vector<std::string> codes(const std::vector<Finding>& findings) {
    std::vector<std::string> out;
    for (const auto& f : findings) out.push_back(f.code + ":" + f.severity + ":" + std::to_string(f.line));
    return out;
}

const Finding* find(const std::vector<Finding>& findings, std::string_view code) {
    for (const auto& f : findings) if (f.code == code) return &f;
    return nullptr;
}

// The shape a new recipe has: what the checker must accept as it is.
constexpr std::string_view kTemplate = R"LUA(package = {
    spec = "3",
    name = "probe",
    description = "a probe",
    type = "package",
    archs = { "x86_64", "aarch64" },
    xpm = {
        linux = {
            strip = 1,
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://example.com/probe-1.0.0-linux-${arch}.tar.gz",
                sha256 = { x86_64 = "aa", aarch64 = "bb" },
            },
        },
    },
}

import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

-- os.mv in a comment, and "is_host" in a string, are not code.
local note = "is_host(x) and os.mv(a, b)"

function install()
    fs.chmod(path.join(pkginfo.install_dir(), "bin", "probe"), "+x")
end

function config()
    xvm.add("probe")
end
)LUA";

} // namespace

TEST(CheckTest, TheFormatsTemplateHasNoFinding) {
    Index index;
    auto findings = check_recipe(index.recipe(kTemplate));
    EXPECT_TRUE(findings.empty()) << ::testing::PrintToString(codes(findings));
}

TEST(CheckTest, AnEntryWithOneArtifactAndNoArchitectureIsNamed) {
    Index index;
    std::string text(kTemplate);
    text.replace(text.find("sha256 = { x86_64 = \"aa\", aarch64 = \"bb\" },"),
                 std::string_view("sha256 = { x86_64 = \"aa\", aarch64 = \"bb\" },").size(), "sha256 = \"aa\",");
    text.replace(text.find("linux-${arch}"), std::string_view("linux-${arch}").size(), "linux-x86_64");
    auto findings = check_recipe(index.recipe(text));
    const auto* f = find(findings, "arch-unstated");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
    EXPECT_EQ(f->severity, "error");
    EXPECT_EQ(f->line, 11);
    EXPECT_NE(f->message.find("xpm.linux[\"1.0.0\"]"), std::string::npos) << f->message;
    // spec 2: the same is a warning (the host still installs it).
    text.replace(text.find("spec = \"3\""), std::string_view("spec = \"3\"").size(), "spec = \"2\"");
    findings = check_recipe(index.recipe(text));
    f = find(findings, "arch-unstated");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->severity, "warning");
}

TEST(CheckTest, PackageArchsIsTheUnionOfTheEntries) {
    Index index;
    std::string text(kTemplate);
    text.replace(text.find("archs = { \"x86_64\", \"aarch64\" },"),
                 std::string_view("archs = { \"x86_64\", \"aarch64\" },").size(), "archs = { \"x86_64\", \"riscv64\" },");
    auto findings = check_recipe(index.recipe(text));
    const auto* f = find(findings, "archs-union");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
    EXPECT_NE(f->message.find("entries list aarch64 and archs does not"), std::string::npos) << f->message;
    EXPECT_NE(f->message.find("archs lists riscv64 and no entry does"), std::string::npos) << f->message;
}

TEST(CheckTest, ACompatibilityNameInTheCodeIsNamedWithWhatReplacesIt) {
    Index index;
    std::string text(kTemplate);
    text.replace(text.find("    fs.chmod("), 0, "    os.mv(pkginfo.install_file(), pkginfo.install_dir())\n"
                                              "    local format = string.format\n"
                                              "    print(format('%d', 1))\n");
    auto findings = check_recipe(index.recipe(text));
    ASSERT_EQ(findings.size(), 1u) << ::testing::PrintToString(codes(findings));
    EXPECT_EQ(findings[0].code, "compat-name");
    EXPECT_EQ(findings[0].severity, "error");
    EXPECT_EQ(findings[0].line, 28);
    EXPECT_NE(findings[0].message.find("os.mv is the compatibility layer's; a recipe of spec 3 writes fs.move"),
              std::string::npos) << findings[0].message;
}

TEST(CheckTest, TheGlobalPathIsTheLayersWhenTheModuleIsNotImported) {
    Index index;
    std::string text(kTemplate);
    text.replace(text.find("import(\"xim.libxpkg.path\")\n"), std::string_view("import(\"xim.libxpkg.path\")\n").size(), "");
    auto findings = check_recipe(index.recipe(text));
    const auto* f = find(findings, "compat-name");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
    EXPECT_NE(f->message.find("path is the compatibility layer's"), std::string::npos) << f->message;
}

TEST(CheckTest, TheSpecIsRead) {
    Index index;
    auto findings = check_recipe(index.recipe("package = { name = \"probe\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"));
    EXPECT_NE(find(findings, "spec-missing"), nullptr) << ::testing::PrintToString(codes(findings));
    findings = check_recipe(index.recipe("package = { spec = \"9\", name = \"probe\", xpm = {} }\n"));
    ASSERT_NE(find(findings, "spec-newer"), nullptr) << ::testing::PrintToString(codes(findings));
    findings = check_recipe(index.recipe("package = { spec = \"2\", name = \"probe\", xpm = {} }\n"));
    ASSERT_NE(find(findings, "spec-old"), nullptr) << ::testing::PrintToString(codes(findings));
    findings = check_recipe(index.recipe("package = {"));
    ASSERT_NE(find(findings, "load"), nullptr) << ::testing::PrintToString(codes(findings));
}

TEST(CheckTest, AStripThatIsNoComponentCountIsAnErrorOfSpec3AndUnreadBefore) {
    Index index;
    std::string text(kTemplate);
    text.replace(text.find("strip = 1,"), std::string_view("strip = 1,").size(), "strip = \"1\",");
    auto findings = check_recipe(index.recipe(text));
    const auto* f = find(findings, "strip");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
    EXPECT_EQ(f->severity, "error");
    text = std::string(kTemplate);
    text.replace(text.find("spec = \"3\""), std::string_view("spec = \"3\"").size(), "spec = \"2\"");
    findings = check_recipe(index.recipe(text));
    f = find(findings, "strip");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
    EXPECT_EQ(f->severity, "warning");
}

TEST(CheckTest, BuildIsNoHookOfSpec3) {
    Index index;
    std::string text(kTemplate);
    text += "function build() end\n";
    auto findings = check_recipe(index.recipe(text));
    const auto* f = find(findings, "build-hook");
    ASSERT_NE(f, nullptr) << ::testing::PrintToString(codes(findings));
}

TEST(CheckTest, AnIndexIsCheckedRecipeByRecipeAndItsDeclarations) {
    Index index;
    index.recipe(kTemplate);
    std::ofstream(index.root / "index-compat.json") << R"({ "recipes": { "template": "absent.lua" } })";
    auto all = check_index(index.root);
    ASSERT_TRUE(all.contains(""));
    EXPECT_NE(all.at("")[0].message.find("which does not exist"), std::string::npos);
}

// Not a unit test: the rules over a real index, for measuring them
// (XPKG_CHECK_INDEX=<index>; --gtest_also_run_disabled_tests; each finding
// too with XPKG_CHECK_VERBOSE=1).
TEST(CheckTest, DISABLED_ARealIndex) {
    const char* root = std::getenv("XPKG_CHECK_INDEX");
    if (!root) GTEST_SKIP() << "XPKG_CHECK_INDEX names the index";
    const bool verbose = std::getenv("XPKG_CHECK_VERBOSE") != nullptr;
    std::map<std::string, int> counts;
    std::map<std::string, std::set<std::string>> recipes;
    for (const auto& [file, findings] : check_index(root)) {
        for (const auto& f : findings) {
            ++counts[f.code + " " + f.severity];
            recipes[f.code + " " + f.severity].insert(file);
            if (verbose) std::println("{}:{}: {} {}: {}", file, f.line, f.severity, f.code, f.message);
        }
    }
    for (const auto& [k, n] : counts) std::println("{:28} {:5} findings in {:4} recipes", k, n, recipes[k].size());
}

// XPKG-API-MANIFEST: the manifest as data is the manifest -- every module and
// name a recipe of spec "3" can import, the part of standard Lua it sees, and
// the compatibility layer's table, so a host's reference has nothing of its own.
TEST(CheckTest, TheInterfaceManifestIsData) {
    const auto& m = interface_manifest();
    ASSERT_FALSE(m.modules.empty());
    std::set<std::string> kinds, names;
    for (const auto& mod : m.modules) {
        EXPECT_FALSE(mod.summary.empty()) << mod.name;
        for (const auto& n : mod.names) {
            EXPECT_TRUE(n.name.starts_with(mod.name + ".")) << n.name;
            EXPECT_FALSE(n.signature.empty()) << n.name;
            EXPECT_FALSE(n.summary.empty()) << n.name;
            kinds.insert(n.kind);
            names.insert(n.name);
        }
    }
    EXPECT_EQ(kinds, (std::set<std::string>{"action", "query", "declaration", "pure"}));
    EXPECT_TRUE(names.contains("fs.move"));
    EXPECT_TRUE(names.contains("pkginfo.arch"));
    for (const auto& mod : m.modules)
        for (const auto& n : mod.names)
            if (n.name == "fs.chmod") { EXPECT_EQ(n.since, "0.0.68"); EXPECT_FALSE(n.platforms.empty()); }
            else if (n.name == "pkginfo.name") EXPECT_TRUE(n.since.empty());
    EXPECT_NE(std::ranges::find(m.stdlib_globals, "pcall"), m.stdlib_globals.end());
    EXPECT_NE(std::ranges::find(m.stdlib_libraries, "string"), m.stdlib_libraries.end());
    EXPECT_EQ(std::ranges::find(m.stdlib_libraries, "io"), m.stdlib_libraries.end());
    EXPECT_NE(std::ranges::find(m.stdlib_os, "getenv"), m.stdlib_os.end());
    const auto mv = std::ranges::find(m.compat, std::string("os.mv"), &std::pair<std::string, std::string>::first);
    ASSERT_NE(mv, m.compat.end());
    EXPECT_EQ(mv->second, "fs.move");
    const auto sleep = std::ranges::find(m.compat, std::string("os.sleep"), &std::pair<std::string, std::string>::first);
    ASSERT_NE(sleep, m.compat.end());
    EXPECT_TRUE(sleep->second.empty());
    EXPECT_NE(std::ranges::find(m.host, "elfpatch.apply_auto"), m.host.end());
}
