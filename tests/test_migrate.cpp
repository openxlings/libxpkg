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

// A guard on the extracted directory goes with the landing: under the host's
// landing the directory is never there to test.
TEST(MigrateTest, AGuardOnTheExtractedDirectoryGoesWithTheLanding) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    local dir = pkginfo.install_dir()
    local payload = pkginfo.install_file():replace(".tar.gz", "")
    if os.isdir(payload) then
        os.tryrm(dir)
        os.mv(payload, dir)
    end
    return os.isfile(path.join(dir, "bin", "ps"))
end
)LUA"));
    EXPECT_TRUE(m.spec3);
    EXPECT_TRUE(m.landing_removed);
    EXPECT_FALSE(has(m.text, "payload")) << m.text;
    EXPECT_FALSE(has(m.text, "\n    end\n")) << m.text;
    EXPECT_TRUE(has(m.text, "return fs.is_file(path.join(dir, \"bin\", \"ps\"))")) << m.text;

    const auto raised = migrate_recipe(recipe(R"LUA(
function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    local extracted = pkginfo.install_file():replace(".tar.xz", "")
    if not os.isdir(extracted) then
        raise("the payload was not found at " .. extracted)
    end
    os.mv(extracted, dir)
    io.writefile(path.join(dir, "stamp"), "x")
    return true
end
)LUA"));
    EXPECT_TRUE(raised.spec3);
    EXPECT_FALSE(has(raised.text, "extracted")) << raised.text;
    EXPECT_TRUE(has(raised.text, "fs.write(path.join(dir, \"stamp\"), \"x\")")) << raised.text;
}

// A landing variable used for anything else is not a landing the tool knows.
TEST(MigrateTest, ALandingVariableUsedElsewhereIsForAPerson) {
    const auto m = migrate_recipe(recipe(R"LUA(
function install()
    local dir = pkginfo.install_dir()
    local payload = pkginfo.install_file():replace(".tar.gz", "")
    os.tryrm(dir)
    os.mv(payload, dir)
    log.info("landed " .. payload)
    return true
end
)LUA"));
    EXPECT_FALSE(m.landing_removed);
}

// A lib function the index names as a landing is one.
TEST(MigrateTest, AnIndexLandingCallIsALanding) {
    const auto text = recipe(R"LUA(
import("xim.pkgindex.sysroot")
function install()
    sysroot.adopt_payload()
    return true
end
)LUA");
    EXPECT_FALSE(migrate_recipe(text).landing_removed);
    const auto m = migrate_recipe(text, { .landing_calls = { "sysroot.adopt_payload" } });
    EXPECT_TRUE(m.landing_removed);
    EXPECT_TRUE(m.spec3);
    EXPECT_FALSE(has(m.text, "adopt_payload")) << m.text;
    EXPECT_TRUE(has(m.text, "strip = 1")) << m.text;
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
    os.iorun("touch stamp")
    os.execute("rm -f stale")
    os.trymv("a", "b")
    cprint(format("%s", raise and "x" or "y"))
    return true
end
)LUA"));
    EXPECT_TRUE(m.spec3) << (m.left.empty() ? "" : m.left.front().name);
    EXPECT_TRUE(has(m.text, "if (pkginfo.platform() == \"linux\") then"));
    EXPECT_TRUE(has(m.text, "system.exec(\"true\")"));
    EXPECT_TRUE(has(m.text, "system.run(\"echo hi\").out"));     // never failed, still does not
    EXPECT_TRUE(has(m.text, "    system.run(\"touch stamp\")\n")) << m.text;   // a statement: no `.out`
    EXPECT_TRUE(has(m.text, "system.run(\"rm -f stale\")"));     // its status was not read
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

TEST(MigrateTest, ACopyOrMoveIsForAPersonItsDestinationMayBeADirectory) {
    // os.cp and os.mv put src into dst when dst is a directory and replace what
    // is there; fs.copy and fs.move take the final path and refuse one that
    // exists. The text does not say which a call meant.
    const auto m = migrate_recipe(recipe(R"LUA(
function config()
    os.cp(path.join(pkginfo.install_dir(), "a.h"), system.subos_sysrootdir())
    os.mv("x", "y")
    return true
end
)LUA"));
    EXPECT_FALSE(m.spec3);
    EXPECT_TRUE(has(m.text, "os.cp(")) << m.text;
    EXPECT_TRUE(has(m.text, "os.mv(")) << m.text;
    EXPECT_EQ(std::ranges::count_if(m.left, [](const MigrationNote& n) { return n.name == "os.cp" || n.name == "os.mv"; }), 2);
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

TEST(MigrateTest, ALibGetsItsNamesAndImportsAndNoSpec) {
    const auto m = migrate_recipe(R"LUA(local M = {}
function M.recipe_text(name)
    return string.format('package = { spec = "1", name = %q }', name)
end
function M.stamp(dir)
    io.writefile(path.join(dir, "stamp"), "x")
end
return M
)LUA", { .lib = true });
    EXPECT_FALSE(m.spec3);
    EXPECT_TRUE(has(m.text, "spec = \"1\""));   // the text it makes is not its own spec
    EXPECT_TRUE(has(m.text, "fs.write(path.join(dir, \"stamp\"), \"x\")"));
    EXPECT_TRUE(has(m.text, "import(\"xim.libxpkg.fs\")"));
    EXPECT_TRUE(m.left.empty());
}

TEST(MigrateTest, ANameUsedAsAValueIsForAPerson) {
    const auto m = migrate_recipe(recipe(R"LUA(
function config()
    local ok, out = pcall(os.iorun, "uname -m")
    if type(os.iorun) == "function" then out = os.iorun("uname -m") end
    return true
end
)LUA"));
    EXPECT_FALSE(m.spec3);
    EXPECT_TRUE(has(m.text, "pcall(os.iorun, \"uname -m\")")) << m.text;
    EXPECT_TRUE(has(m.text, "out = system.run(\"uname -m\").out")) << m.text;
    EXPECT_EQ(std::ranges::count_if(m.left, [](const MigrationNote& n) { return n.name == "os.iorun"; }), 2);
}
