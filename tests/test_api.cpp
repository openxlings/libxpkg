// The module interface of a hook (design 2026-11-27 §9.6): each action states
// what holds when it returns and raises otherwise, at the recipe's line; each
// query raises only when it cannot answer. Runs on Linux, macOS and Windows.
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
    explicit Recipe(std::string_view hooks) {
        dir = fs::temp_directory_path() / ("xpkg-api-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "install");
        file = dir / "api.lua";
        std::ofstream(file, std::ios::binary)
            << "package = { spec = \"2\", name = \"api\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
            << "import(\"xim.libxpkg.fs\")\n"
            << "import(\"xim.libxpkg.system\")\n"
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "import(\"xim.libxpkg.path\")\n"
            << hooks;
    }
    ~Recipe() { std::error_code ec; fs::remove_all(dir, ec); }
    ExecutionContext context() const {
        ExecutionContext ctx;
        ctx.pkg_name = "api";
        ctx.version = "1.0.0";
        ctx.platform = kWindows ? "windows" : "linux";
        ctx.arch = "amd64";
        ctx.install_dir = dir / "install";
        ctx.args = { dir.generic_string() };
        return ctx;
    }
    // Run `install()`; the hook gets the scratch directory as D.
    HookResult install(const std::optional<HostPorts>& ports = std::nullopt) const {
        auto exec = create_executor(file);
        if (!exec) return { .success = false, .error = exec.error() };
        if (ports) exec->set_host_ports(*ports);
        return exec->run_hook(HookType::Install, context());
    }
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// D is the scratch directory, as a Lua string, for the hook below.
constexpr std::string_view kD = "local D = system.xpkg_args()[1] or _RUNTIME.args[1]\n";

std::string hook(std::string_view body) {
    return std::string("function install()\n") + std::string(kD) + std::string(body) + "\nreturn true\nend\n";
}

} // namespace

