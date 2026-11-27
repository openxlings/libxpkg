// The environment of a recipe of spec "3" (design 2026-11-27 §9.6, §9.9): the
// computing part of standard Lua and the modules, seen through the interface
// manifest; no compatibility layer; module calls only from hooks.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// An index on disk: pkgs/<letter>/<name>.lua and libs/<lib>.lua.
struct Index {
    fs::path root = fs::temp_directory_path() / ("xpkg-spec3-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Index() { fs::create_directories(root / "pkgs" / "p"); fs::create_directories(root / "libs"); }
    ~Index() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path recipe(std::string_view body, std::string_view spec = "3") const {
        auto file = root / "pkgs" / "p" / "probe.lua";
        std::ofstream(file, std::ios::binary)
            << "package = { spec = \"" << spec << "\", name = \"probe\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
            << body;
        return file;
    }
    void lib(std::string_view name, std::string_view body) const {
        std::ofstream(root / "libs" / (std::string(name) + ".lua"), std::ios::binary) << body;
    }
    ExecutionContext context() const {
        ExecutionContext ctx;
        ctx.pkg_name = "probe";
        ctx.version = "1.0.0";
        ctx.platform = kWindows ? "windows" : "linux";
        ctx.arch = "x86_64";
        ctx.install_dir = root / "install";
        ctx.pkgindex_dir = root.string();
        return ctx;
    }
};

HookResult install(const fs::path& recipe, const ExecutionContext& ctx) {
    auto exec = create_executor(recipe);
    if (!exec) return { .success = false, .error = exec.error() };
    return exec->run_hook(HookType::Install, ctx);
}

} // namespace

// HOOK-ENV-SPEC3: none of the compatibility layer, none of Lua's file and
// process functions; the computing part and the modules.
TEST(Spec3EnvTest, TheCompatibilityLayerAndLuasIoAreNotThere) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.fs")
function install()
    for _, name in ipairs({ "is_host", "format", "raise", "try", "cprint", "path", "io",
                            "load", "loadfile", "dofile", "require", "collectgarbage" }) do
        assert(_G[name] == nil, name .. " is in the environment")
    end
    assert(os.mv == nil and os.cp == nil and os.execute == nil and os.remove == nil and os.exit == nil)
    assert(type(os.time()) == "number" and type(os.clock()) == "number" and os.getenv("PATH") ~= nil)
    assert(("a.tar.gz"):replace(".tar.gz", "") == "a" and ("x y"):split(" ")[2] == "y")
    assert(string.format("%d", 3) == "3" and math.max(1, 2) == 2 and table.concat({ "a" }) == "a")
    assert(fs.mkdir_p == nil and fs.remove_all == nil and fs.is_directory == nil, "old fs names")
    assert(type(fs.mkdir) == "function" and type(fs.extract) == "function")
    return true
end
)LUA");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// The same text with spec "2" is the compatibility layer's.
// The string library has the three helpers every spec has (pure, plain
// text): recipes of each spec use them, and a migration keeps them.
TEST(Spec3EnvTest, TheStringLibraryHasItsThreeHelpers) {
    Index index;
    auto recipe = index.recipe(R"LUA(
function install()
    assert(("  a b \n"):trim() == "a b")
    local parts = ("x,y,,z"):split(",", true)
    assert(#parts == 4 and parts[1] == "x" and parts[3] == "" and parts[4] == "z")
    assert(("a.b.c"):replace(".", "/") == "a/b/c", "plain text, not a pattern")
    assert(string.trim == ("").trim)
    return true
end
)LUA");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// A recipe named by a path relative to the working directory finds its
// index's libs as one named absolutely does.
TEST(Spec3EnvTest, ARelativePathFindsTheIndexLibs) {
    Index index;
    index.lib("probelib", "local M = {}\nfunction M.word() return 'lib' end\nreturn M\n");
    index.recipe(R"LUA(
local probelib = import("xim.pkgindex.probelib")
function install() return probelib.word() == "lib" end
)LUA");
    const auto here = fs::current_path();
    fs::current_path(index.root);
    auto result = install(fs::path("pkgs") / "p" / "probe.lua", index.context());
    fs::current_path(here);
    EXPECT_TRUE(result.success) << result.error;
}

TEST(Spec3EnvTest, TheSameTextWithSpec2HasTheCompatibilityLayer) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.fs")
function install()
    assert(type(is_host) == "function" and type(os.mv) == "function" and type(io.open) == "function")
    assert(type(fs.mkdir_p) == "function" and type(fs.mkdir) == "function")
    return true
end
)LUA", "2");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// E15 in the new environment: a module the interface does not have is an
// error, not a stub that swallows calls.
TEST(Spec3EnvTest, AnUnknownModuleIsAnError) {
    Index index;
    auto recipe = index.recipe(R"LUA(
function install()
    local ok, err = pcall(import, "xim.libxpkg.no_such_module")
    assert(not ok and err:find('"xim.libxpkg.no_such_module" is not a module of the hook interface', 1, true), err)
    local ok2, err2 = pcall(import, "xim.libxpkg.utils")
    assert(not ok2, "utils is the compatibility layer's")
    local ok3, err3 = pcall(import, "platform")
    assert(not ok3 and err3:find("unknown module", 1, true), err3)
    return true
end
)LUA");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// A failed action fails the hook unless the recipe says it tolerates it.
TEST(Spec3EnvTest, AnActionThatDoesNotHappenFailsTheHook) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.fs")
import("xim.libxpkg.pkginfo")
function install()
    assert(not pcall(fs.move, pkginfo.install_dir() .. "/not-extracted", pkginfo.install_dir() .. "/x"))
    fs.move(pkginfo.install_dir() .. "/not-extracted", pkginfo.install_dir() .. "/x")
    return true
end
)LUA");
    auto result = install(recipe, index.context());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("fs.move "), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("does not exist"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("probe.lua:7:"), std::string::npos) << result.error;
}

