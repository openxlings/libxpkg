// The migration of a recipe of spec 1 or 2 into spec "3" (design 2026-11-27
// §9.6): what keeps its meaning is rewritten, a landing install() becomes
// `strip = 1`, and the recipe is spec "3" only when nothing is left for a
// person.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.migrate;

using namespace mcpplibs::xpkg;

namespace {

constexpr std::string_view kHead = R"LUA(package = {
    spec = "1",
    name = "probe",
    xpm = {
        linux = {
            -- the archive holds probe-<version>/
            ["1.0.0"] = { url = "https://example.invalid/probe-1.0.0.tar.gz", sha256 = { x86_64 = "00" } },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
)LUA";

std::string recipe(std::string_view body) { return std::string(kHead) + std::string(body); }

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

} // namespace

TEST(MigrateTest, ALandingInstallBecomesStrip) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    -- move the extracted directory into place
    local srcdir = pkginfo.install_file()
        :replace(".zip", "")
        :replace(".tar.gz", "")
    os.tryrm(pkginfo.install_dir())
    os.mv(srcdir, pkginfo.install_dir())
    return true
end

function config()
    xvm.add("probe", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
)LUA"));
    EXPECT_TRUE(m.spec3);
    EXPECT_TRUE(m.landing_removed);
    EXPECT_TRUE(m.left.empty()) << m.left.front().name;
    EXPECT_TRUE(has(m.text, "spec = \"3\""));
    EXPECT_FALSE(has(m.text, "function install"));
    EXPECT_TRUE(has(m.text, "        linux = {\n            strip = 1,\n")) << m.text;
    EXPECT_TRUE(has(m.text, "import(\"xim.libxpkg.path\")"));
}

TEST(MigrateTest, ALandingBesideOtherStatementsLeavesThem) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mv("probe-" .. pkginfo.version(), dir)
    io.writefile(path.join(dir, "stamp"), "x")
    return os.isfile(path.join(dir, "stamp"))
end
)LUA"));
    EXPECT_TRUE(m.spec3);
    EXPECT_TRUE(m.landing_removed);
    EXPECT_FALSE(has(m.text, "os.tryrm"));
    EXPECT_FALSE(has(m.text, "os.mv"));
    EXPECT_TRUE(has(m.text, "fs.write(path.join(dir, \"stamp\"), \"x\")")) << m.text;
    EXPECT_TRUE(has(m.text, "return fs.is_file(path.join(dir, \"stamp\"))"));
    EXPECT_TRUE(has(m.text, "import(\"xim.libxpkg.fs\")"));
}

TEST(MigrateTest, NamesAreRewrittenWhereTheMeaningIsKept) {
    const auto m = migrate_recipe(recipe(R"LUA(
function config()
    if is_host("linux") then
        os.exec("true")
    end
    local out = os.iorun("echo hi")
    os.trymv("a", "b")
    cprint(format("%s", raise and "x" or "y"))
    return true
end
)LUA"));
    EXPECT_TRUE(m.spec3) << (m.left.empty() ? "" : m.left.front().name);
    EXPECT_TRUE(has(m.text, "if (pkginfo.platform() == \"linux\") then"));
    EXPECT_TRUE(has(m.text, "system.exec(\"true\")"));
    EXPECT_TRUE(has(m.text, "system.exec(\"echo hi\", { capture = true })"));
    EXPECT_TRUE(has(m.text, "pcall(fs.move, \"a\", \"b\")"));
    EXPECT_TRUE(has(m.text, "log.info(string.format(\"%s\""));
    for (const auto* mod : {"system", "fs", "log"})
        EXPECT_NE(std::ranges::find(m.imports_added, std::string(mod)), m.imports_added.end()) << mod;
}

TEST(MigrateTest, WhatNeedsAPersonKeepsTheSpecAndIsListed) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    for _, d in ipairs(os.dirs(path.join(pkginfo.install_dir(), "*"))) do
        os.mkdir(d)
    end
    return true
end
)LUA"));
    EXPECT_FALSE(m.spec3);
    EXPECT_TRUE(has(m.text, "spec = \"1\""));
    EXPECT_TRUE(has(m.text, "fs.mkdir(d)"));   // rewritten all the same: the module's name works in spec 1
    ASSERT_FALSE(m.left.empty());
    EXPECT_EQ(m.left.front().name, "os.dirs");
    EXPECT_EQ(m.left.front().line, 16);
}

TEST(MigrateTest, AnInstallThatLandsItselfAnotherWayIsForAPerson) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    local dl = path.directory(pkginfo.install_file())
    os.mv(path.join(dl, "probe"), path.join(pkginfo.install_dir(), "probe"))
    return true
end
)LUA"));
    EXPECT_FALSE(m.spec3);
    EXPECT_FALSE(m.landing_removed);
    ASSERT_FALSE(m.left.empty());
    EXPECT_EQ(m.left.front().name, "install()");
}

TEST(MigrateTest, ACallAtTheTopLevelIsForAPerson) {
    auto text = recipe("function install() return true end\n");
    text.insert(0, "local a = os.arch()\n");
    const auto m = migrate_recipe(text);
    EXPECT_FALSE(m.spec3);
    ASSERT_FALSE(m.left.empty());
    EXPECT_EQ(m.left.front().line, 1);
}