TEST(ApiFsTest, MkdirWriteReadAndList) {
    Recipe r(hook(R"LUA(
        assert(fs.mkdir(D .. "/a/b/c") == true)
        assert(fs.mkdir(D .. "/a/b/c"))                 -- already there: done
        fs.write(D .. "/a/x/new.txt", "hello\n")       -- parents made
        assert(fs.read(D .. "/a/x/new.txt") == "hello\n")
        local names = {}
        for _, e in ipairs(fs.list(D .. "/a")) do table.insert(names, e.name .. ":" .. e.type) end
        assert(table.concat(names, ",") == "b:dir,x:dir", table.concat(names, ","))
        assert(fs.is_dir(D .. "/a/b") and not fs.is_dir(D .. "/a/x/new.txt") and not fs.is_dir(D .. "/none"))
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiFsTest, AnActionThatCannotHoldRaisesWithActionPathReasonAndLine) {
    Recipe r(hook(R"LUA(
        fs.write(D .. "/file", "x")
        fs.mkdir(D .. "/file")
    )LUA"));
    auto result = r.install();
    ASSERT_FALSE(result.success);
    EXPECT_NE(result.error.find("fs.mkdir "), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("file"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("api.lua:10:"), std::string::npos) << "the recipe's line: " << result.error;
}

TEST(ApiFsTest, QueriesRaiseOnlyWhenTheyCannotAnswer) {
    Recipe r(hook(R"LUA(
        local ok, err = pcall(fs.read, D .. "/missing")
        assert(not ok and err:find("fs.read ", 1, true) and err:find("does not exist", 1, true), err)
        local ok2, err2 = pcall(fs.list, D .. "/missing")
        assert(not ok2 and err2:find("does not exist", 1, true), err2)
        fs.write(D .. "/f", "")
        local ok3, err3 = pcall(fs.list, D .. "/f")
        assert(not ok3 and err3:find("is not a directory", 1, true), err3)
        assert(fs.exists(D .. "/f") and not fs.exists(D .. "/g"))
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiFsTest, RemoveTakesTheTreeAndAMissingPathIsDone) {
    Recipe r(hook(R"LUA(
        fs.write(D .. "/t/a/b.txt", "b")
        fs.remove(D .. "/t")
        assert(not fs.exists(D .. "/t"))
        assert(fs.remove(D .. "/never-was") == true)
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// The destination is the final path: one already there is an error unless
// `replace`; parents are made.
TEST(ApiFsTest, MoveAndCopyNameTheFinalPath) {
    Recipe r(hook(R"LUA(
        fs.write(D .. "/src/inner/f.txt", "f")
        fs.copy(D .. "/src", D .. "/out/copy")
        assert(fs.read(D .. "/out/copy/inner/f.txt") == "f")
        local ok, err = pcall(fs.copy, D .. "/src", D .. "/out/copy")
        assert(not ok and err:find("already exists (pass { replace = true } to replace it)", 1, true), err)
        fs.copy(D .. "/src", D .. "/out/copy", { replace = true })
        fs.move(D .. "/src", D .. "/moved/here")
        assert(not fs.exists(D .. "/src") and fs.read(D .. "/moved/here/inner/f.txt") == "f")
        local ok2, err2 = pcall(fs.move, D .. "/gone", D .. "/x")
        assert(not ok2 and err2:find("fs.move ", 1, true) and err2:find("does not exist", 1, true), err2)
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// The archive's top-level directory moved onto the directory that holds it:
// the source is set aside first, so replacing the destination keeps it.
TEST(ApiFsTest, MoveReplacesADirectoryWithWhatIsInsideIt) {
    Recipe r(hook(R"LUA(
        fs.write(D .. "/dest/pkg-1.0/bin/tool", "tool")
        fs.write(D .. "/dest/stale.txt", "old")
        fs.move(D .. "/dest/pkg-1.0", D .. "/dest", { replace = true })
        assert(fs.read(D .. "/dest/bin/tool") == "tool")
        assert(not fs.exists(D .. "/dest/stale.txt") and not fs.exists(D .. "/dest/pkg-1.0"))
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiFsTest, SymlinkRaisesWhenTheLinkCannotBeMade) {
    if constexpr (kWindows) GTEST_SKIP() << "symbolic links need a privilege Windows runners may not grant";
    Recipe r(hook(R"LUA(
        fs.write(D .. "/target", "t")
        assert(fs.symlink(D .. "/target", D .. "/link") == true)
        assert(fs.is_symlink(D .. "/link") and fs.readlink(D .. "/link") == D .. "/target")
        local ok, err = pcall(fs.symlink, D .. "/target", D .. "/link")
        assert(not ok and err:find("fs.symlink ", 1, true), err)
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiFsTest, ChmodSetsTheModeAndIsNothingOnWindows) {
    Recipe r(hook(R"LUA(
        fs.write(D .. "/tool", "#!/bin/sh\n")
        fs.chmod(D .. "/tool", "+x")
        fs.chmod(D .. "/tool", "750")
        local ok, err = pcall(fs.chmod, D .. "/tool", "rwx")
        assert(not ok and err:find("neither octal digits", 1, true), err)
        local ok2, err2 = pcall(fs.chmod, D .. "/none", "755")
        assert(not ok2 and err2:find("does not exist", 1, true), err2)
    )LUA"));
    auto result = r.install();
    ASSERT_TRUE(result.success) << result.error;
    if constexpr (!kWindows) {
        const auto perms = fs::status(r.dir / "tool").permissions();
        EXPECT_EQ(perms & fs::perms::all, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    }
}

// Tolerating a failure has one spelling, Lua's own.
TEST(ApiFsTest, PcallIsTheWayToTolerateAFailure) {
    Recipe r(hook(R"LUA(
        local ok = pcall(fs.move, D .. "/absent", D .. "/x")
        assert(not ok)
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// ---- system.exec, system.run ----

TEST(ApiSystemTest, ExecTakesAListOrALineAndRaisesWithTheCommand) {
    const std::string body = kWindows ? R"LUA(
        system.exec({ "cmd.exe", "/d", "/c", "exit 0" })
        system.exec("exit 0")
        local ok, err = pcall(system.exec, { "cmd.exe", "/d", "/c", "exit 3" })
        assert(not ok and err:find("system.exec: cmd.exe /d /c exit 3: exited with 3", 1, true), err)
    )LUA"
        : R"LUA(
        system.exec({ "sh", "-c", "exit 0" })
        system.exec("true && exit 0")
        local ok, err = pcall(system.exec, { "sh", "-c", "exit 3" })
        assert(not ok and err:find("system.exec: sh -c exit 3: exited with 3", 1, true), err)
        local ok2, err2 = pcall(system.exec, "exit 2", { retry = 1 })
        assert(not ok2 and err2:find("exited with 2 (2 attempts)", 1, true), err2)
    )LUA";
    Recipe r(hook(body));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiSystemTest, ExecRunsInItsDirectoryWithItsEnvironmentAndCanCapture) {
    const std::string body = kWindows ? R"LUA(
        local out = system.exec({ "cmd.exe", "/d", "/c", "cd && echo %XPKG_API%" },
                                { cwd = D, env = { XPKG_API = "v w" }, capture = true })
        assert(out:find("v w", 1, true), out)
    )LUA"
        : R"LUA(
        local out = system.exec({ "sh", "-c", "pwd; printf '%s' \"$XPKG_API\"" },
                                { cwd = D, env = { XPKG_API = "v w" }, capture = true })
        assert(out:find("v w", 1, true), out)
        assert(os.getenv("XPKG_API") == nil)
    )LUA";
    Recipe r(hook(body));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiSystemTest, RunAnswersEvenWhenTheCommandFails) {
    const std::string body = kWindows ? R"LUA(
        local r = system.run({ "cmd.exe", "/d", "/c", "echo out && exit 4" })
        assert(r.ok == false and r.code == 4 and r.out:find("out", 1, true), tostring(r.code))
        local missing = system.run({ "no-such-program-for-libxpkg" })
        assert(missing.ok == false and missing.code == 127 and missing.error, tostring(missing.error))
    )LUA"
        : R"LUA(
        local r = system.run({ "sh", "-c", "echo out; exit 4" })
        assert(r.ok == false and r.code == 4 and r.out == "out\n", r.out)
        local good = system.run("printf yes")
        assert(good.ok and good.out == "yes")
        local missing = system.run({ "no-such-program-for-libxpkg" })
        assert(missing.ok == false and missing.code == 127 and missing.error, tostring(missing.error))
        local ok, err = pcall(system.run, {})
        assert(not ok and err:find("the command list is empty", 1, true), err)
    )LUA";
    Recipe r(hook(body));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// While the host keeps a hook log, a command's output goes to it.
TEST(ApiSystemTest, ACommandsOutputGoesToTheHookLog) {
    Recipe r(hook(kWindows ? "system.exec({ \"cmd.exe\", \"/d\", \"/c\", \"echo to-the-log\" })"
                           : "system.exec({ \"sh\", \"-c\", \"echo to-the-log\" })"));
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = r.context();
    ctx.hook_log = r.dir / "logs" / "install.log";
    auto result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_NE(read(ctx.hook_log).find("to-the-log"), std::string::npos) << read(ctx.hook_log);
}

// ---- pkginfo.platform, pkginfo.arch, path ----

TEST(ApiPkginfoTest, PlatformAndArchAreTheInstallsInTheFormatsNames) {
    Recipe r(hook(R"LUA(
        assert(pkginfo.platform() == (_RUNTIME.platform))
        assert(pkginfo.arch() == "x86_64", pkginfo.arch())   -- the context said amd64
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

TEST(ApiPkginfoTest, PathIsAModuleAndTheCompatGlobalIsTheSameTable) {
    Recipe r(hook(R"LUA(
        local p = import("xim.libxpkg.path")
        assert(p.join("a", "b", "", "c") == "a/b/c")
        assert(p.filename("x/y.tar.gz") == "y.tar.gz" and p.directory("x/y/z") == "x/y")
        assert(p.is_absolute("/a") and p.is_absolute("C:\\a") and not p.is_absolute("a"))
        assert(_G.path == p)
    )LUA"));
    auto result = r.install();
    EXPECT_TRUE(result.success) << result.error;
}

// ---- fs.extract: the host's port ----

TEST(ApiExtractTest, WithoutAPortTheCallSaysSo) {
    Recipe r(hook("fs.extract(D .. \"/a.tar.gz\", D .. \"/out\")"));
    auto result = r.install();
    ASSERT_FALSE(result.success);
    EXPECT_NE(result.error.find("this host gives hooks no extractor"), std::string::npos) << result.error;
}

TEST(ApiExtractTest, ThePortIsCalledWithStripAndItsErrorIsRaised) {
    Recipe r(hook(R"LUA(
        assert(fs.extract(D .. "/a.tar.gz", D .. "/out", { strip = 1 }) == true)
        local ok, err = pcall(fs.extract, D .. "/bad.tar.gz", D .. "/out")
        assert(not ok and err:find("fs.extract ", 1, true) and err:find("not an archive", 1, true), err)
        local ok2, err2 = pcall(fs.extract, D .. "/a.tar.gz", D .. "/out", { strip = -1 })
        assert(not ok2 and err2:find("strip is a whole number", 1, true), err2)
    )LUA"));
    std::vector<std::string> calls;
    HostPorts ports;
    ports.extract = [&](const fs::path& archive, const fs::path& dir, int strip) -> std::optional<std::string> {
        calls.push_back(archive.filename().string() + " -> " + dir.filename().string() + " strip " + std::to_string(strip));
        if (archive.filename() == "bad.tar.gz") return "not an archive";
        return std::nullopt;
    };
    auto result = r.install(ports);
    EXPECT_TRUE(result.success) << result.error;
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0], "a.tar.gz -> out strip 1");
    EXPECT_EQ(calls[1], "bad.tar.gz -> out strip 0");
}