// The top level holds the package table, imports and definitions: a module
// call there is a load error, where the package table is read too.
TEST(Spec3EnvTest, AModuleCallAtTheTopLevelIsALoadError) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.fs")
fs.mkdir("/tmp/never-made-by-a-recipe-top-level")
function install() return true end
)LUA");
    auto pkg = load_package(recipe);
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("fs.mkdir is called at the top level of a recipe of spec \"3\""), std::string::npos)
        << pkg.error();
    auto exec = create_executor(recipe);
    ASSERT_FALSE(exec.has_value());
    EXPECT_NE(exec.error().find("top level"), std::string::npos) << exec.error();
    EXPECT_FALSE(fs::exists("/tmp/never-made-by-a-recipe-top-level"));
}

TEST(Spec3EnvTest, ACompatibilityNameAtTheTopLevelIsALoadError) {
    Index index;
    auto recipe = index.recipe(R"LUA(
if is_host("linux") then package.description = "linux" end
function install() return true end
)LUA");
    auto pkg = load_package(recipe);
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("is_host"), std::string::npos) << pkg.error();
}

// Pure functions may run at the top level.
TEST(Spec3EnvTest, PureFunctionsMayRunAtTheTopLevel) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.path")
import("xim.libxpkg.json")
local bin = path.join("bin", "probe")
local data = json.decode('{"a": 1}')
function install() assert(bin == "bin/probe" and data.a == 1) return true end
)LUA");
    auto pkg = load_package(recipe);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_EQ(pkg->spec, "3");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// An index lib a recipe of spec "3" imports runs under the same rules, in an
// environment of its own.
TEST(Spec3EnvTest, AnIndexLibRunsUnderTheSameRules) {
    Index index;
    index.lib("good", R"LUA(
import("xim.libxpkg.fs")
local M = {}
function M.made(dir) fs.mkdir(dir .. "/by-the-lib") return fs.is_dir(dir .. "/by-the-lib") end
return M
)LUA");
    index.lib("old", R"LUA(
local M = {}
function M.check(dir) return os.isdir(dir) end
return M
)LUA");
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.pkginfo")
import("xim.pkgindex.good")
import("xim.pkgindex.old")
function install()
    assert(good.made(pkginfo.install_dir()))
    local ok, err = pcall(old.check, pkginfo.install_dir())
    assert(not ok and err:find("isdir", 1, true), err)
    assert(helper_global == nil, "a lib's globals stay in its environment")
    return true
end
)LUA");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(fs::is_directory(index.root / "install" / "by-the-lib"));
}

