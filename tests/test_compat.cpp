// The compatibility layer is frozen (design 2026-11-27 §9.6): each of its names
// returns what it always returned (the measurements E8, E15, E16, E17 of the
// design); what it adds is a record of the actions that failed, for the host.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

struct Recipe {
    fs::path dir;
    fs::path file;
    explicit Recipe(std::string_view install_body, std::string_view spec = "2") {
        dir = fs::temp_directory_path() / ("xpkg-compat-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "install");
        file = dir / "compat.lua";
        std::ofstream(file, std::ios::binary)
            << "package = { spec = \"" << spec << "\", name = \"compat\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "function install()\n"
            << "    local D = _RUNTIME.args[1]\n"
            << install_body
            << "\n    return true\nend\n";
    }
    ~Recipe() { std::error_code ec; fs::remove_all(dir, ec); }
    HookResult install() const {
        auto exec = create_executor(file);
        if (!exec) return { .success = false, .error = exec.error() };
        ExecutionContext ctx;
        ctx.pkg_name = "compat";
        ctx.version = "1.0.0";
        ctx.platform = kWindows ? "windows" : "linux";
        ctx.install_dir = dir / "install";
        ctx.args = { dir.generic_string() };
        return exec->run_hook(HookType::Install, ctx);
    }
};

} // namespace

// E8: os.exec of a failing command returns Lua's results and the hook goes on.
TEST(CompatFrozenTest, OsExecOfAFailingCommandReturnsItsStatusAndIsRecorded) {
    Recipe r(R"LUA(
    local a, b, c = os.exec("exit 1")
    assert(a == nil and b == "exit" and c == 1, tostring(a) .. " " .. tostring(b) .. " " .. tostring(c))
    os.exec("exit 0")
    )LUA");
    auto result = r.install();
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.call_failures.size(), 1u);
    EXPECT_EQ(result.call_failures[0].call, "os.exec");
    EXPECT_EQ(result.call_failures[0].what, "exit 1");
    EXPECT_EQ(result.call_failures[0].why, "exited with 1");
}

// E16: os.mv of a directory that is not there returns false; the hook reports
// success, and the host is told which call did not happen.
TEST(CompatFrozenTest, OsMvOfAMissingDirectoryReturnsFalseAndIsRecorded) {
    Recipe r(R"LUA(
    assert(os.mv(D .. "/not-extracted", pkginfo.install_dir()) == false)
    assert(os.trymv(D .. "/not-extracted", D .. "/x") == false)
    )LUA");
    auto result = r.install();
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.call_failures.size(), 2u);
    EXPECT_EQ(result.call_failures[0].call, "os.mv");
    EXPECT_NE(result.call_failures[0].what.find("not-extracted -> "), std::string::npos) << result.call_failures[0].what;
    EXPECT_FALSE(result.call_failures[0].why.empty());
    EXPECT_EQ(result.call_failures[1].call, "os.mv") << "os.trymv is os.mv";
}

TEST(CompatFrozenTest, OsCpAndOsMkdirReturnFalseAndAreRecorded) {
    Recipe r(R"LUA(
    assert(os.cp(D .. "/no-such-file", D .. "/copy") == false)
    local f = io.open(D .. "/a-file", "w"); f:write("x"); f:close()
    assert(os.mkdir(D .. "/a-file/sub") == false)
    assert(os.mkdir(D .. "/made/sub") == true)
    assert(os.cp(D .. "/a-file", D .. "/made/sub/copied") == true)
    )LUA");
    auto result = r.install();
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.call_failures.size(), 2u);
    EXPECT_EQ(result.call_failures[0].call, "os.cp");
    EXPECT_EQ(result.call_failures[1].call, "os.mkdir");
}

// E17: os.tryrm is true whatever happened; "try" is in its name, and it is
// not recorded.
TEST(CompatFrozenTest, OsTryrmIsAlwaysTrue) {
    if constexpr (kWindows) GTEST_SKIP() << "a path the user cannot remove needs POSIX permissions here";
    Recipe r(R"LUA(
    local f = io.open(D .. "/locked/file", "w")
    )LUA");
    fs::create_directories(r.dir / "locked");
    std::ofstream(r.dir / "locked" / "file") << "x";
    fs::permissions(r.dir / "locked", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    Recipe probe(std::string("assert(os.tryrm(\"") + (r.dir / "locked" / "file").generic_string() + "\") == true)");
    auto result = probe.install();
    fs::permissions(r.dir / "locked", fs::perms::owner_all, fs::perm_options::replace);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_TRUE(result.call_failures.empty());
}

// E15: a module the layer does not know is a stub that swallows every call.
TEST(CompatFrozenTest, AnUnknownModuleIsAStub) {
    Recipe r(R"LUA(
    local m = import("xim.libxpkg.no_such_module")
    assert(type(m.anything()) == "table")
    )LUA");
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// The layer's other names, as recipes of spec 1 and 2 use them.
TEST(CompatFrozenTest, TheGlobalsOfTheLayerAreThere) {
    Recipe r(R"LUA(
    assert(format("%d-%s", 1, "a") == "1-a")
    assert(("a.tar.gz"):replace(".tar.gz", "") == "a")
    assert(is_host(_RUNTIME.platform) and os.host() == _RUNTIME.platform)
    assert(type(raise) == "function" and type(cprint) == "function")
    local caught
    try { function() error("boom") end, catch = { function(e) caught = e end } }
    assert(caught and caught:find("boom", 1, true))
    assert(path.join("a", "b") == "a/b")
    )LUA");
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(result.call_failures.empty());
}

TEST(CompatFrozenTest, TheRecordIsBounded) {
    Recipe r(R"LUA(
    for i = 1, 150 do os.mv(D .. "/none-" .. i, D .. "/x") end
    )LUA");
    auto result = r.install();
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.call_failures.size(), 100u);
}