// A recipe's top level runs once, in the environment its spec chooses: the
// executor reads spec from the text (declared_spec), not by running it first.
TEST(Spec3EnvTest, ATopLevelRunsOnceWhenTheExecutorIsMade) {
    Index index;
    const auto marks = index.root / "top-level-runs";
    auto recipe = index.recipe(std::format(R"LUA(
local f = io.open([[{}]], "a")
f:write("ran\n")
f:close()
function install() return true end
)LUA", marks.string()), "2");
    auto exec = create_executor(recipe);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    std::ifstream in(marks);
    std::string line;
    int runs = 0;
    while (std::getline(in, line)) ++runs;
    EXPECT_EQ(runs, 1);
}

// The spec is read from the recipe's code: a comment or a string that says
// spec is not the declaration, and a table whose spec is not the string its
// text declares is a load error -- the executor could not know it.
TEST(Spec3EnvTest, TheSpecIsTheStringTheTextDeclares) {
    EXPECT_EQ(declared_spec("package = { spec = \"3\", name = \"x\" }"), "3");
    EXPECT_EQ(declared_spec("package = {\n  spec='2',\n}"), "2");
    EXPECT_EQ(declared_spec("-- spec = \"3\" is the current one\npackage = { name = \"x\" }"), std::nullopt);
    EXPECT_EQ(declared_spec("--[[ spec = \"3\" ]] package = { description = \"spec = '3'\", spec = \"1\" }"), "1");
    EXPECT_EQ(declared_spec("package = { name = \"x\" }\nif package.spec == \"3\" then end"), std::nullopt);
    EXPECT_EQ(declared_spec("package = { xspec = \"3\" }"), std::nullopt);
    EXPECT_EQ(recipe_code("a = \"b\" -- c\nd"), "a =" + std::string(9, ' ') + "\nd");

    Index index;
    auto file = index.root / "pkgs" / "p" / "probe.lua";
    std::ofstream(file, std::ios::binary)
        << "local s = \"3\"\npackage = { spec = s, name = \"probe\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n";
    auto pkg = load_package(file);
    ASSERT_FALSE(pkg.has_value());
    EXPECT_NE(pkg.error().find("package.spec is \"3\", and its text declares none"), std::string::npos) << pkg.error();
}

TEST(Spec3EnvTest, AHigherSpecIsRefusedByName) {
    Index index;
    auto recipe = index.recipe("function install() return true end\n", "4");
    auto exec = create_executor(recipe);
    ASSERT_FALSE(exec.has_value());
    EXPECT_NE(exec.error().find("spec \"4\" is newer than this libxpkg reads (the highest is 3)"), std::string::npos)
        << exec.error();
    EXPECT_EQ(spec_number("3"), 3);
    EXPECT_EQ(spec_number(""), std::nullopt);
    EXPECT_EQ(spec_number("2.0"), std::nullopt);
}

TEST(Spec3EnvTest, PrintAndTheHooksOutputAreCaptured) {
    Index index;
    auto recipe = index.recipe(R"LUA(
import("xim.libxpkg.log")
function install() print("from print") log.warn("a notice") return true end
)LUA");
    auto result = install(recipe, index.context());
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_NE(result.output.find("from print"), std::string::npos) << result.output;
    ASSERT_EQ(result.notices.size(), 1u);
    EXPECT_EQ(result.notices[0].text, "a notice");
}

// XPKG-API-MANIFEST: every function a module has is listed in the manifest --
// as a name of the interface, of the compatibility layer, or of the host -- and
// every name it lists for a module exists there.
TEST(ManifestTest, EveryFunctionOfEveryModuleIsListed) {
    Index index;
    auto recipe = index.recipe(R"LUA(
function install()
    local listed = {}
    for _, m in ipairs(_LIBXPKG_MANIFEST.modules) do
        for _, row in ipairs(m.entries) do
            local fn = _LIBXPKG_MODULES[m.name][row[1]]
            assert(type(fn) == "function", m.name .. "." .. row[1] .. " is listed and missing")
            listed[m.name .. "." .. row[1]] = true
        end
    end
    for _, n in ipairs(_LIBXPKG_MANIFEST.host) do listed[n] = true end
    for _, row in ipairs(_LIBXPKG_MANIFEST.compat) do listed[row[1]] = true end
    local missing = {}
    for name, m in pairs(_LIBXPKG_MODULES) do
        if type(m) == "table" then
            for k, v in pairs(m) do
                if type(v) == "function" and not listed[name .. "." .. k] and not listed[name] then
                    table.insert(missing, name .. "." .. k)
                end
            end
        end
    end
    table.sort(missing)
    assert(#missing == 0, "not in the manifest: " .. table.concat(missing, ", "))
    return true
end
)LUA", "2");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}

// The compatibility names the manifest lists are the ones a recipe of spec 1
// or 2 sees.
TEST(ManifestTest, TheCompatibilityNamesExistWhereTheyBelong) {
    Index index;
    auto recipe = index.recipe(R"LUA(
function install()
    local absent = {}
    for _, row in ipairs(_LIBXPKG_MANIFEST.compat) do
        local name = row[1]
        if not name:find(" ") then
            local value = _G
            for part in name:gmatch("[^.]+") do
                value = type(value) == "table" and (value[part] or (_LIBXPKG_MODULES[part] ~= nil and value == _G and _LIBXPKG_MODULES[part]) or nil) or nil
            end
            if value == nil then table.insert(absent, name) end
        end
    end
    assert(#absent == 0, "listed and absent: " .. table.concat(absent, ", "))
    return true
end
)LUA", "2");
    auto result = install(recipe, index.context());
    EXPECT_TRUE(result.success) << result.error;
}
