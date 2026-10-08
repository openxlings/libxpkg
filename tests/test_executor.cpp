#include <gtest/gtest.h>
#include <cstdlib>
import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

#ifndef XPKG_TEST_PKGINDEX
#  define XPKG_TEST_PKGINDEX tests/fixtures/pkgindex
#endif

#define XPKG_STRINGIFY_IMPL(x) #x
#define XPKG_STRINGIFY(x) XPKG_STRINGIFY_IMPL(x)

namespace {

std::string_view normalize_pkgindex_macro(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

static const fs::path PKGINDEX{
    std::string(normalize_pkgindex_macro(XPKG_STRINGIFY(XPKG_TEST_PKGINDEX)))
};
static const fs::path HELLO_PKG = PKGINDEX / "pkgs/h/hello.lua";

fs::path make_temp_dir(std::string_view prefix) {
    auto dir = fs::temp_directory_path() / fs::path(prefix);
    dir += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, std::string_view content) {
    std::ofstream out(path);
    ASSERT_TRUE(out.good()) << "failed to write " << path.string();
    out << content;
}

void write_executable_script(const fs::path& path, std::string_view content) {
    write_text(path, content);
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                    fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
}

struct ScopedEnvVar {
    std::string name;
    std::optional<std::string> old_value;

    ScopedEnvVar(std::string name_, std::string value)
        : name(std::move(name_)) {
        if (const char* existing = std::getenv(name.c_str())) {
            old_value = existing;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (old_value) {
            set(*old_value);
        } else {
            unset();
        }
    }

private:
    void set(std::string_view value) const {
#ifdef _WIN32
        _putenv_s(name.c_str(), std::string(value).c_str());
#else
        ::setenv(name.c_str(), std::string(value).c_str(), 1);
#endif
    }

    void unset() const {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        ::unsetenv(name.c_str());
#endif
    }
};

ExecutionContext make_context(const fs::path& install_dir, std::string platform,
                             const fs::path& tools_dir = {}) {
    ExecutionContext ctx;
    ctx.pkg_name = "elfpatch-macos";
    ctx.version = "1.0.0";
    ctx.platform = std::move(platform);
    ctx.arch = "arm64";
    ctx.install_file = install_dir / "elfpatch-macos.lua";
    ctx.install_dir = install_dir;
    ctx.run_dir = install_dir;
    ctx.xpkg_dir = install_dir;
    ctx.bin_dir = tools_dir.empty() ? install_dir / "bin" : tools_dir;
    ctx.project_data_dir = install_dir / "data";
    return ctx;
}

bool is_valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead <= 0x7f) {
            ++i;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            continuationCount = 1;
            codePoint = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            continuationCount = 2;
            codePoint = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            continuationCount = 3;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (i + continuationCount >= text.size()) return false;
        for (std::size_t j = 1; j <= continuationCount; ++j) {
            const auto byte = static_cast<unsigned char>(text[i + j]);
            if ((byte & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (byte & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return false;
        }
        i += continuationCount + 1;
    }
    return true;
}

} // namespace

TEST(ExecutorTest, CreateExecutor_ExistingFile) {
    auto result = create_executor(HELLO_PKG);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error());
}

TEST(ExecutorTest, CreateExecutor_MissingFile) {
    auto result = create_executor("/nonexistent/path/pkg.lua");
    EXPECT_FALSE(result.has_value());
}

TEST(ExecutorTest, HasHook_Install) {
    auto exec = create_executor(HELLO_PKG);
    ASSERT_TRUE(exec.has_value());
    EXPECT_TRUE(exec->has_hook(HookType::Install));
}

TEST(ExecutorTest, HasHook_Config) {
    auto exec = create_executor(HELLO_PKG);
    ASSERT_TRUE(exec.has_value());
    EXPECT_TRUE(exec->has_hook(HookType::Config));
}

TEST(ExecutorTest, HasHook_Uninstall) {
    auto exec = create_executor(HELLO_PKG);
    ASSERT_TRUE(exec.has_value());
    EXPECT_TRUE(exec->has_hook(HookType::Uninstall));
}

TEST(ExecutorTest, HasHook_Installed_True) {
    auto exec = create_executor(HELLO_PKG);
    ASSERT_TRUE(exec.has_value());
    // hello.lua has an installed() hook (unlike the old mdbook fixture)
    EXPECT_TRUE(exec->has_hook(HookType::Installed));
}

TEST(ExecutorTest, RunHook_CapturesLuaOutputAndNamesFalse) {
    const fs::path temp = make_temp_dir("libxpkg-hook-output-");
    const fs::path pkg = temp / "hook-output.lua";
    write_text(pkg,
        "package = { name = \"hook-output\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "local log = import(\"xim.libxpkg.log\")\n"
        "function install()\n"
        "    print(\"REPRO stdout\")\n"
        "    log.error(\"REPRO log.error\")\n"
        "    io.stderr:write(\"REPRO stderr\\n\")\n"
        "    return false\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const auto result = exec->run_hook(HookType::Install,
                                       make_context(temp / "install", "linux"));
    const std::string escapedStdout = testing::internal::GetCapturedStdout();
    const std::string escapedStderr = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "install hook returned false");
    EXPECT_NE(result.output.find("REPRO stdout"), std::string::npos);
    EXPECT_NE(result.output.find("REPRO log.error"), std::string::npos);
    EXPECT_NE(result.output.find("REPRO stderr"), std::string::npos);
    EXPECT_EQ(escapedStdout.find("REPRO"), std::string::npos);
    EXPECT_EQ(escapedStderr.find("REPRO"), std::string::npos);

    fs::remove_all(temp);
}

TEST(ExecutorTest, RunHook_BoundsTranscriptAndKeepsTail) {
    constexpr std::size_t outputCap = 16 * 1024;
    constexpr std::string_view truncatedMarker =
        "\n[libxpkg: hook output truncated]\n";
    const fs::path temp = make_temp_dir("libxpkg-hook-output-bound-");
    const fs::path pkg = temp / "hook-output-bound.lua";
    write_text(pkg,
        "package = { name = \"hook-output-bound\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function install()\n"
        "    io.write(string.rep(\"HEAD-\", 4000))\n"
        "    io.write(string.char(0xff))\n"
        "    io.write(\"TAIL-MARKER\\n\")\n"
        "    return false\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    const auto result = exec->run_hook(HookType::Install,
                                       make_context(temp / "install", "linux"));

    EXPECT_FALSE(result.success);
    EXPECT_LE(result.output.size(), outputCap + truncatedMarker.size());
    EXPECT_NE(result.output.find("TAIL-MARKER"), std::string::npos);
    EXPECT_NE(result.output.find("\xef\xbf\xbd"), std::string::npos);
    EXPECT_TRUE(is_valid_utf8(result.output));
    EXPECT_EQ(result.output.find(truncatedMarker),
              result.output.rfind(truncatedMarker));
    EXPECT_NE(result.output.find(truncatedMarker), std::string::npos);

    fs::remove_all(temp);
}

TEST(ExecutorTest, RunHook_ExecutorTranscriptsDoNotCross) {
    const fs::path temp = make_temp_dir("libxpkg-hook-output-concurrent-");
    const fs::path pkgA = temp / "hook-a.lua";
    const fs::path pkgB = temp / "hook-b.lua";
    write_text(pkgA,
        "package = { name = \"hook-a\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function install()\n"
        "    for _ = 1, 2000 do io.write(\"A\") end\n"
        "    print(\"MARKER-A\")\n"
        "    return false\n"
        "end\n");
    write_text(pkgB,
        "package = { name = \"hook-b\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function install()\n"
        "    for _ = 1, 2000 do io.write(\"B\") end\n"
        "    print(\"MARKER-B\")\n"
        "    return false\n"
        "end\n");

    auto execA = create_executor(pkgA);
    auto execB = create_executor(pkgB);
    ASSERT_TRUE(execA.has_value()) << execA.error();
    ASSERT_TRUE(execB.has_value()) << execB.error();
    std::latch start { 2 };
    auto runA = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return execA->run_hook(HookType::Install,
                               make_context(temp / "install-a", "linux"));
    });
    auto runB = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return execB->run_hook(HookType::Install,
                               make_context(temp / "install-b", "linux"));
    });

    const auto resultA = runA.get();
    const auto resultB = runB.get();
    EXPECT_NE(resultA.output.find("MARKER-A"), std::string::npos);
    EXPECT_EQ(resultA.output.find("MARKER-B"), std::string::npos);
    EXPECT_NE(resultB.output.find("MARKER-B"), std::string::npos);
    EXPECT_EQ(resultB.output.find("MARKER-A"), std::string::npos);

    fs::remove_all(temp);
}

TEST(ExecutorTest, RunHook_PreservesStderrMethodsAndOrdinaryFileWrites) {
    const fs::path temp = make_temp_dir("libxpkg-hook-output-files-");
    const fs::path pkg = temp / "hook-output-files.lua";
    const fs::path written = temp / "written.txt";
    write_text(pkg,
        "package = { name = \"hook-output-files\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function install()\n"
        "    io.stderr:write(\"STDERR-MARKER\\n\")\n"
        "    io.stderr:flush()\n"
        "    local file = assert(io.open(\"" + written.string() + "\", \"w\"))\n"
        "    file:write(\"FILE-PAYLOAD\")\n"
        "    file:close()\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    const auto result = exec->run_hook(HookType::Install,
                                       make_context(temp / "install", "linux"));

    EXPECT_TRUE(result.success) << result.error;
    EXPECT_NE(result.output.find("STDERR-MARKER"), std::string::npos);
    EXPECT_EQ(result.output.find("FILE-PAYLOAD"), std::string::npos);
    std::ifstream input(written);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}),
              "FILE-PAYLOAD");

    fs::remove_all(temp);
}

TEST(ExecutorTest, RunScriptCallsXpkgMain) {
    auto tmp = fs::temp_directory_path() / "test_run_script.lua";
    {
        std::ofstream out(tmp);
        out << R"(
            package = { name = "test-script", xpm = { linux = { ["0.0.1"] = {} } } }
            _test_result = nil
            function xpkg_main(a, b)
                _test_result = (a or "") .. ":" .. (b or "")
            end
        )";
    }
    auto exec = create_executor(tmp);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {"hello", "world"};
    auto result = exec->run_script(ctx);
    EXPECT_TRUE(result.success) << result.error;
    fs::remove(tmp);
}

TEST(ExecutorTest, RunScriptFailsWithoutXpkgMain) {
    auto tmp = fs::temp_directory_path() / "test_run_script_no_main.lua";
    {
        std::ofstream out(tmp);
        out << "package = { name = \"no-main\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n";
    }
    auto exec = create_executor(tmp);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    ExecutionContext ctx;
    ctx.platform = "linux";
    auto result = exec->run_script(ctx);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("xpkg_main"), std::string::npos);
    fs::remove(tmp);
}

// ---- os.* C++ override tests ----

TEST(ExecutorTest, OsFuncs_Cp_CopiesDirectory) {
    const fs::path temp = make_temp_dir("libxpkg-oscp-dir-");
    const fs::path src = temp / "src_dir";
    const fs::path dst = temp / "dst_dir";
    fs::create_directories(src / "sub");
    write_text(src / "a.txt", "hello");
    write_text(src / "sub" / "b.txt", "world");

    auto pkg = temp / "oscp.lua";
    write_text(pkg, std::string(
        "package = { name = \"oscp\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(s, d) return os.cp(s, d) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {src.string(), dst.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    // dst didn't exist, so src contents are copied directly into dst
    EXPECT_TRUE(fs::exists(dst / "a.txt"));
    EXPECT_TRUE(fs::exists(dst / "sub" / "b.txt"));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Cp_CopiesDirIntoExistingDir) {
    // cp -a semantics: copy dir into existing dir creates dst/src_name/...
    const fs::path temp = make_temp_dir("libxpkg-oscp-into-");
    const fs::path include = temp / "include";
    const fs::path usr = temp / "usr";
    fs::create_directories(include / "linux");
    fs::create_directories(usr);
    write_text(include / "linux" / "errno.h", "#define ERRNO_H");

    auto pkg = temp / "oscp_into.lua";
    write_text(pkg, std::string(
        "package = { name = \"oscp_into\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(s, d) return os.cp(s, d) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {include.string(), usr.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    // include copied INTO usr → usr/include/linux/errno.h
    EXPECT_TRUE(fs::exists(usr / "include" / "linux" / "errno.h"));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Cp_CopiesFile) {
    const fs::path temp = make_temp_dir("libxpkg-oscp-file-");
    const fs::path src = temp / "file.txt";
    const fs::path dst = temp / "copy.txt";
    write_text(src, "content");

    auto pkg = temp / "oscp2.lua";
    write_text(pkg, std::string(
        "package = { name = \"oscp2\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(s, d) return os.cp(s, d) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {src.string(), dst.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::is_regular_file(dst));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Trymv_MovesDirectory) {
    const fs::path temp = make_temp_dir("libxpkg-osmv-");
    const fs::path src = temp / "move_src";
    const fs::path dst = temp / "move_dst";
    fs::create_directories(src);
    write_text(src / "f.txt", "data");

    auto pkg = temp / "osmv.lua";
    write_text(pkg, std::string(
        "package = { name = \"osmv\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(s, d) return os.trymv(s, d) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {src.string(), dst.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::exists(dst / "f.txt"));
    EXPECT_FALSE(fs::exists(src));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Tryrm_RemovesDirectory) {
    const fs::path temp = make_temp_dir("libxpkg-osrm-");
    const fs::path target = temp / "to_remove";
    fs::create_directories(target / "nested");
    write_text(target / "nested" / "f.txt", "x");

    auto pkg = temp / "osrm.lua";
    write_text(pkg, std::string(
        "package = { name = \"osrm\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(p) os.tryrm(p) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {target.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_FALSE(fs::exists(target));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Mkdir_CreatesNested) {
    const fs::path temp = make_temp_dir("libxpkg-osmkdir-");
    const fs::path nested = temp / "a" / "b" / "c";

    auto pkg = temp / "osmkdir.lua";
    write_text(pkg, std::string(
        "package = { name = \"osmkdir\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(p) return os.mkdir(p) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {nested.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::is_directory(nested));
    fs::remove_all(temp);
}

TEST(ExecutorTest, OsFuncs_Isfile_DistinguishesFileAndDir) {
    const fs::path temp = make_temp_dir("libxpkg-osisfile-");
    const fs::path file = temp / "real.txt";
    write_text(file, "hi");

    auto pkg = temp / "osisfile.lua";
    write_text(pkg, std::string(
        "package = { name = \"osisfile\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(f, d)\n"
        "    if not os.isfile(f) then error('file not detected') end\n"
        "    if os.isfile(d) then error('dir detected as file') end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {file.string(), temp.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, ApplyElfpatchAuto_DisabledReturnsZeroCounts) {
    auto exec = create_executor(HELLO_PKG);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "0 0 0");
}

TEST(ExecutorTest, ApplyElfpatchAuto_WindowsSkipsPatching) {
    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-windows-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path pkg_path = temp_dir / "elfpatch-windows.lua";

    fs::create_directories(lib_dir);
    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-windows\", xpm = { windows = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.zip\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "windows"));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "0 0 0");

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, ApplyElfpatchAuto_LinuxUsesPatchelfForElf) {
#ifdef _WIN32
    GTEST_SKIP() << "Linux tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-linux-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-linux.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(tools_dir);
    fs::create_directories(lib_dir);

    write_executable_script(tools_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-linux\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "linux", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "1 1 0");

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();
    EXPECT_NE(log.find("--set-rpath " + lib_dir.string()), std::string::npos);

    fs::remove_all(temp_dir);
}

// The tag an executable's search path is stamped in decides whether the
// graphics stack works, and it is one decision that must be made in one place.
//
// `patchelf --set-rpath` writes DT_RUNPATH by default. DT_RUNPATH is consulted
// only for the object carrying it; DT_RPATH is consulted for every dlopen
// anywhere in the process. The graphics stack's load chain is three to four
// dlopens deep -- glvnd dlopens a vendor, the vendor dlopens its external
// platform modules, those dlopen their own dependencies -- so only the
// transitive tag reaches the bottom. Measured on an NVIDIA host: the same path
// content stamped DT_RPATH puts GLX, EGL, GLESv2 and headless surfaceless EGL
// all on the GPU with no environment variable and no recipe change; stamped
// DT_RUNPATH it renders on llvmpipe.
//
// Before this, the tag was whatever the last writer left. One recipe in the
// whole index flipped it by hand, and that one package was the only program in
// the stack observed to render on the GPU -- 1 of 73 installed executables had
// the right tag while 55 of the other 68 already had the right PATH.
//
// The second assertion is the load-bearing one. Forcing DT_RPATH on a LIBRARY
// is measured HARMFUL (xim-pkgindex#593): transitivity runs downward too, so a
// library's RPATH enters every lookup made beneath it, and the NVIDIA EGL
// interposer stamped that way fails eglInitialize outright. PT_INTERP is the
// predicate that separates the two, and it is the same predicate that already
// decides whether to set an interpreter.
TEST(ExecutorTest, ApplyElfpatchAuto_ForcesRpathOnExecutablesOnly) {
#ifdef _WIN32
    GTEST_SKIP() << "Linux tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-tag-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-tag.lua";

    fs::create_directories(tools_dir);
    // A lib dir must exist: it is what becomes the rpath, and with no rpath to
    // stamp `--set-rpath` is never invoked and this test would pass vacuously.
    fs::create_directories(install_dir / "lib");

    // The stub answers --print-interpreter by filename, so the test drives the
    // real predicate rather than asserting on a hardcoded branch.
    write_executable_script(tools_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n"
                            "if [ \"$1\" = \"--print-interpreter\" ]; then\n"
                            "  case \"$2\" in *demo-exe*) echo /lib64/ld-linux-x86-64.so.2 ;; esac\n"
                            "fi\n"
                            "exit 0\n");

    auto write_elf = [](const fs::path& p) {
        std::ofstream f(p, std::ios::binary);
        const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        f.write(reinterpret_cast<const char*>(magic), sizeof(magic));
        f.close();
        fs::permissions(p,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                        fs::perm_options::replace);
    };
    write_elf(install_dir / "demo-exe");
    write_elf(install_dir / "demo-lib.so");

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-tag\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "linux", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();

    // Each --set-rpath line names exactly one file; find the two and read the
    // flag off each. Searching the whole log for "--force-rpath" would pass
    // even if the flag landed on the library and not the executable.
    auto rpath_line_for = [&](const std::string& name) -> std::string {
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("--set-rpath") != std::string::npos
                && line.find(name) != std::string::npos) {
                return line;
            }
        }
        return {};
    };

    const std::string exe_line = rpath_line_for("demo-exe");
    const std::string lib_line = rpath_line_for("demo-lib.so");
    ASSERT_FALSE(exe_line.empty()) << "no --set-rpath recorded for the executable\n" << log;
    ASSERT_FALSE(lib_line.empty()) << "no --set-rpath recorded for the library\n" << log;

    EXPECT_NE(exe_line.find("--force-rpath"), std::string::npos)
        << "an executable stamped DT_RUNPATH cannot reach what it dlopens:\n" << exe_line;
    EXPECT_EQ(lib_line.find("--force-rpath"), std::string::npos)
        << "a library stamped DT_RPATH poisons every lookup beneath it "
           "(xim-pkgindex#593):\n" << lib_line;

    fs::remove_all(temp_dir);
}

// A minimal but well-formed little-endian ELF64: header, program headers, an
// optional PT_INTERP string and a PT_DYNAMIC table holding `needed` DT_NEEDED
// entries. Enough for a reader of the headers to reach a verdict; a stand-in
// that is only the ELF magic reads as UNKNOWN and is patched as before.
static void write_min_elf64(const fs::path& p, std::uint16_t machine, bool interp,
                     int needed) {
    std::string bytes(64, '\0');
    auto put = [&](std::size_t at, std::uint64_t v, int width) {
        if (bytes.size() < at + width) bytes.resize(at + width, '\0');
        for (int i = 0; i < width; ++i)
            bytes[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
    };
    const std::string interp_path = "/lib64/ld-linux-x86-64.so.2";
    const int phnum = interp ? 2 : 1;
    const std::size_t phoff = 64;
    const std::size_t interp_off = phoff + 56 * phnum;
    const std::size_t dyn_off = interp_off + (interp ? interp_path.size() + 1 : 0);
    const std::size_t dyn_size = 16 * (needed + 1);

    bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2;    // ELFCLASS64
    bytes[5] = 1;    // little endian
    bytes[6] = 1;    // EV_CURRENT
    put(16, 3, 2);             // e_type = ET_DYN
    put(18, machine, 2);       // e_machine
    put(20, 1, 4);             // e_version
    put(32, phoff, 8);         // e_phoff
    put(52, 64, 2);            // e_ehsize
    put(54, 56, 2);            // e_phentsize
    put(56, phnum, 2);         // e_phnum

    std::size_t ph = phoff;
    if (interp) {
        put(ph + 0, 3, 4);                          // PT_INTERP
        put(ph + 8, interp_off, 8);                 // p_offset
        put(ph + 32, interp_path.size() + 1, 8);    // p_filesz
        ph += 56;
    }
    put(ph + 0, 2, 4);                              // PT_DYNAMIC
    put(ph + 8, dyn_off, 8);
    put(ph + 32, dyn_size, 8);

    if (interp) {
        bytes.resize(interp_off);
        bytes += interp_path;
        bytes.push_back('\0');
    }
    bytes.resize(dyn_off + dyn_size, '\0');
    for (int i = 0; i < needed; ++i) put(dyn_off + 16 * i, 1, 8);  // DT_NEEDED

    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    fs::permissions(p,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
}

// libxpkg#43. Three kinds of file are never handed to patchelf, whatever the
// scan finds: one with neither PT_INTERP nor DT_NEEDED (static-pie; writing an
// RPATH into one made it exit 139 before main), one built for another machine
// than the loader being written, and one the recipe named in `skip`. What IS
// patched is unchanged: the executable and the library next to them.
//
// The ABI comes from the loader file, so the verdict does not depend on the
// machine the test runs on (the context below even claims arm64).
TEST(ExecutorTest, ElfpatchGate_LeavesStaticForeignAndSkippedFilesAlone) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    constexpr std::uint16_t kX86_64 = 62;
    constexpr std::uint16_t kAarch64 = 183;

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-gate-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path loader = temp_dir / "loader" / "ld-linux-x86-64.so.2";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-gate.lua";

    fs::create_directories(tools_dir);
    write_executable_script(tools_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n"
                            "if [ \"$1\" = \"--print-interpreter\" ]; then\n"
                            "  case \"$2\" in *app*|*skipme*) echo /lib64/ld-linux-x86-64.so.2 ;; esac\n"
                            "fi\n"
                            "exit 0\n");

    write_min_elf64(loader, kX86_64, /*interp=*/false, /*needed=*/0);
    write_min_elf64(install_dir / "bin" / "app", kX86_64, true, 1);
    write_min_elf64(install_dir / "lib" / "libfoo.so", kX86_64, false, 1);
    write_min_elf64(install_dir / "resources" / "static-helper", kX86_64, false, 0);
    write_min_elf64(install_dir / "prebuilds" / "linux-arm64" / "addon.node",
                    kAarch64, false, 1);
    write_min_elf64(install_dir / "bin" / "skipme", kX86_64, true, 1);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-gate\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.set({ interpreter = \"" + loader.generic_string() + "\",\n"
               "                   skip = { \"./bin/skipme\" } })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install,
                                      make_context(install_dir, "linux", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    ASSERT_TRUE(patch_result.success) << patch_result.error;
    // scanned patched failed: all five are seen, two are patched.
    EXPECT_EQ(patch_result.output, "5 2 0");

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();

    auto touched = [&](const std::string& name) {
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("--set-") != std::string::npos
                && line.find(name) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(touched("bin/app")) << log;
    EXPECT_TRUE(touched("lib/libfoo.so")) << log;
    EXPECT_FALSE(touched("static-helper")) << log;
    EXPECT_FALSE(touched("addon.node")) << log;
    EXPECT_FALSE(touched("skipme")) << log;

    fs::remove_all(temp_dir);
}

// `scan` narrows the walk: a file outside the listed paths is not even seen.
TEST(ExecutorTest, ElfpatchGate_ScanLimitsTheWalk) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    constexpr std::uint16_t kX86_64 = 62;

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-scan-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path loader = temp_dir / "loader" / "ld-linux-x86-64.so.2";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-scan.lua";

    fs::create_directories(tools_dir);
    write_executable_script(tools_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n"
                            "exit 0\n");

    write_min_elf64(loader, kX86_64, false, 0);
    write_min_elf64(install_dir / "lib" / "libin.so", kX86_64, false, 1);
    write_min_elf64(install_dir / "extras" / "libout.so", kX86_64, false, 1);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-scan\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.set({ interpreter = \"" + loader.generic_string() + "\",\n"
               "                   scan = { \"lib/\" } })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto hook_result = exec->run_hook(HookType::Install,
                                      make_context(install_dir, "linux", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    ASSERT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "1 1 0");

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();
    EXPECT_NE(log.find("libin.so"), std::string::npos) << log;
    EXPECT_EQ(log.find("libout.so"), std::string::npos) << log;

    fs::remove_all(temp_dir);
}

// A driver vendor library is the host's file: a symlink into /usr/lib, coupled
// to the host's kernel module, and not ours to put an RPATH on. The historical
// answer was to put OUR libraries on LD_LIBRARY_PATH so the vendor could find
// its dependencies -- which also handed them to every other process in the
// subos, including host binaries on the host loader. That is how
// `xlings subos use` once returned a /bin/bash that died of SIGSEGV before
// printing a character.
//
// host_link_interposer does the same job with a scope of exactly one object.
// Measured on a real NVIDIA stack on 2026-08-06: with LD_LIBRARY_PATH carrying
// only the host driver directory, GL_RENDERER came back as the RTX 4080 and
// the probe read back the pixel it drew; the same subos without it renders on
// llvmpipe.
//
// These cover the SHAPE of the produced object. Each of the three assertions
// below exists because its absence produces an interposer that loads perfectly
// and does nothing: a wrong SONAME is never asked for, a missing NEEDED leaves
// dlsym with no entry point (the caller reports "no device", not an error),
// and DT_RUNPATH instead of DT_RPATH is not transitive so the vendor's own
// dependencies fall through to the host.
TEST(ExecutorTest, HostLinkInterposer_ShapeIsAssertedNotAssumed) {
#ifdef _WIN32
    GTEST_SKIP() << "ELF-specific";
#endif
    const fs::path temp_dir = make_temp_dir("libxpkg-interposer-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path libdir = install_dir / "lib";
    const fs::path tools = temp_dir / "tools";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "interposer.lua";
    const fs::path stub = temp_dir / "stub.so";
    const fs::path vendor = temp_dir / "libFAKE_vendor.so.550";

    fs::create_directories(libdir);
    fs::create_directories(tools);

    // A fake patchelf that records its arguments and answers the three
    // --print-* queries from what it was told to set. That is enough to prove
    // the call sequence and that the result is CHECKED; whether real patchelf
    // writes a valid ELF is real patchelf's business.
    write_executable_script(tools / "patchelf",
        "#!/bin/sh\n"
        "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n"
        "case \"$1\" in\n"
        "  --set-soname) echo \"$2\" > \"$ELFPATCH_STATE.soname\" ;;\n"
        "  --add-needed) echo \"$2\" >> \"$ELFPATCH_STATE.needed\" ;;\n"
        "  --set-rpath)  echo \"$2\" > \"$ELFPATCH_STATE.rpath\" ;;\n"
        "  --print-soname) cat \"$ELFPATCH_STATE.soname\" 2>/dev/null ;;\n"
        "  --print-needed) cat \"$ELFPATCH_STATE.needed\" 2>/dev/null ;;\n"
        "  --print-rpath)  cat \"$ELFPATCH_STATE.rpath\"  2>/dev/null ;;\n"
        "esac\n"
        "exit 0\n");

    write_text(stub, "\x7f" "ELF-stub-placeholder\n");
    write_text(vendor, "\x7f" "ELF-vendor-placeholder\n");

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"interposer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
        "function install()\n"
        "    elfpatch.host_link_interposer{\n"
        "        vendor  = \"" + vendor.string() + "\",\n"
        "        out     = \"" + (libdir / "libFAKE_vendor.so.0").string() + "\",\n"
        "        stub    = \"" + stub.string() + "\",\n"
        "        libdirs = { \"/payload/a/lib\", \"/payload/b/lib64\" },\n"
        "    }\n"
        "    return true\n"
        "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());
    ScopedEnvVar st_env("ELFPATCH_STATE", (temp_dir / "state").string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(install_dir, "linux", tools);
    auto hook_result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    EXPECT_TRUE(fs::exists(libdir / "libFAKE_vendor.so.0"))
        << "the interposer was not produced";

    std::ifstream lf(log_path);
    std::ostringstream lb; lb << lf.rdbuf();
    const std::string log = lb.str();

    // The SONAME is the vendor's, so whoever asks for it gets this instead.
    EXPECT_NE(log.find("--set-soname libFAKE_vendor.so.0"), std::string::npos)
        << log;
    // The real vendor by ABSOLUTE path -- dlsym searches the handle's whole
    // dependency tree, which is how glvnd still reaches the real entry points.
    EXPECT_NE(log.find("--add-needed " + vendor.string()), std::string::npos)
        << log;
    // --force-rpath, not RUNPATH. DT_RPATH is transitive along the load chain
    // and DT_RUNPATH is not; that difference is the whole mechanism.
    EXPECT_NE(log.find("--set-rpath /payload/a/lib:/payload/b/lib64"),
              std::string::npos) << log;
    EXPECT_NE(log.find("--force-rpath"), std::string::npos) << log;

    fs::remove_all(temp_dir);
}

// An empty closure means the vendor's dependencies would resolve from the
// HOST, which is the single thing this function exists to prevent. Failing the
// install is the only outcome that is not a silent success: the interposer
// would load, the GPU would work by accident, and every library it pulled in
// would be the host's.
TEST(ExecutorTest, HostLinkInterposer_RefusesAnEmptyClosure) {
#ifdef _WIN32
    GTEST_SKIP() << "ELF-specific";
#endif
    const fs::path temp_dir = make_temp_dir("libxpkg-interposer-empty-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "interposer.lua";
    const fs::path stub = temp_dir / "stub.so";
    const fs::path vendor = temp_dir / "libFAKE_vendor.so.550";
    fs::create_directories(install_dir);
    write_text(stub, "stub\n");
    write_text(vendor, "vendor\n");

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"interposer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
        "function install()\n"
        "    elfpatch.host_link_interposer{\n"
        "        vendor = \"" + vendor.string() + "\",\n"
        "        out    = \"" + (install_dir / "x.so").string() + "\",\n"
        "        stub   = \"" + stub.string() + "\",\n"
        "        libdirs = {},\n"
        "    }\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto r = exec->run_hook(HookType::Install, make_context(install_dir, "linux"));
    EXPECT_FALSE(r.success)
        << "an empty closure resolves the vendor's dependencies from the host";

    fs::remove_all(temp_dir);
}

// A vendor that is not there. An interposer NEEDing a missing file loads
// exactly as successfully as one NEEDing nothing, and the caller reports "no
// device" -- a machine without this driver must fail at install, where the
// message can say so.
// The vendor's OWN dependency closure has to be checked, not just the shape of
// the object we made.
//
// The three assertions that were here (soname / needed / rpath present) can all
// hold while the interposer does nothing useful: an RPATH naming directories
// that do not contain the vendor's DT_NEEDED resolves them from the host, which
// still renders -- on llvmpipe -- and says nothing. Warn rather than fail: the
// object IS correctly built and the host's ld.so.cache makes it work outside a
// sandbox, so refusing would break a working install. Being invisible is the
// defect.
TEST(ExecutorTest, HostLinkInterposer_ReportsAnUnservedVendorClosure) {
#ifdef _WIN32
    GTEST_SKIP() << "ELF-specific";
#endif
    const fs::path temp_dir = make_temp_dir("libxpkg-interposer-closure-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path libdir = install_dir / "lib";
    const fs::path pkg_path = temp_dir / "interposer.lua";
    const fs::path stub = temp_dir / "stub.so";
    const fs::path vendor = temp_dir / "libFAKE_vendor.so.550";
    const fs::path tools = temp_dir / "tools";
    const fs::path served = temp_dir / "served";
    fs::create_directories(libdir);
    fs::create_directories(tools);
    fs::create_directories(served);

    // One of the two SONAMEs the vendor needs is present in the closure; the
    // other is not. A fraction, so a pass cannot be produced by an empty list.
    write_text(served / "libserved.so.1", "x\n");

    // A fake patchelf that answers --print-needed differently for the VENDOR
    // than for the object under construction. The previous fake could not tell
    // them apart, which is exactly why this check could not have been tested
    // with it.
    write_executable_script(tools / "patchelf",
        "#!/bin/sh\n"
        "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n"
        "case \"$1\" in\n"
        "  --set-soname) echo \"$2\" > \"$ELFPATCH_STATE.soname\" ;;\n"
        "  --add-needed) echo \"$2\" >> \"$ELFPATCH_STATE.needed\" ;;\n"
        "  --set-rpath)  echo \"$2\" > \"$ELFPATCH_STATE.rpath\" ;;\n"
        "  --print-soname) cat \"$ELFPATCH_STATE.soname\" 2>/dev/null ;;\n"
        "  --print-needed)\n"
        "     case \"$2\" in\n"
        "       *libFAKE_vendor.so.550) printf 'libserved.so.1\\nlibmissing.so.7\\n' ;;\n"
        "       *) cat \"$ELFPATCH_STATE.needed\" 2>/dev/null ;;\n"
        "     esac ;;\n"
        "  --print-rpath)  cat \"$ELFPATCH_STATE.rpath\"  2>/dev/null ;;\n"
        "esac\n"
        "exit 0\n");

    write_text(stub, "\x7f" "ELF-stub-placeholder\n");
    write_text(vendor, "\x7f" "ELF-vendor-placeholder\n");

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"interposer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
        "function install()\n"
        "    local r = elfpatch.host_link_interposer{\n"
        "        vendor  = \"" + vendor.string() + "\",\n"
        "        out     = \"" + (libdir / "libFAKE_vendor.so.0").string() + "\",\n"
        "        stub    = \"" + stub.string() + "\",\n"
        "        libdirs = { \"" + served.string() + "\" },\n"
        "    }\n"
        "    assert(#r.unresolved == 1, \"expected one unresolved dep\")\n"
        "    assert(r.unresolved[1] == \"libmissing.so.7\", r.unresolved[1])\n"
        "    return true\n"
        "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", (temp_dir / "log.txt").string());
    ScopedEnvVar st_env("ELFPATCH_STATE", (temp_dir / "state").string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(install_dir, "linux", tools);
    auto hook_result = exec->run_hook(HookType::Install, ctx);
    // The install SUCCEEDS -- the unresolved entry is reported, not fatal.
    ASSERT_TRUE(hook_result.success) << hook_result.error;
    EXPECT_TRUE(fs::exists(libdir / "libFAKE_vendor.so.0"));

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, Elfpatch_ClosureRemainsPayloadDirectAcrossScopes) {
    const fs::path tempDir = make_temp_dir("libxpkg-elfpatch-scope-");
    const fs::path installDir = tempDir / "payload";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(installDir);
    write_text(pkgPath, R"lua(
        package = { spec = "1", name = "consumer",
            xpm = { linux = { ["1.0.0"] = {} } } }
        local elfpatch = import("xim.libxpkg.elfpatch")
        function config()
            local paths = elfpatch.closure_lib_paths()
            assert(#paths == 3, "unexpected closure: " .. table.concat(paths, ":"))
            assert(paths[1] == "/store/consumer/lib")
            assert(paths[2] == "/store/declared/lib")
            assert(paths[3] == "/store/resolved/lib")
            local explicit = elfpatch.closure_lib_paths({ deps_list = {} })
            assert(#explicit == 1 and explicit[1] == paths[1],
                "empty dependency override included scope libraries")
            return true
        end
    )lua");
    for (const auto scope : {"/home/subos/first", "/home/subos/second"}) {
        auto exec = create_executor(pkgPath);
        ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
        auto ctx = make_context(installDir, "linux");
        ctx.subos_sysrootdir = scope;
        ctx.self_exports.libdirs = {"/store/consumer/lib"};
        ctx.deps_list = {"declared@1", "resolved@1", "build@1"};
        ctx.runtime_deps_list = {"declared@1", "resolved@1"};
        ctx.build_deps_list = {"build@1"};
        ctx.deps_exports["declared@1"].libdirs = {"/store/declared/lib"};
        ctx.resolved_deps["resolved@1"].libdirs = {"/store/resolved/lib"};
        ctx.resolved_deps["build@1"].libdirs = {"/store/build/lib"};
        const auto result = exec->run_hook(HookType::Config, ctx);
        EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    }
    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_UsesExplicitDependencyStoreRoots) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-roots-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path registryPayload =
        registryRoot / "compat-x-zlib" / "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path decoyPayload = tempDir / "member" / "data" / "xpkgs" /
                                  "other-x-zlib" / "1.3.2";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(registryPayload);
    fs::create_directories(memberPayload);
    fs::create_directories(decoyPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == \"" + registryPayload.string() +
            "\", \"explicit root mismatch: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {registryRoot};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExplicitDependencyStoreRootsPreserveOrder) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-root-order-");
    const fs::path firstRoot = tempDir / "first" / "data" / "xpkgs";
    const fs::path secondRoot = tempDir / "second" / "data" / "xpkgs";
    const fs::path firstPayload = firstRoot / "compat-x-zlib" / "1.3.2";
    const fs::path secondPayload = secondRoot / "compat-x-zlib" / "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(firstPayload);
    fs::create_directories(secondPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == \"" + firstPayload.string() +
            "\", \"root order mismatch: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {firstRoot, secondRoot};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExactResolverRecordWinsOverExplicitRoots) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-record-wins-");
    const fs::path root = tempDir / "registry" / "data" / "xpkgs";
    const fs::path rootPayload = root / "compat-x-zlib" / "1.3.2";
    const fs::path recordPayload = tempDir / "resolved" / "compat-x-zlib" /
                                   "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(rootPayload);
    fs::create_directories(recordPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == \"" + recordPayload.string() +
            "\", \"resolver record lost authority: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps["compat:zlib@1.3.2"] = ResolvedDep {
        .spec = "compat:zlib@1.3.2",
        .name = "compat:zlib",
        .version = "1.3.2",
        .install_dir = recordPayload.string(),
        .libdirs = {},
        .source = "plan",
    };
    ctx.dependency_store_roots = {root};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_UniqueBareNameUsesExactResolvedRecord) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-record-");
    const fs::path recordPayload = tempDir / "resolved" / "compat-x-zlib" /
                                   "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(recordPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.install_dir(\"zlib\", \"1.3.2\")\n"
        "    assert(got == \"" + recordPayload.string() +
            "\", \"unique bare record mismatch: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps["compat:zlib@>=1.0"] = ResolvedDep {
        .spec = "compat:zlib@>=1.0",
        .name = "compat:zlib",
        .version = "1.3.2",
        .install_dir = recordPayload.string(),
        .libdirs = {},
        .source = "plan",
    };
    ctx.dependency_store_roots = {};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_BareNameRejectsResolvedNamespaceCollision) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-collision-");
    const fs::path compatPayload = tempDir / "resolved" / "compat-x-zlib" /
                                   "1.3.2";
    const fs::path otherPayload = tempDir / "resolved" / "other-x-zlib" /
                                  "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(compatPayload);
    fs::create_directories(otherPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"namespace collision chose: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps["compat:zlib@>=1.0"] = ResolvedDep {
        .spec = "compat:zlib@>=1.0",
        .name = "compat:zlib",
        .version = "1.3.2",
        .install_dir = compatPayload.string(),
        .libdirs = {},
        .source = "plan",
    };
    ctx.resolved_deps["other:zlib@1.3.2"] = ResolvedDep {
        .spec = "other:zlib@1.3.2",
        .name = "other:zlib",
        .version = "1.3.2",
        .install_dir = otherPayload.string(),
        .libdirs = {},
        .source = "plan",
    };
    ctx.dependency_store_roots = {};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExplicitRootsRejectBareNameRequests) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-root-");
    const fs::path root = tempDir / "registry" / "data" / "xpkgs";
    const fs::path namespacedPayload = root / "compat-x-zlib" / "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(namespacedPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"bare root lookup chose: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {root};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_InvalidExactRecordFailsWithoutRootFallback) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-invalid-record-");
    const fs::path root = tempDir / "registry" / "data" / "xpkgs";
    const fs::path rootPayload = root / "compat-x-zlib" / "1.3.2";
    const fs::path missingPayload = tempDir / "missing" / "compat-x-zlib" /
                                    "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(rootPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"invalid record fell through: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.deps_list = {"compat:zlib@1.3.2"};
    ctx.resolved_deps["compat:zlib@1.3.2"] = ResolvedDep {
        .spec = "compat:zlib@1.3.2",
        .name = "compat:zlib",
        .version = "1.3.2",
        .install_dir = missingPayload.string(),
        .libdirs = {},
        .source = "plan",
    };
    ctx.dependency_store_roots = {root};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(result.output.find("resolver record"), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("missing payload"), std::string::npos)
        << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExplicitRootsRejectWrongNamespaceAndLegacyDecoy) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-namespace-");
    const fs::path root = tempDir / "registry" / "data" / "xpkgs";
    const fs::path wrongNamespacePayload =
        root / "other-x-zlib" / "1.3.2";
    const fs::path memberStore = tempDir / "member" / "data" / "xpkgs";
    const fs::path memberPayload = memberStore / "consumer" / "1.0.0";
    const fs::path legacyDecoy = memberStore / "compat-x-zlib" / "1.3.2";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(wrongNamespacePayload);
    fs::create_directories(memberPayload);
    fs::create_directories(legacyDecoy);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"wrong namespace or legacy decoy won: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {root};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExplicitRootsDoNotInferMcppHome) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-mcpp-home-");
    const fs::path root = tempDir / "authorized" / "data" / "xpkgs";
    const fs::path unrelatedHome = tempDir / "unrelated-mcpp-home";
    const fs::path unrelatedPayload = unrelatedHome / "registry" / "data" /
                                      "xpkgs" / "compat-x-zlib" / "1.3.2";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(root);
    fs::create_directories(unrelatedPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"MCPP_HOME leaked into roots: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {root};
    ScopedEnvVar mcppHome("MCPP_HOME", unrelatedHome.string());
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_ExplicitRootsRejectWildcardPartialAndRangeVersions) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-exact-version-");
    const fs::path root = tempDir / "registry" / "data" / "xpkgs";
    const fs::path depRoot = root / "compat-x-zlib";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(depRoot / "1.3.2");
    fs::create_directories(depRoot / "1.x");
#ifndef _WIN32
    fs::create_directories(depRoot / "*");
    fs::create_directories(depRoot / "1.3.*");
#endif
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    for _, version in ipairs({\"*\", \"1.x\", \"1.3.*\", \">=1.0\"}) do\n"
        "        local got = pkginfo.dep_install_dir(\"compat:zlib\", version)\n"
        "        assert(got == nil, version .. \" selected a payload: \" .. tostring(got))\n"
        "    end\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    ctx.dependency_store_roots = {root};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(tempDir);
}

TEST(ExecutorTest, PkgInfo_MissingRootsFieldPreservesLegacyScanWithOneWarning) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-legacy-");
    const fs::path memberStore = tempDir / "member" / "data" / "xpkgs";
    const fs::path memberPayload = memberStore / "consumer" / "1.0.0";
    const fs::path legacyPayload = memberStore / "compat-x-zlib" / "1.3.2";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(memberPayload);
    fs::create_directories(legacyPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    _RUNTIME.dependency_store_roots = nil\n"
        "    local got = pkginfo.install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == \"" + legacyPayload.string() +
            "\", \"legacy scan mismatch: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.resolved_deps = {};
    const auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    constexpr std::string_view warning = "fell back to a scan";
    EXPECT_NE(result.output.find(warning), std::string::npos) << result.output;
    EXPECT_EQ(result.output.find(warning), result.output.rfind(warning))
        << "legacy fallback must emit exactly one warning:\n" << result.output;
    EXPECT_LE(result.output.size(), kMaxHookOutputBytes + 64);

    fs::remove_all(tempDir);
}

// `install_dir` for a package that is not a dependency here must SAY that.
//
// openxlings/xlings#487: a macOS install of ollama reported "cannot get
// install dir for xim:libcuda-host-link@0.0.1" -- a linux-only sentinel that
// macOS never resolves, asked for by a hook whose branch tested
// `is_host("windows")` when the real distinction was linux. "cannot" names an
// internal state and covers two causes that point opposite ways: a broken
// path, and a package that was never a dependency here. The reader went
// looking at paths.
//
// The hook still returns nil either way -- this is about which of the two the
// message names.
TEST(ExecutorTest, InstallDir_NotADependencyHere_SaysSoRatherThanCannot) {
    const fs::path temp_dir = make_temp_dir("libxpkg-installdir-why-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "consumer.lua";
    fs::create_directories(install_dir);

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local d = pkginfo.install_dir(\"xim:linux-only-thing\", \"0.0.1\")\n"
        "    if d then error(\"expected nil for a package that is not a dep\") end\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(install_dir, "macosx");
    ctx.deps_list = {"xim:something-else@1.0.0"};
    // The Lua `log.error` goes to the process's stdout, not HookResult::output.
    testing::internal::CaptureStdout();
    auto r = exec->run_hook(HookType::Install, ctx);
    const std::string out = testing::internal::GetCapturedStdout() + r.output;
    EXPECT_TRUE(r.success) << "the hook itself is fine; only the message changes";
    EXPECT_NE(out.find("NOT a dependency"), std::string::npos)
        << "the message must name the cause, not the internal state. got:\n" << out;
    EXPECT_NE(out.find("linux-only-thing"), std::string::npos)
        << "the message must name the package asked for. got:\n" << out;
    EXPECT_NE(out.find("something-else"), std::string::npos)
        << "the message must list what IS a dep here, so the reader can see "
           "the platform split. got:\n" << out;

    fs::remove_all(temp_dir);
}

// A package that IS declared here but has no payload is the OTHER cause, and
// it must not be described as a platform mismatch -- that would send the
// reader to the recipe's platform sections when the dependency install is
// what failed.
TEST(ExecutorTest, InstallDir_DeclaredButAbsent_NamesTheIncompleteInstall) {
    const fs::path temp_dir = make_temp_dir("libxpkg-installdir-absent-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "consumer.lua";
    fs::create_directories(install_dir);

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    pkginfo.install_dir(\"xim:declared-thing\", \"0.0.1\")\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(install_dir, "linux");
    ctx.deps_list = {"xim:declared-thing@0.0.1"};
    testing::internal::CaptureStdout();
    auto r = exec->run_hook(HookType::Install, ctx);
    const std::string out = testing::internal::GetCapturedStdout() + r.output;
    EXPECT_TRUE(r.success);
    EXPECT_NE(out.find("declared as a dependency"), std::string::npos)
        << "got:\n" << out;
    EXPECT_EQ(out.find("NOT a dependency"), std::string::npos)
        << "a declared-but-absent dep must not be reported as a platform "
           "mismatch. got:\n" << out;

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, HostLinkInterposer_RefusesAMissingVendor) {
#ifdef _WIN32
    GTEST_SKIP() << "ELF-specific";
#endif
    const fs::path temp_dir = make_temp_dir("libxpkg-interposer-novendor-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "interposer.lua";
    const fs::path stub = temp_dir / "stub.so";
    fs::create_directories(install_dir);
    write_text(stub, "stub\n");

    write_text(pkg_path,
        "package = { spec = \"1\", name = \"interposer\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/d.tar.gz\", sha256 = \"0\" } } } }\n"
        "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
        "function install()\n"
        "    elfpatch.host_link_interposer{\n"
        "        vendor = \"" + (temp_dir / "does-not-exist.so").string() + "\",\n"
        "        out    = \"" + (install_dir / "x.so").string() + "\",\n"
        "        stub   = \"" + stub.string() + "\",\n"
        "        libdirs = { \"/p/lib\" },\n"
        "    }\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto r = exec->run_hook(HookType::Install, make_context(install_dir, "linux"));
    EXPECT_FALSE(r.success) << "an interposer pointing at a missing vendor";

    fs::remove_all(temp_dir);
}

// A downloaded prebuilt carries the build machine's absolute paths in its text
// files. glibc's recipe knew this and rewrote them, and got all three parts of
// the job wrong -- so this is the shared capability that replaces it.
//
// The fixture reproduces the real damage byte for byte. glibc's pattern was
// `([^%s)]+)/<marker>/lib`, whose `[^%s)]+` runs leftward through anything that
// is not whitespace or `)`. On the shipped `bin/ldd` it swallowed `RTLDLIST="`
// along with the path, and the ldd in the 2.39 and 2.44 payloads on disk today
// does not survive `bash -n`. It also anchored the tail at `/lib`, so the same
// file's `share/locale` line was left alone -- a build path still in the
// artifact, which was the only thing the code existed to remove.
TEST(ExecutorTest, RelocateBuildPaths_AnchorsTheTokenAndAssertsTheResult) {
#ifdef _WIN32
    GTEST_SKIP() << "Shell syntax check is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-relocate-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "relocate.lua";
    const std::string marker = "fromsource-x-glibc/2.39";
    const std::string built = "/home/xlings/.xlings_data/xim/xpkgs/" + marker;

    fs::create_directories(install_dir / "bin");
    fs::create_directories(install_dir / "lib");
    fs::create_directories(install_dir / "lib/pkgconfig");

    // The exact two lines that broke, in the order they appear in ldd.
    write_executable_script(install_dir / "bin/ldd",
        "#! /bin/bash\n"
        "TEXTDOMAIN=libc\n"
        "TEXTDOMAINDIR=" + built + "/share/locale\n"
        "RTLDLIST=\"" + built + "/lib/ld-linux.so.2 "
                      + built + "/lib64/ld-linux-x86-64.so.2 "
                      + built + "/libx32/ld-linux-x32.so.2\"\n"
        "case \"$1\" in\n"
        "  --version) printf $\"Copyright (C) %s\\n\" \"2024\" ;;\n"
        "esac\n");

    // A linker script: the path sits inside parentheses, which the old
    // pattern's stop set treated specially and this one does not need to.
    write_text(install_dir / "lib/libc.so",
        "/* GNU ld script */\n"
        "GROUP ( " + built + "/lib/libc.so.6 " + built + "/lib/libc_nonshared.a"
        " AS_NEEDED ( " + built + "/lib/ld-linux-x86-64.so.2 ) )\n");

    // NOT on glibc's six-file list. Enumeration is the point: a list of what
    // someone thought of is not a measurement of what is there.
    write_text(install_dir / "lib/pkgconfig/libc.pc",
        "prefix=" + built + "\n"
        "libdir=${prefix}/lib\n"
        "Name: libc\n");

    // A binary holding the same bytes. Rewriting it would change its length
    // and corrupt it; that is patchelf's job, not a text substitution's.
    const std::string binary_body = std::string("\x7f", 1) + "ELF" +
        std::string("\0\0\0\0", 4) + built + "/lib\n";
    {
        std::ofstream b(install_dir / "lib/probe.so", std::ios::binary);
        b.write(binary_body.data(),
                static_cast<std::streamsize>(binary_body.size()));
    }

    // A relative reference, already correct. Rewriting it would invent an
    // absolute path where the file deliberately has none.
    write_text(install_dir / "lib/relative.txt", "./" + marker + "/lib\n");

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"relocate\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
               "function install()\n"
               "    elfpatch.relocate_build_paths{ marker = \"" + marker + "\" }\n"
               "    return true\n"
               "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install,
                                      make_context(install_dir, "linux"));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    const auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf(); return ss.str();
    };

    const auto ldd = read(install_dir / "bin/ldd");
    // The assignment survived. This is the whole regression.
    EXPECT_NE(ldd.find("RTLDLIST=\""), std::string::npos)
        << "the shell assignment was swallowed with the path:\n" << ldd;
    EXPECT_EQ(ldd.find(built), std::string::npos)
        << "a build path is still in the artifact:\n" << ldd;
    // All three loader dirs, including the two the /lib anchor mangled.
    EXPECT_NE(ldd.find(install_dir.string() + "/lib/ld-linux.so.2"),
              std::string::npos) << ldd;
    EXPECT_NE(ldd.find(install_dir.string() + "/lib64/ld-linux-x86-64.so.2"),
              std::string::npos) << ldd;
    EXPECT_NE(ldd.find(install_dir.string() + "/libx32/ld-linux-x32.so.2"),
              std::string::npos) << ldd;
    // The line the /lib anchor never reached.
    EXPECT_NE(ldd.find(install_dir.string() + "/share/locale"),
              std::string::npos) << ldd;

    // And it still parses. The payload on disk today does not.
    EXPECT_EQ(std::system(("bash -n " + (install_dir / "bin/ldd").string()
                           + " 2>/dev/null").c_str()), 0)
        << "the rewritten script no longer parses:\n" << ldd;

    const auto libc_so = read(install_dir / "lib/libc.so");
    EXPECT_EQ(libc_so.find(built), std::string::npos) << libc_so;
    EXPECT_NE(libc_so.find(install_dir.string() + "/lib/libc.so.6"),
              std::string::npos) << libc_so;
    // The closing parens of the linker script survived.
    EXPECT_NE(libc_so.find(") )"), std::string::npos) << libc_so;

    const auto pc = read(install_dir / "lib/pkgconfig/libc.pc");
    EXPECT_EQ(pc.find(built), std::string::npos)
        << "a file that was not on the old hand-written list kept its build "
           "path:\n" << pc;

    EXPECT_EQ(read(install_dir / "lib/probe.so"), binary_body)
        << "a binary was rewritten as text";
    EXPECT_EQ(read(install_dir / "lib/relative.txt"), "./" + marker + "/lib\n")
        << "a deliberately relative reference was made absolute";

    fs::remove_all(temp_dir);
}

// The assertion half. A rewrite that corrupts a script must fail the install,
// not report success -- glibc's version treated "we wrote something" as
// success, so "still has build paths" and "we broke the file" both produced
// exactly the output of a clean run: nothing.
TEST(ExecutorTest, RelocateBuildPaths_FailsWhenAMarkerIsMissing) {
#ifdef _WIN32
    GTEST_SKIP() << "Shell syntax check is POSIX-specific";
#endif
    const fs::path temp_dir = make_temp_dir("libxpkg-relocate-nomarker-");
    const fs::path install_dir = temp_dir / "install";
    const fs::path pkg_path = temp_dir / "relocate.lua";
    fs::create_directories(install_dir);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"relocate\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.relocate_build_paths{}\n"
               "    return true\n"
               "end\n");

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto hook_result = exec->run_hook(HookType::Install,
                                      make_context(install_dir, "linux"));
    EXPECT_FALSE(hook_result.success)
        << "relocation without a marker would rewrite any absolute path in "
           "the payload, including legitimate references to /usr";

    fs::remove_all(temp_dir);
}

// patchelf is what stamps INTERP and RPATH onto every payload we ship, so
// "which patchelf" decides what our artifacts look like -- versions differ in
// how they grow the dynamic segment and in --force-rpath semantics.
//
// It used to be decided by a mutable view: subos/<name>/bin first, then the
// home's bin, then /usr/bin, then PATH, with the payload not a candidate at
// all. Every one of those resolves to the same file in the default
// configuration, which is why it survived -- the answers agree by coincidence
// until a second home, a second version, or a host install exists.
//
// Two patchelf binaries here, distinguishable only by what they log. The
// payload one is not on PATH and not in bin_dir; the view one is both. If the
// payload does not win, the assertion below fails on the marker.
// See the architecture proposal's R6 (internal consumers bind the payload).
TEST(ExecutorTest, FindTool_PrefersPayloadOverViewAndHost) {
#ifdef _WIN32
    GTEST_SKIP() << "Tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-findtool-payload-");
    const fs::path store = temp_dir / "xpkgs";
    const fs::path payload_bin = store / "xim-x-patchelf" / "0.18.0" / "bin";
    const fs::path view_dir = temp_dir / "tools";
    const fs::path install_dir = store / "xim-x-findtool" / "1.0.0";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "findtool.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(payload_bin);
    fs::create_directories(view_dir);
    fs::create_directories(lib_dir);

    write_executable_script(payload_bin / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'PAYLOAD %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");
    write_executable_script(view_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'VIEW %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"findtool\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", view_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto ctx = make_context(install_dir, "linux", view_dir);
    ctx.xpkg_dir = store;
    auto hook_result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();

    EXPECT_NE(log.find("PAYLOAD"), std::string::npos)
        << "the payload patchelf never ran; log was:\n" << log;
    EXPECT_EQ(log.find("VIEW"), std::string::npos)
        << "the view's patchelf ran even though a payload exists. The tool that "
           "stamps INTERP and RPATH must not be selected by a mutable view.\n"
           "log was:\n" << log;

    fs::remove_all(temp_dir);
}

// The other half of the contract: with no payload in the store, the view is
// still usable. A home whose store predates this change has to keep working --
// the change is which answer WINS, not the removal of the others.
TEST(ExecutorTest, FindTool_FallsBackToViewWhenNoPayloadExists) {
#ifdef _WIN32
    GTEST_SKIP() << "Tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-findtool-fallback-");
    const fs::path store = temp_dir / "xpkgs";
    const fs::path view_dir = temp_dir / "tools";
    const fs::path install_dir = store / "xim-x-findtool" / "1.0.0";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "findtool.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(view_dir);
    fs::create_directories(lib_dir);

    write_executable_script(view_dir / "patchelf",
                            "#!/bin/sh\n"
                            "printf 'VIEW %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"findtool\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", view_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto ctx = make_context(install_dir, "linux", view_dir);
    ctx.xpkg_dir = store;
    auto hook_result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();
    EXPECT_NE(log.find("VIEW"), std::string::npos)
        << "no payload and no view means no patching at all; log was:\n" << log;

    fs::remove_all(temp_dir);
}

// Regression: patchelf 0.18.0 corrupts compact ELFs (e.g. ninja 1.12.1
// at 273 KB) when --set-interpreter runs before --set-rpath. The interp
// op extends PT_LOAD and shifts the dynamic section; the subsequent
// rpath op operates on stale offsets and writes through DT_NEEDED,
// segfaulting the binary at execve+1. Workaround: rpath must run first,
// interp second. See docs/plans/2026-05-03-patchelf-order-bug-analysis.md.
TEST(ExecutorTest, ApplyElfpatchAuto_LinuxRpathBeforeInterpreter) {
#ifdef _WIN32
    GTEST_SKIP() << "Linux tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-order-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-order.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(tools_dir);
    fs::create_directories(lib_dir);

    // Fake patchelf logs every invocation; --print-interpreter returns
    // a non-empty string so _has_pt_interp treats the file as having
    // PT_INTERP (we want to exercise both ops on the same file).
    write_executable_script(tools_dir / "patchelf",
                            "#!/bin/sh\n"
                            "if [ \"$1\" = \"--print-interpreter\" ]; then\n"
                            "  echo /lib64/ld-linux-x86-64.so.2\n"
                            "  exit 0\n"
                            "fi\n"
                            "printf 'patchelf %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-order\", xpm = { linux = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               // explicit interpreter so we bypass _resolve_loader's "subos"
               // default (the system probe fails in this hermetic test env);
               // we just need both --set-rpath and --set-interpreter to fire.
               "    elfpatch.auto({ enable = true, interpreter = \"/lib64/ld-linux-x86-64.so.2\" })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "linux", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;
    auto patch_result = exec->apply_elfpatch_auto();
    ASSERT_TRUE(patch_result.success) << patch_result.error;

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();

    auto rpath_pos  = log.find("--set-rpath");
    auto interp_pos = log.find("--set-interpreter");
    ASSERT_NE(rpath_pos,  std::string::npos) << "expected --set-rpath in log; got:\n" << log;
    ASSERT_NE(interp_pos, std::string::npos) << "expected --set-interpreter in log; got:\n" << log;
    EXPECT_LT(rpath_pos, interp_pos)
        << "--set-rpath must be invoked BEFORE --set-interpreter to avoid "
           "patchelf 0.18.0's compact-ELF corruption bug. Log:\n" << log;

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, ApplyElfpatchAuto_MacOsUsesInstallNameToolForMachO) {
#ifdef _WIN32
    GTEST_SKIP() << "macOS tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-macos-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path log_path = temp_dir / "tool.log";
    const fs::path pkg_path = temp_dir / "elfpatch-macos.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(tools_dir);
    fs::create_directories(lib_dir);

    write_executable_script(tools_dir / "install_name_tool",
                            "#!/bin/sh\n"
                            "printf 'install_name_tool %s\\n' \"$*\" >> \"$ELFPATCH_LOG\"\n");
    write_executable_script(tools_dir / "otool",
                            "#!/bin/sh\n"
                            "if [ \"$1\" = \"-L\" ]; then\n"
                            "  printf '%s:\\n' \"$2\"\n"
                            "  printf '\\t/opt/demo/lib/libdemo.dylib (compatibility version 1.0.0, current version 1.0.0)\\n'\n"
                            "fi\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-macos\", xpm = { macosx = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);
    ScopedEnvVar log_env("ELFPATCH_LOG", log_path.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "macosx", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "1 1 0");

    std::ifstream log_file(log_path);
    std::ostringstream log_buffer;
    log_buffer << log_file.rdbuf();
    const std::string log = log_buffer.str();
    EXPECT_NE(log.find("-add_rpath " + lib_dir.string()), std::string::npos);
    EXPECT_NE(log.find("-change /opt/demo/lib/libdemo.dylib @rpath/libdemo.dylib " + binary_path.string()),
              std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, ApplyElfpatchAuto_MacOsAddRpathFailureCountsAsFailed) {
#ifdef _WIN32
    GTEST_SKIP() << "macOS tool emulation test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-macos-rpath-fail-");
    const fs::path tools_dir = temp_dir / "tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path pkg_path = temp_dir / "elfpatch-macos.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(tools_dir);
    fs::create_directories(lib_dir);

    write_executable_script(tools_dir / "install_name_tool",
                            "#!/bin/sh\n"
                            "exit 1\n");
    write_executable_script(tools_dir / "otool",
                            "#!/bin/sh\n"
                            "if [ \"$1\" = \"-L\" ]; then\n"
                            "  printf '%s:\\n' \"$2\"\n"
                            "fi\n");

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-macos\", xpm = { macosx = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvVar path_env("PATH", tools_dir.string() + ":" + original_path);

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "macosx", tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "1 0 1");

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, ApplyElfpatchAuto_MacOsMissingToolSkipsGracefully) {
#ifdef _WIN32
    GTEST_SKIP() << "macOS tool lookup test is POSIX-specific";
#endif

    const fs::path temp_dir = make_temp_dir("libxpkg-elfpatch-macos-missing-tool-");
    const fs::path empty_tools_dir = temp_dir / "empty-tools";
    const fs::path install_dir = temp_dir / "install";
    const fs::path lib_dir = install_dir / "lib";
    const fs::path pkg_path = temp_dir / "elfpatch-macos.lua";
    const fs::path binary_path = install_dir / "demo-bin";

    fs::create_directories(empty_tools_dir);
    fs::create_directories(lib_dir);

    {
        std::ofstream binary(binary_path, std::ios::binary);
        ASSERT_TRUE(binary.good());
        const unsigned char magic[] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0};
        binary.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    }
    fs::permissions(binary_path,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    write_text(pkg_path,
               "package = { spec = \"1\", name = \"elfpatch-macos\", xpm = { macosx = { [\"latest\"] = { ref = \"1.0.0\" }, [\"1.0.0\"] = { url = \"https://example.com/demo.tar.gz\", sha256 = \"0\" } } } }\n"
               "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n"
               "function install()\n"
               "    elfpatch.auto({ enable = true })\n"
               "    return true\n"
               "end\n");

    ScopedEnvVar path_env("PATH", empty_tools_dir.string());

    auto exec = create_executor(pkg_path);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());

    auto hook_result = exec->run_hook(HookType::Install, make_context(install_dir, "macosx", empty_tools_dir));
    ASSERT_TRUE(hook_result.success) << hook_result.error;

    auto patch_result = exec->apply_elfpatch_auto();
    EXPECT_TRUE(patch_result.success) << patch_result.error;
    EXPECT_EQ(patch_result.output, "0 0 0");

    fs::remove_all(temp_dir);
}

TEST(ExecutorTest, OsFuncs_Cp_PreservesSymlinks) {
#ifdef _WIN32
    GTEST_SKIP() << "Symlink preservation test is POSIX-specific";
#endif

    const fs::path temp = make_temp_dir("libxpkg-oscp-symlink-");
    const fs::path src = temp / "src_dir";
    const fs::path dst = temp / "dst_dir";
    fs::create_directories(src);
    write_text(src / "real.txt", "hello");
    fs::create_symlink("real.txt", src / "link.txt");

    auto pkg = temp / "oscp_sym.lua";
    write_text(pkg, std::string(
        "package = { name = \"oscp_sym\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main(s, d) return os.cp(s, d) end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {src.string(), dst.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::is_symlink(dst / "link.txt"))
        << "link.txt should remain a symlink after os.cp";
    EXPECT_EQ(fs::read_symlink(dst / "link.txt").string(), "real.txt");
    fs::remove_all(temp);
}

// ─── auto-stamp behavior on HookType::Install ────────────────────────────
//
// Background: wrapper packages (linux-headers, etc.) leave install_dir
// empty because their real payload lives elsewhere. Without anything in
// install_dir, xlings's catalog probe (`is_directory && !is_empty`) flags
// them as not-installed on every dependent install, looping forever.
// run_hook(Install) auto-stamps `.xim-installed` when install_dir ends up
// empty so the catalog probe can see "yes, installed".

namespace {
std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST(ExecutorTest, ApplyInstallStamp_WritesStampWhenInstallDirEmpty) {
    // Wrapper packages (linux-headers, fromsource:* aliases) leave
    // install_dir empty after install hook; stamp marks them as installed.
    const fs::path temp = make_temp_dir("libxpkg-stamp-empty-");
    const fs::path install_dir = temp / "install";
    fs::create_directories(install_dir);

    auto pkg = temp / "wrapper.lua";
    write_text(pkg, std::string(
        "package = { spec = \"1\", name = \"wrapper\", "
        "xpm = { linux = { [\"1.0.0\"] = { url = \"x\", sha256 = \"0\" } } } }\n"
        "function install() return true end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();

    ExecutionContext ctx = make_context(install_dir, "linux");
    ctx.pkg_name = "wrapper";
    ctx.version  = "1.0.0";

    auto r = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(r.success) << r.error;
    // Consumer (e.g. xlings installer) calls stamp explicitly after all
    // install paths (hook + extracted-payload fallback + script default).
    exec->apply_install_stamp_if_empty(ctx);

    auto stamp = install_dir / ".xim-installed";
    EXPECT_TRUE(fs::exists(stamp)) << "auto-stamp must write .xim-installed when install_dir empty";

    auto content = read_file(stamp);
    EXPECT_NE(content.find("schema = 1"),       std::string::npos);
    EXPECT_NE(content.find("name = wrapper"),   std::string::npos);
    EXPECT_NE(content.find("version = 1.0.0"),  std::string::npos);
    EXPECT_NE(content.find("platform = linux"), std::string::npos);

    fs::remove_all(temp);
}

TEST(ExecutorTest, ApplyInstallStamp_SkipsWhenInstallDirNonEmpty) {
    // If anything has populated install_dir (install hook content,
    // staged extracted payload, default script install), don't add stamp.
    const fs::path temp = make_temp_dir("libxpkg-stamp-nonempty-");
    const fs::path install_dir = temp / "install";
    fs::create_directories(install_dir);

    auto pkg = temp / "regular.lua";
    write_text(pkg, std::string(
        "package = { spec = \"1\", name = \"regular\", "
        "xpm = { linux = { [\"1.0.0\"] = { url = \"x\", sha256 = \"0\" } } } }\n"
        "function install()\n"
        "  io.open(_RUNTIME.install_dir .. '/payload.txt', 'w'):write('content'):close()\n"
        "  return true\n"
        "end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();

    ExecutionContext ctx = make_context(install_dir, "linux");
    ctx.pkg_name = "regular";
    ctx.version  = "1.0.0";

    auto r = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(r.success) << r.error;
    exec->apply_install_stamp_if_empty(ctx);

    EXPECT_TRUE(fs::exists(install_dir / "payload.txt"));
    EXPECT_FALSE(fs::exists(install_dir / ".xim-installed"))
        << "auto-stamp must not write when install_dir already has content";

    fs::remove_all(temp);
}

TEST(ExecutorTest, RunHook_DoesNotImplicitlyStamp) {
    // Regression: auto-stamp used to live inside run_hook, which wrote
    // .xim-installed before xlings's stage_extracted_payload_ fallback
    // could check "is install_dir empty?". This poisoned the fallback
    // for packages whose install hook silently no-ops (e.g. patchelf,
    // whose tarball has no top-level dir, so `os.mv(extracted_dir,
    // install_dir)` is a no-op). Stamp must now be explicit.
    const fs::path temp = make_temp_dir("libxpkg-stamp-noimplicit-");
    const fs::path install_dir = temp / "install";
    fs::create_directories(install_dir);

    auto pkg = temp / "silent.lua";
    write_text(pkg, std::string(
        "package = { spec = \"1\", name = \"silent\", "
        "xpm = { linux = { [\"1.0.0\"] = { url = \"x\", sha256 = \"0\" } } } }\n"
        "function install() return true end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();

    ExecutionContext ctx = make_context(install_dir, "linux");
    ctx.pkg_name = "silent";
    ctx.version  = "1.0.0";

    auto r = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_FALSE(fs::exists(install_dir / ".xim-installed"))
        << "run_hook must NOT write the stamp; consumers call apply_install_stamp_if_empty explicitly";

    fs::remove_all(temp);
}

// ---- fs module tests ----

TEST(ExecutorTest, FsModule_SymlinkAndReadlink) {
#ifdef _WIN32
    GTEST_SKIP() << "Symlink tests are POSIX-specific";
#endif
    const fs::path temp = make_temp_dir("libxpkg-fs-symlink-");
    write_text(temp / "target.txt", "hello");

    auto pkg = temp / "fs_symlink.lua";
    write_text(pkg, std::string(
        "package = { name = \"fs_symlink\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "import('xim.libxpkg.fs')\n"
        "function xpkg_main(dir)\n"
        "    local src = dir .. '/target.txt'\n"
        "    local dst = dir .. '/link.txt'\n"
        "    if not fs.symlink(src, dst) then error('symlink failed') end\n"
        "    if not fs.is_symlink(dst) then error('is_symlink failed') end\n"
        "    local target = fs.readlink(dst)\n"
        "    if target ~= src then error('readlink mismatch: ' .. tostring(target)) end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {temp.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::is_symlink(temp / "link.txt"));
    fs::remove_all(temp);
}

TEST(ExecutorTest, FsModule_Entries) {
    const fs::path temp = make_temp_dir("libxpkg-fs-entries-");
    write_text(temp / "a.txt", "a");
    write_text(temp / "b.txt", "b");
    fs::create_directories(temp / "subdir");

    auto pkg = temp / "fs_entries.lua";
    write_text(pkg, std::string(
        "package = { name = \"fs_entries\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "import('xim.libxpkg.fs')\n"
        "function xpkg_main(dir)\n"
        "    local entries = fs.entries(dir)\n"
        "    if #entries < 3 then error('expected >= 3 entries, got ' .. #entries) end\n"
        "    local has_file, has_dir = false, false\n"
        "    for _, e in ipairs(entries) do\n"
        "        if e.name == 'a.txt' and e.type == 'file' then has_file = true end\n"
        "        if e.name == 'subdir' and e.type == 'directory' then has_dir = true end\n"
        "    end\n"
        "    if not has_file then error('missing file entry') end\n"
        "    if not has_dir then error('missing dir entry') end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {temp.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, FsModule_FilesRecursive) {
    const fs::path temp = make_temp_dir("libxpkg-fs-files-");
    // Use a subdirectory to avoid counting the test pkg .lua file
    const fs::path data = temp / "data";
    fs::create_directories(data / "a" / "b");
    write_text(data / "top.txt", "t");
    write_text(data / "a" / "mid.txt", "m");
    write_text(data / "a" / "b" / "deep.txt", "d");

    auto pkg = temp / "fs_files.lua";
    write_text(pkg, std::string(
        "package = { name = \"fs_files\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "import('xim.libxpkg.fs')\n"
        "function xpkg_main(dir)\n"
        "    local flat = fs.files(dir)\n"
        "    local deep = fs.files(dir, true)\n"
        "    if #flat ~= 1 then error('flat expected 1, got ' .. #flat) end\n"
        "    if #deep ~= 3 then error('deep expected 3, got ' .. #deep) end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {data.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, FsModule_CopyFile) {
    const fs::path temp = make_temp_dir("libxpkg-fs-copyfile-");
    write_text(temp / "src.txt", "content");

    auto pkg = temp / "fs_cp.lua";
    write_text(pkg, std::string(
        "package = { name = \"fs_cp\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "import('xim.libxpkg.fs')\n"
        "function xpkg_main(dir)\n"
        "    if not fs.copy_file(dir..'/src.txt', dir..'/dst.txt') then error('copy_file failed') end\n"
        "    if not fs.is_file(dir..'/dst.txt') then error('dst missing') end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {temp.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_TRUE(fs::exists(temp / "dst.txt"));
    fs::remove_all(temp);
}

TEST(ExecutorTest, FsModule_MkdirP_Remove) {
    const fs::path temp = make_temp_dir("libxpkg-fs-mkdir-");

    auto pkg = temp / "fs_mkdir.lua";
    write_text(pkg, std::string(
        "package = { name = \"fs_mkdir\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "import('xim.libxpkg.fs')\n"
        "function xpkg_main(dir)\n"
        "    local nested = dir .. '/a/b/c'\n"
        "    if not fs.mkdir_p(nested) then error('mkdir_p failed') end\n"
        "    if not fs.is_directory(nested) then error('not a dir') end\n"
        "    if not fs.exists(nested) then error('not exists') end\n"
        "    fs.remove_all(dir .. '/a')\n"
        "    if fs.exists(dir .. '/a') then error('remove_all failed') end\n"
        "end\n"));
    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.args = {temp.string()};
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

// ---- pkgindex custom module loading tests ----

TEST(ExecutorTest, PkgindexCustomModule_LoadsFromLibsDir) {
    const fs::path temp = make_temp_dir("libxpkg-pkgindex-mod-");
    // Create pkgindex structure: <root>/libs/mymod.lua, <root>/pkgs/t/test.lua
    fs::create_directories(temp / "pkgindex" / "libs");
    fs::create_directories(temp / "pkgindex" / "pkgs" / "t");

    // Write custom module
    write_text(temp / "pkgindex" / "libs" / "mymod.lua",
        "local M = {}\n"
        "function M.greet() return 'hello from mymod' end\n"
        "return M\n");

    // Write package that uses it (import inside xpkg_main so _RUNTIME is set)
    auto pkg = temp / "pkgindex" / "pkgs" / "t" / "test.lua";
    write_text(pkg, std::string(
        "package = { name = \"test\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main()\n"
        "    import('xim.pkgindex.mymod')\n"
        "    local msg = mymod.greet()\n"
        "    if msg ~= 'hello from mymod' then error('wrong: ' .. tostring(msg)) end\n"
        "end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.pkgindex_dir = (temp / "pkgindex").string();
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, PkgindexCustomModule_CachesAcrossCalls) {
    const fs::path temp = make_temp_dir("libxpkg-pkgindex-cache-");
    fs::create_directories(temp / "pkgindex" / "libs");
    fs::create_directories(temp / "pkgindex" / "pkgs" / "t");

    // Module with a counter to verify it's only loaded once
    write_text(temp / "pkgindex" / "libs" / "counter.lua",
        "local M = { count = 0 }\n"
        "M.count = M.count + 1\n"
        "return M\n");

    auto pkg = temp / "pkgindex" / "pkgs" / "t" / "test.lua";
    write_text(pkg, std::string(
        "package = { name = \"test\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main()\n"
        "    import('xim.pkgindex.counter')\n"
        "    local c1 = counter.count\n"
        "    import('xim.pkgindex.counter')  -- second import should hit cache\n"
        "    local c2 = counter.count\n"
        "    if c1 ~= 1 then error('first load count should be 1, got ' .. tostring(c1)) end\n"
        "    if c1 ~= c2 then error('module reloaded: ' .. tostring(c1) .. ' vs ' .. tostring(c2)) end\n"
        "end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.pkgindex_dir = (temp / "pkgindex").string();
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, PkgindexCustomModule_UnknownReturnsStub) {
    const fs::path temp = make_temp_dir("libxpkg-pkgindex-unknown-");
    fs::create_directories(temp / "pkgindex" / "libs");
    fs::create_directories(temp / "pkgindex" / "pkgs" / "t");

    auto pkg = temp / "pkgindex" / "pkgs" / "t" / "test.lua";
    write_text(pkg, std::string(
        "package = { name = \"test\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main()\n"
        "    import('xim.pkgindex.nonexistent')\n"
        "    -- Should get a stub proxy, not crash\n"
        "    local x = tostring(nonexistent.something)\n"
        "end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.pkgindex_dir = (temp / "pkgindex").string();
    auto r = exec->run_script(ctx);
    EXPECT_TRUE(r.success) << r.error;
    fs::remove_all(temp);
}

TEST(ExecutorTest, ApplyInstallStamp_IsIdempotent) {
    // Calling apply_install_stamp_if_empty twice is safe — the second
    // call sees a non-empty dir (the first call's stamp) and no-ops.
    const fs::path temp = make_temp_dir("libxpkg-stamp-idempotent-");
    const fs::path install_dir = temp / "install";
    fs::create_directories(install_dir);

    auto pkg = temp / "any.lua";
    write_text(pkg, std::string(
        "package = { spec = \"1\", name = \"any\", "
        "xpm = { linux = { [\"1.0.0\"] = { url = \"x\", sha256 = \"0\" } } } }\n"
        "function install() return true end\n"));

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();

    ExecutionContext ctx = make_context(install_dir, "linux");
    ctx.pkg_name = "any";
    ctx.version  = "1.0.0";

    exec->apply_install_stamp_if_empty(ctx);
    auto stamp = install_dir / ".xim-installed";
    ASSERT_TRUE(fs::exists(stamp));
    auto first_content = read_file(stamp);

    exec->apply_install_stamp_if_empty(ctx);
    auto second_content = read_file(stamp);
    EXPECT_EQ(first_content, second_content)
        << "second call must not rewrite stamp";

    fs::remove_all(temp);
}

// ============================================================
// files assets and injected args
//
// `includedir` could only say "this one directory becomes sysroot include".
// It could not express a destination, an asset that is not a header, or a
// source and destination that differ in name -- openssl's `lib64/` ->
// `usr/lib/` is all three at once. With no way to say it, package indexes
// grow their own file-placing helpers and the tool managing versions can
// neither see nor undo them.
//
// `args` is separate from `alias` because the only way to inject anything
// used to be appending it to the alias string, which consumers then split on
// the first space -- broken by any path containing one, and it makes every
// reader of `alias` report a command line where a name belongs.
// ============================================================

namespace {

// Write a recipe whose config() hook is `body`, and return its ops.
std::vector<XvmOp> ops_from_config(const fs::path& dir, const char* body,
                                   const char* extra_imports = "") {
    fs::create_directories(dir);
    auto pkg = dir / "opsfixture.lua";
    std::string lua =
        "package = { spec = \"1\", name = \"opsfixture\", type = \"package\",\n"
        "    xpm = { linux = { [\"1.0.0\"] = {} },\n"
        "            macosx = { [\"1.0.0\"] = {} },\n"
        "            windows = { [\"1.0.0\"] = {} } } }\n"
        "import(\"xim.libxpkg.xvm\")\n";
    lua += extra_imports;
    lua += "function config()\n";
    lua += body;
    lua += "\n    return true\nend\n";
    std::ofstream(pkg) << lua;

    auto exec = create_executor(pkg.string());
    EXPECT_TRUE(exec.has_value());
    if (!exec) return {};
    auto ctx = make_context(dir, "linux");
    ctx.pkg_name = "opsfixture";
    auto hook = exec->run_hook(HookType::Config, ctx);
    EXPECT_TRUE(hook.success) << hook.error;
    return exec->xvm_operations();
}

} // namespace

TEST(ExecutorTest, XvmAdd_CarriesSrcAndDstForFilesAssets) {
    auto dir = fs::temp_directory_path() / "libxpkg_files_assets";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    xvm.add(\"pkg.files.1\", { type = \"files\",\n"
        "        src = \"include/openssl\", dst = \"usr/include/openssl\" })");

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].type, "files");
    EXPECT_EQ(ops[0].src, "include/openssl");
    EXPECT_EQ(ops[0].dst, "usr/include/openssl");
    fs::remove_all(dir);
}

TEST(ExecutorTest, XvmAdd_CarriesInjectedArgsInOrder) {
    auto dir = fs::temp_directory_path() / "libxpkg_args";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    xvm.add(\"clang\", { args = { \"-isystem\", \"/a b/include\",\n"
        "                                  \"--sysroot=/root\" } })");

    ASSERT_EQ(ops.size(), 1u);
    ASSERT_EQ(ops[0].args.size(), 3u);
    EXPECT_EQ(ops[0].args[0], "-isystem");
    // A path containing a space survives, which it cannot when arguments are
    // smuggled through `alias` and split on the first one.
    EXPECT_EQ(ops[0].args[1], "/a b/include");
    EXPECT_EQ(ops[0].args[2], "--sysroot=/root");
    EXPECT_TRUE(ops[0].alias.empty()) << "args must not leak into alias";
    fs::remove_all(dir);
}

TEST(ExecutorTest, XvmFiles_DerivesADistinctTargetPerDeclaration) {
    auto dir = fs::temp_directory_path() / "libxpkg_files_sugar";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    xvm.files({ src = \"include\", dst = \"usr/include\" })\n"
        "    xvm.files({ src = \"lib64\",   dst = \"usr/lib\" })");

    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].type, "files");
    EXPECT_EQ(ops[1].type, "files");
    EXPECT_EQ(ops[0].src, "include");
    EXPECT_EQ(ops[1].src, "lib64");
    // Names are derived, not caller-supplied, so two declarations from one
    // package cannot collide.
    EXPECT_NE(ops[0].name, ops[1].name);
    fs::remove_all(dir);
}

TEST(ExecutorTest, XvmAdd_OmittingTheNewFieldsLeavesThemEmpty) {
    auto dir = fs::temp_directory_path() / "libxpkg_no_new_fields";
    fs::remove_all(dir);
    // An existing recipe must be completely unaffected.
    auto ops = ops_from_config(dir,
        "    xvm.add(\"tool\", { bindir = \"bin\", binding = \"root@1.0.0\" })");

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_TRUE(ops[0].src.empty());
    EXPECT_TRUE(ops[0].dst.empty());
    EXPECT_TRUE(ops[0].args.empty());
    EXPECT_EQ(ops[0].binding, "root@1.0.0");
    fs::remove_all(dir);
}

// ── subos.env: subos-scoped environment declarations ─────────────────────
//
// A separate op kind rather than more fields on `add`, because the scope is
// different: `envs` on an add is what one program shim exports for itself,
// and the program that has to see LIBGL_DRIVERS_PATH is the user's own
// binary, which xlings never wraps.

namespace {

constexpr const char* SUBOS_IMPORT = "import(\"xim.libxpkg.subos\")\n";

} // namespace

TEST(ExecutorTest, SubosEnv_RecordsASetDeclaration) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_set";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    subos.env({ var = \"LIBGL_DRIVERS_PATH\", op = \"set\",\n"
        "                value = \"${pkgdir}/lib/dri\",\n"
        "                binding = \"compat.mesa@25.0.0\" })",
        SUBOS_IMPORT);

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].op, "subos_env");
    EXPECT_EQ(ops[0].var, "LIBGL_DRIVERS_PATH");
    // The recipe's `op` argument travels as `mode`; `op` is the category the
    // consumer dispatches on and was already taken.
    EXPECT_EQ(ops[0].mode, "set");
    EXPECT_EQ(ops[0].value, "${pkgdir}/lib/dri");
    EXPECT_EQ(ops[0].binding, "compat.mesa@25.0.0");
    fs::remove_all(dir);
}

TEST(ExecutorTest, SubosEnv_RecordsAPrependDeclaration) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_prepend";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    subos.env({ var = \"XDG_DATA_DIRS\", op = \"prepend\",\n"
        "                value = \"${pkgdir}/share\",\n"
        "                binding = \"compat.mesa@25.0.0\" })",
        SUBOS_IMPORT);

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].mode, "prepend");
    EXPECT_EQ(ops[0].var, "XDG_DATA_DIRS");
    fs::remove_all(dir);
}

TEST(ExecutorTest, SubosEnv_DefaultsOpToSetAndBindingToThePackage) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_defaults";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    subos.env({ var = \"MESA_LOADER_DRIVER_OVERRIDE\",\n"
        "                value = \"swrast\" })",
        SUBOS_IMPORT);

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].mode, "set");
    EXPECT_EQ(ops[0].binding, "opsfixture@1.0.0");
    fs::remove_all(dir);
}

TEST(ExecutorTest, SubosEnv_RejectsAnOpThisClientDoesNotImplement) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_badop";
    fs::remove_all(dir);
    // `append` is a documented future op. Recording it as if it were `set`
    // would put a value in the manifest that no one asked for; dropping it
    // silently would be the same bug one layer down. It is refused, and the
    // recipe sees `false`.
    auto ops = ops_from_config(dir,
        "    local ok = subos.env({ var = \"PATH\", op = \"append\",\n"
        "                           value = \"${pkgdir}/bin\" })\n"
        "    xvm.add(\"probe.returned.\" .. tostring(ok))",
        SUBOS_IMPORT);

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].name, "probe.returned.false");
    fs::remove_all(dir);
}

TEST(ExecutorTest, SubosEnv_RejectsAMissingVariableName) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_novar";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    local ok = subos.env({ value = \"anything\" })\n"
        "    xvm.add(\"probe.returned.\" .. tostring(ok))",
        SUBOS_IMPORT);

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].name, "probe.returned.false");
    fs::remove_all(dir);
}

// The capability probe a recipe has to write, and the reason it cannot be
// spelled the obvious way.
//
// import() answers an unknown module with a permissive proxy: every key read
// off it is a truthy callable table. So on a client that predates this module,
// `if subos.env then` is *true*, the recipe takes the new branch, and the call
// evaporates -- install succeeds, nothing is configured, nothing complains.
//
// `if xvm.files then` in the V2 spec is safe only because `xvm` is a module
// those clients already ship, so the missing *field* really is nil. A missing
// *module* never is. type() is what separates them.
TEST(ExecutorTest, SubosEnv_ProbeMustTestTypeBecauseUnknownModulesAreTruthy) {
    auto dir = fs::temp_directory_path() / "libxpkg_subos_env_probe";
    fs::remove_all(dir);
    auto ops = ops_from_config(dir,
        "    xvm.add(\"real.truthy.\"  .. tostring(subos.env ~= nil))\n"
        "    xvm.add(\"real.typed.\"   .. tostring(type(subos.env) == \"function\"))\n"
        "    xvm.add(\"absent.truthy.\" .. tostring(notyet.env ~= nil))\n"
        "    xvm.add(\"absent.typed.\"  .. tostring(type(notyet.env) == \"function\"))",
        "import(\"xim.libxpkg.subos\")\n"
        "import(\"xim.libxpkg.notyet\")\n");

    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0].name, "real.truthy.true");
    EXPECT_EQ(ops[1].name, "real.typed.true");
    // Both of these are the point: the truthiness test cannot tell a stub from
    // a real module, and the type test can.
    EXPECT_EQ(ops[2].name, "absent.truthy.true")
        << "if import() ever stopped stubbing unknown modules, the probe rule "
           "in the V2 spec could be relaxed -- until then it must stay";
    EXPECT_EQ(ops[3].name, "absent.typed.false");
    fs::remove_all(dir);
}

// The shape gcc.lua and llvm.lua actually use, and the shape they should use.
//
// Both declare `xim:glibc@>=2.39` and then ask `dep_install_dir("glibc")` --
// bare and unversioned. 0.0.55 made explicit dependency stores the resolver
// for this path, and they answer only an exact NAMESPACED coordinate, so the
// call started returning nil. Silently: a recipe cannot tell that from "the
// dependency is not installed". Measured under xlings 2026.8.10.1 -- gcc's
// config hook reported "glibc payload not found" on a home where glibc was
// installed, and gcc could not install on Linux.
//
// nil is the right answer for a bare name (guessing between `compat-x-zlib`
// and `other-x-zlib` is the decoy problem these roots remove). Saying nothing
// was not. And the query IS answerable the moment the caller names the
// dependency the way it declared it.
TEST(ExecutorTest, PkgInfo_NamespacedUnversionedQueryUsesResolvedRecord) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-ns-noversion-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path glibcPayload = registryRoot / "xim-x-glibc" / "2.44";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(glibcPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"xim:glibc\")\n"
        "    assert(got ~= nil, \"namespaced unversioned query resolved to nil\")\n"
        "    assert(got:find(\"xim-x-glibc\", 1, true) ~= nil, got)\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};
    ctx.resolved_deps["xim:glibc@>=2.39"] = ResolvedDep {
        .spec = "xim:glibc@>=2.39",
        .name = "xim:glibc",
        .version = "2.44",
        .install_dir = glibcPayload.string(),
        .libdirs = {},
        .source = "plan-range",
    };

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
}

// The bare form still returns nil -- and now says why, and what to write
// instead. "Cannot answer" and "not installed" must not look the same.
TEST(ExecutorTest, PkgInfo_BareNameUnderExplicitRootsExplainsItself) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-explains-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(registryRoot / "xim-x-glibc" / "2.44");
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"glibc\")\n"
        "    assert(got == nil, \"bare name guessed: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(result.output.find("bare name cannot be resolved"),
              std::string::npos)
        << "a bare-name miss must explain itself:\n" << result.output;
    EXPECT_NE(result.output.find("ns:glibc"), std::string::npos)
        << "the diagnostic must show the call that would work:\n"
        << result.output;
}

// ...and the guarantee 0.0.55 added is untouched: an EXACT,
// NAMESPACED coordinate that the supplied roots do not contain is a definite
// no. If this fell through to the scan, a same-bare-name decoy elsewhere would
// answer it -- which is the whole thing explicit roots exist to prevent.
TEST(ExecutorTest, PkgInfo_ExactNamespacedCoordinateStillFailsClosed) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-exact-closed-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path memberXpkgs = tempDir / "member" / "data" / "xpkgs";
    const fs::path decoy = memberXpkgs / "other-x-zlib" / "1.3.2";
    const fs::path memberPayload = memberXpkgs / "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(registryRoot);
    fs::create_directories(decoy);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"compat:zlib\", \"1.3.2\")\n"
        "    assert(got == nil, \"exact coordinate fell through to: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
}

// ── the #524 regression itself ───────────────────────────────────────────
//
// Everything above tests the case where `resolved_deps` genuinely cannot
// answer. This is the case where it CAN, and 0.0.55 refused anyway.
//
// gcc.lua declares `xim:glibc@>=2.39` and asks `dep_install_dir("glibc")`.
// The record is right there, under a bare name that nothing else in the table
// claims. 0.0.55 rejected it because the QUESTION was underspecified, not
// because the ANSWER was ambiguous -- and the uniqueness guard that decides
// ambiguity was already present and already failing closed. Measured: 6 of
// the 7 real call sites in xim-pkgindex returned nil; gcc and meson could not
// install on any cold home.
TEST(ExecutorTest, PkgInfo_BareUnversionedResolvesWhenTheRecordIsUnique) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-unique-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path glibcPayload = registryRoot / "xim-x-glibc" / "2.44";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(glibcPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"glibc\")\n"
        "    assert(got ~= nil, \"the unique record did not answer\")\n"
        "    assert(got:find(\"xim-x-glibc\", 1, true) ~= nil, got)\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};
    ctx.resolved_deps["xim:glibc@>=2.39"] = ResolvedDep {
        .spec = "xim:glibc@>=2.39",
        .name = "xim:glibc",
        .version = "2.44",
        .install_dir = glibcPayload.string(),
        .libdirs = {},
        .source = "plan-range",
    };

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
}

// A caller may restate the version, and what it restates is the RANGE the
// recipe declared -- not the concrete version the resolver picked. Comparing
// those as strings makes them unequal, which is openxlings/xlings#481 again:
// there `xim:glibc@>=2.38` matched no plan node, so nothing got an RPATH and
// the package installed reporting success.
TEST(ExecutorTest, PkgInfo_BareWithRangeMatchesTheResolversConcretePick) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-bare-range-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path glibcPayload = registryRoot / "xim-x-glibc" / "2.44";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(glibcPayload);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local hit = pkginfo.dep_install_dir(\"glibc\", \">=2.39\")\n"
        "    assert(hit ~= nil, \"a range did not match the resolved 2.44\")\n"
        "    -- ...and a version the record does NOT satisfy is still a miss.\n"
        "    local miss = pkginfo.dep_install_dir(\"glibc\", \"2.39\")\n"
        "    assert(miss == nil, \"wrong exact version matched: \" .. tostring(miss))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};
    ctx.resolved_deps["xim:glibc@>=2.39"] = ResolvedDep {
        .spec = "xim:glibc@>=2.39",
        .name = "xim:glibc",
        .version = "2.44",
        .install_dir = glibcPayload.string(),
        .libdirs = {},
        .source = "plan-range",
    };

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
}

// Ambiguity is what fails closed -- and it must name both providers. "not
// found" would send the reader looking for a missing payload when the payload
// is there twice.
//
// It must ALSO not repeat the bare-name advice underneath. That message says
// "you did not name a namespace"; here the caller's real problem is that two
// namespaces answer, and stacking the generic advice on top points at the
// wrong fix.
TEST(ExecutorTest, PkgInfo_TwoProvidersOfOneBareNameFailClosedAndNameBoth) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-ambiguous-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path compatZlib = registryRoot / "compat-x-zlib" / "1.3";
    const fs::path otherZlib = registryRoot / "other-x-zlib" / "1.3";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(compatZlib);
    fs::create_directories(otherZlib);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"zlib\")\n"
        "    assert(got == nil, \"ambiguous name guessed: \" .. tostring(got))\n"
        "    -- naming the namespace resolves it, which is the advice we print\n"
        "    local ok = pkginfo.dep_install_dir(\"compat:zlib\")\n"
        "    assert(ok ~= nil and ok:find(\"compat-x-zlib\", 1, true) ~= nil,\n"
        "           \"namespaced form did not resolve: \" .. tostring(ok))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};
    ctx.resolved_deps["compat:zlib@1.3"] = ResolvedDep {
        .spec = "compat:zlib@1.3",
        .name = "compat:zlib",
        .version = "1.3",
        .install_dir = compatZlib.string(),
        .libdirs = {},
        .source = "plan-exact",
    };
    ctx.resolved_deps["other:zlib@1.3"] = ResolvedDep {
        .spec = "other:zlib@1.3",
        .name = "other:zlib",
        .version = "1.3",
        .install_dir = otherZlib.string(),
        .libdirs = {},
        .source = "plan-exact",
    };

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(result.output.find("ambiguous"), std::string::npos)
        << "a collision must say so:\n" << result.output;
    EXPECT_NE(result.output.find("compat:zlib"), std::string::npos)
        << "the diagnostic must name the providers:\n" << result.output;
    EXPECT_NE(result.output.find("other:zlib"), std::string::npos)
        << "the diagnostic must name the providers:\n" << result.output;
    EXPECT_EQ(result.output.find("bare name cannot be resolved"),
              std::string::npos)
        << "the generic bare-name advice points at the wrong fix here:\n"
        << result.output;
}

// Widening the match must NOT weaken the payload check 0.0.55 added: a record
// whose install_dir is not on disk is a definite no, with its own message.
// Returning the path anyway is how a hook proceeds against a directory that
// does not exist and blames something further downstream.
TEST(ExecutorTest, PkgInfo_UniqueRecordWithNoPayloadOnDiskIsStillAMiss) {
    const fs::path tempDir = make_temp_dir("libxpkg-pkginfo-no-payload-");
    const fs::path registryRoot = tempDir / "registry" / "data" / "xpkgs";
    const fs::path memberPayload = tempDir / "member" / "data" / "xpkgs" /
                                   "consumer" / "1.0.0";
    const fs::path pkgPath = tempDir / "consumer.lua";
    fs::create_directories(registryRoot);
    fs::create_directories(memberPayload);

    write_text(pkgPath,
        "package = { spec = \"1\", name = \"consumer\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "function install()\n"
        "    local got = pkginfo.dep_install_dir(\"glibc\")\n"
        "    assert(got == nil, \"answered with an absent payload: \" .. tostring(got))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkgPath);
    ASSERT_TRUE(exec.has_value()) << (exec ? "" : exec.error());
    auto ctx = make_context(memberPayload, "linux");
    ctx.dependency_store_roots = {registryRoot};
    ctx.resolved_deps["xim:glibc@>=2.39"] = ResolvedDep {
        .spec = "xim:glibc@>=2.39",
        .name = "xim:glibc",
        .version = "2.44",
        .install_dir = (registryRoot / "xim-x-glibc" / "2.44").string(),
        .libdirs = {},
        .source = "plan-range",
    };

    auto result = exec->run_hook(HookType::Install, ctx);
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(result.output.find("missing payload"), std::string::npos)
        << "an absent payload must say so:\n" << result.output;
}

// ---- ExecutionContext::hook_log: child-process output of a hook (0.0.60) ----
//
// Children started by os.execute inherit this process's fd 1 and 2, which is
// exactly what testing::internal::CaptureStdout/CaptureStderr swap out -- so a
// marker that shows up in the captured text reached "the terminal", and one that
// does not was kept off it.

namespace {

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(begin, end - begin));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        begin = end + 1;
    }
    return lines;
}

fs::path write_hook_package(const fs::path& dir, std::string_view name,
                            std::string_view body) {
    const fs::path pkg = dir / (std::string(name) + ".lua");
    write_text(pkg,
        "package = { name = \"" + std::string(name) +
        "\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "local system = import(\"xim.libxpkg.system\")\n" + std::string(body));
    return pkg;
}

struct Escaped {
    std::string out, err;
};

template <typename Fn>
Escaped run_captured(Fn&& fn) {
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    fn();
    Escaped escaped;
    escaped.out = testing::internal::GetCapturedStdout();
    escaped.err = testing::internal::GetCapturedStderr();
    return escaped;
}

} // namespace

TEST(ExecutorTest, HookLog_ChildOutputAndPrintLandInOrderNotOnTheTerminal) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-order-");
    const fs::path pkg = write_hook_package(temp, "hook-log-order",
        "function install()\n"
        "    print(\"MARK-1-print\")\n"
        "    os.execute(\"echo MARK-2-child-out\")\n"
        "    os.execute(\"echo MARK-3-child-err 1>&2\")\n"
        "    io.write(\"MARK-4-io-write\\n\")\n"
        "    io.stderr:write(\"MARK-5-io-stderr\\n\")\n"
        "    os.execute(\"echo MARK-6-child-last\")\n"
        "    return true\n"
        "end\n");
    const fs::path logPath = temp / "logs" / "hooks" / "order.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.pkg_name = "hook-log-order";
    ctx.version = "0.0.1";
    ctx.hook_log = logPath;

    HookResult result;
    const auto escaped = run_captured([&] {
        result = exec->run_hook(HookType::Install, ctx);
    });

    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_EQ(escaped.out.find("MARK-"), std::string::npos)
        << "a child reached the process's stdout:\n" << escaped.out;
    EXPECT_EQ(escaped.err.find("MARK-"), std::string::npos)
        << "a child reached the process's stderr:\n" << escaped.err;

    const std::string logged = slurp(logPath);
    EXPECT_EQ(logged.rfind("# install hook of hook-log-order@0.0.1\n", 0), 0u) << logged;
    for (const std::string& text : {logged, result.output}) {
        std::size_t previous = 0;
        for (const char* marker : {"MARK-1-print", "MARK-2-child-out", "MARK-3-child-err",
                                   "MARK-4-io-write", "MARK-5-io-stderr",
                                   "MARK-6-child-last"}) {
            const auto at = text.find(marker);
            ASSERT_NE(at, std::string::npos) << marker << " missing from:\n" << text;
            EXPECT_GE(at, previous) << marker << " is out of order in:\n" << text;
            previous = at;
        }
    }
    EXPECT_EQ(result.output.rfind("# install hook of hook-log-order@0.0.1\n", 0), 0u)
        << result.output;

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_EmptyKeepsTheChildrenOnTheInheritedStreams) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-empty-");
    const fs::path pkg = write_hook_package(temp, "hook-log-empty",
        "function install()\n"
        "    print(\"MARK-print\")\n"
        "    os.execute(\"echo MARK-child-out\")\n"
        "    os.execute(\"echo MARK-child-err 1>&2\")\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");

    HookResult legacy;
    const auto before = run_captured([&] {
        legacy = exec->run_hook(HookType::Install, ctx);
    });
    EXPECT_TRUE(legacy.success) << legacy.error;
    EXPECT_NE(before.out.find("MARK-child-out"), std::string::npos) << before.out;
    EXPECT_NE(before.err.find("MARK-child-err"), std::string::npos) << before.err;
    EXPECT_EQ(before.out.find("MARK-print"), std::string::npos);
    EXPECT_NE(legacy.output.find("MARK-print"), std::string::npos);
    EXPECT_EQ(legacy.output.find("MARK-child"), std::string::npos) << legacy.output;
    EXPECT_EQ(legacy.output.find("# install hook"), std::string::npos) << legacy.output;

    // The same executor with a log, then without one again: nothing the
    // logged run installed may outlive it.
    ctx.hook_log = temp / "empty.log";
    HookResult logged;
    const auto during = run_captured([&] {
        logged = exec->run_hook(HookType::Install, ctx);
    });
    EXPECT_EQ(during.out.find("MARK-"), std::string::npos) << during.out;
    EXPECT_EQ(during.err.find("MARK-"), std::string::npos) << during.err;
    EXPECT_NE(logged.output.find("MARK-child-out"), std::string::npos) << logged.output;

    ctx.hook_log.clear();
    const auto after = run_captured([&] {
        legacy = exec->run_hook(HookType::Install, ctx);
    });
    EXPECT_NE(after.out.find("MARK-child-out"), std::string::npos)
        << "os.execute stayed redirected after the hook returned:\n" << after.out;
    EXPECT_NE(after.err.find("MARK-child-err"), std::string::npos) << after.err;
    EXPECT_EQ(legacy.output.find("MARK-child"), std::string::npos) << legacy.output;

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_ExecuteReturnsWhatLuaOsExecuteReturns) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-triples-");
    std::string body =
        "local function show(...)\n"
        "    local parts = {}\n"
        "    for i = 1, select('#', ...) do parts[i] = tostring((select(i, ...))) end\n"
        "    print(\"RESULT \" .. table.concat(parts, \" \"))\n"
        "end\n"
        "function install()\n"
        "    show(os.execute(\"exit 0\"))\n"
        "    show(os.execute(\"exit 3\"))\n"
        "    show(os.execute())\n"
#ifndef _WIN32
        "    show(os.execute(\"kill -TERM $$\"))\n"
        "    show(os.execute(\"exit 300\"))\n"
#endif
        "    return true\n"
        "end\n";
    const fs::path pkg = write_hook_package(temp, "hook-log-triples", body);

    auto results = [&](const fs::path& hookLog) {
        auto exec = create_executor(pkg);
        EXPECT_TRUE(exec.has_value());
        auto ctx = make_context(temp / "install", "linux");
        ctx.hook_log = hookLog;
        const auto result = exec->run_hook(HookType::Install, ctx);
        EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
        std::vector<std::string> lines;
        for (auto& line : split_lines(result.output)) {
            if (line.rfind("RESULT ", 0) == 0) lines.push_back(std::move(line));
        }
        return lines;
    };

    const auto native = results({});
    const auto redirected = results(temp / "triples.log");

    EXPECT_EQ(redirected, native)
        << "the redirecting os.execute must answer exactly like Lua's";
    ASSERT_GE(redirected.size(), 3u);
    EXPECT_EQ(redirected[0], "RESULT true exit 0");
    EXPECT_EQ(redirected[1], "RESULT nil exit 3");
    EXPECT_EQ(redirected[2], "RESULT true");
#ifndef _WIN32
    ASSERT_EQ(redirected.size(), 5u);
    EXPECT_EQ(redirected[3], "RESULT nil signal 15");
    EXPECT_EQ(redirected[4], "RESULT nil exit 44");  // 300 & 0xff
#endif

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_TtyOptOutReachesTheTerminalAndNotTheLog) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-tty-");
    const fs::path pkg = write_hook_package(temp, "hook-log-tty",
        "function install()\n"
        "    system.exec(\"echo MARK-tty\", { tty = true })\n"
        "    system.exec(\"echo MARK-logged\")\n"
        "    system.exec(\"echo MARK-logged-too\", { tty = false })\n"
        "    return true\n"
        "end\n");
    const fs::path logPath = temp / "tty.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.hook_log = logPath;

    HookResult result;
    const auto escaped = run_captured([&] {
        result = exec->run_hook(HookType::Install, ctx);
    });

    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(escaped.out.find("MARK-tty"), std::string::npos) << escaped.out;
    EXPECT_EQ(escaped.out.find("MARK-logged"), std::string::npos) << escaped.out;
    const std::string logged = slurp(logPath);
    const auto loggedLines = split_lines(logged);
    EXPECT_EQ(logged.find("MARK-tty"), std::string::npos) << logged;
    EXPECT_NE(std::ranges::find(loggedLines, "MARK-logged"), loggedLines.end()) << logged;
    EXPECT_NE(std::ranges::find(loggedLines, "MARK-logged-too"), loggedLines.end()) << logged;
    EXPECT_EQ(result.output.find("MARK-tty"), std::string::npos) << result.output;

    fs::remove_all(temp);
}

#ifndef _WIN32
TEST(ExecutorTest, HookLog_RunInScriptIsLoggedUnlessAdmin) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-script-");
    const fs::path pkg = write_hook_package(temp, "hook-log-script",
        "function install()\n"
        "    system.run_in_script(\"#!/bin/sh\\necho MARK-script-out\\necho MARK-script-err >&2\\n\")\n"
        "    return true\n"
        "end\n");
    const fs::path logPath = temp / "script.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.hook_log = logPath;

    HookResult result;
    const auto escaped = run_captured([&] {
        result = exec->run_hook(HookType::Install, ctx);
    });

    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_EQ(escaped.out.find("MARK-"), std::string::npos) << escaped.out;
    EXPECT_EQ(escaped.err.find("MARK-"), std::string::npos) << escaped.err;
    const std::string logged = slurp(logPath);
    EXPECT_NE(logged.find("MARK-script-out"), std::string::npos) << logged;
    EXPECT_NE(logged.find("MARK-script-err"), std::string::npos) << logged;

    fs::remove_all(temp);
}
#endif

TEST(ExecutorTest, HookLog_UnopenableLogNeverFailsTheHook) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-unopenable-");
    const fs::path pkg = write_hook_package(temp, "hook-log-unopenable",
        "function install()\n"
        "    print(\"MARK-kept\")\n"
        "    local ok = os.execute(\"echo MARK-lost-child\")\n"
        "    assert(ok == true, \"the command must still run\")\n"
        "    return true\n"
        "end\n");
    // A path below a regular file can be neither created nor opened.
    const fs::path blocker = temp / "blocker";
    write_text(blocker, "not a directory");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.hook_log = blocker / "hooks" / "x.log";

    HookResult result;
    const auto escaped = run_captured([&] {
        result = exec->run_hook(HookType::Install, ctx);
    });

    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_EQ(escaped.out.find("MARK-lost-child"), std::string::npos)
        << "with no log the child's output goes to the null device, not the terminal:\n"
        << escaped.out;
    EXPECT_NE(result.output.find("MARK-kept"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("hook output log unavailable"), std::string::npos)
        << result.output;
    const auto first = result.output.find("hook output log unavailable");
    EXPECT_EQ(result.output.find("hook output log unavailable", first + 1),
              std::string::npos) << "the warning must be given once:\n" << result.output;
    EXPECT_EQ(escaped.out.find("hook output log unavailable"), std::string::npos);

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_CreatesItsDirectoryAndStartsEveryRunFresh) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-fresh-");
    const fs::path pkg = write_hook_package(temp, "hook-log-fresh",
        "function install()\n"
        "    print(\"MARK-\" .. tostring(_RUNTIME.args[1]))\n"
        "    return true\n"
        "end\n");
    const fs::path logPath = temp / "a" / "b" / "fresh.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.hook_log = logPath;

    ctx.args = {"first"};
    EXPECT_TRUE(exec->run_hook(HookType::Install, ctx).success);
    ctx.args = {"second"};
    EXPECT_TRUE(exec->run_hook(HookType::Install, ctx).success);

    const std::string logged = slurp(logPath);
    EXPECT_EQ(logged.find("MARK-first"), std::string::npos) << logged;
    EXPECT_NE(logged.find("MARK-second"), std::string::npos) << logged;
    EXPECT_EQ(logged.find("# install hook"), 0u) << logged;
    EXPECT_EQ(logged.find("# install hook", 1), std::string::npos) << logged;

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_OutputIsTheTailOfAFileThatKeepsEverything) {
    constexpr std::size_t outputCap = 16 * 1024;
    constexpr std::string_view truncatedMarker =
        "\n[libxpkg: hook output truncated]\n";
    const fs::path temp = make_temp_dir("libxpkg-hook-log-tail-");
    const fs::path pkg = write_hook_package(temp, "hook-log-tail",
        "function install()\n"
        "    io.write(\"HEAD-MARK\")\n"
        "    io.write(string.rep(\"x\", 40000))\n"
        "    os.execute(\"echo TAIL-MARK\")\n"
        "    return true\n"
        "end\n");
    const fs::path logPath = temp / "tail.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = make_context(temp / "install", "linux");
    ctx.hook_log = logPath;
    const auto result = exec->run_hook(HookType::Install, ctx);

    EXPECT_TRUE(result.success) << result.error;
    EXPECT_LE(result.output.size(), outputCap + truncatedMarker.size());
    EXPECT_NE(result.output.find("TAIL-MARK"), std::string::npos);
    EXPECT_EQ(result.output.find("HEAD-MARK"), std::string::npos);
    EXPECT_EQ(result.output.find(truncatedMarker), 0u) << "the cut must be announced";
    EXPECT_TRUE(is_valid_utf8(result.output));

    const std::string logged = slurp(logPath);
    EXPECT_GT(logged.size(), 40000u);
    EXPECT_NE(logged.find("HEAD-MARK"), std::string::npos)
        << "the file is the full record; only HookResult::output is a tail";

    fs::remove_all(temp);
}

TEST(ExecutorTest, HookLog_ScriptsAreNotIntercepted) {
    const fs::path temp = make_temp_dir("libxpkg-hook-log-script-main-");
    const fs::path pkg = temp / "script-main.lua";
    write_text(pkg,
        "package = { name = \"script-main\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function xpkg_main()\n"
        "    os.execute(\"echo MARK-script-child\")\n"
        "end\n");
    const fs::path logPath = temp / "script-main.log";

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.platform = "linux";
    ctx.hook_log = logPath;

    HookResult result;
    const auto escaped = run_captured([&] { result = exec->run_script(ctx); });

    EXPECT_TRUE(result.success) << result.error;
    EXPECT_NE(escaped.out.find("MARK-script-child"), std::string::npos) << escaped.out;
    EXPECT_FALSE(fs::exists(logPath)) << "run_script has no hook to log";

    fs::remove_all(temp);
}

TEST(ExecutorTest, PkgInfo_BuildDepReadsTheVariableXlingsExports) {
    const fs::path temp = make_temp_dir("libxpkg-build-dep-name-");
    const fs::path sevenZip = temp / "payloads" / "7zip";
    const fs::path myTool = temp / "payloads" / "my-tool";
    fs::create_directories(sevenZip);
    fs::create_directories(myTool);
    // xlings exports the bare name: no namespace, no @version, upper-cased,
    // everything that is not alphanumeric mapped to '_'.
    ScopedEnvVar sevenZipVar("XLINGS_BUILDDEP_7ZIP_PATH", sevenZip.string());
    ScopedEnvVar myToolVar("XLINGS_BUILDDEP_MY_TOOL_PATH", myTool.string());

    const fs::path pkg = write_hook_package(temp, "build-dep-name",
        "local pkginfo = import(\"xim.libxpkg.pkginfo\")\n"
        "local function expect(spelling, dir)\n"
        "    local dep = pkginfo.build_dep(spelling)\n"
        "    assert(dep ~= nil, spelling .. \" resolved to nothing\")\n"
        "    assert(dep.path == dir, spelling .. \" -> \" .. tostring(dep.path))\n"
        "end\n"
        "function install()\n"
        "    expect(\"xim:7zip\", [[" + sevenZip.string() + "]])\n"
        "    expect(\"7zip\", [[" + sevenZip.string() + "]])\n"
        "    expect(\"xim:7zip@26.02\", [[" + sevenZip.string() + "]])\n"
        "    expect(\"7zip@26.02\", [[" + sevenZip.string() + "]])\n"
        "    expect(\"xim:my-tool@1.0\", [[" + myTool.string() + "]])\n"
        "    expect(\"my-tool\", [[" + myTool.string() + "]])\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    const auto result = exec->run_hook(HookType::Install,
                                       make_context(temp / "install", "linux"));
    EXPECT_TRUE(result.success) << result.error << "\n" << result.output;

    fs::remove_all(temp);
}

TEST(ExecutionBoundaryTest, FactoryDoesNotExecuteAnyRecipeLuaOnTheHost) {
    const auto temp = make_temp_dir("libxpkg-boundary-deferred-");
    const auto marker = temp / "top-level-ran";
    const auto recipe = temp / "deferred.lua";
    write_text(recipe, "local f = assert(io.open([[" + marker.string() + "]], 'w'))\n"
                       "f:write('top level'); f:close()\n"
                       "function install() error('must never run on host') end\n");
    int calls = 0;
    auto exec = create_executor(recipe, [&](const HookInvocation& request)
        -> std::expected<HookResponse, std::string> {
        ++calls;
        EXPECT_EQ(request.package, recipe);
        HookResponse response;
        response.result.success = true;
        response.hooks[static_cast<std::size_t>(HookType::Install)] = true;
        return response;
    });
    ASSERT_TRUE(exec) << exec.error();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(exec->has_hook(HookType::Install));
    EXPECT_FALSE(exec->has_hook(HookType::Config));
    EXPECT_FALSE(fs::exists(marker));
    EXPECT_TRUE(exec->run_hook(HookType::Install, {}).success);
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(fs::exists(marker));
    fs::remove_all(temp);
}

TEST(ExecutionBoundaryTest, TheLegacyFactoryStillLoadsAndRunsLocally) {
    const auto temp = make_temp_dir("libxpkg-boundary-legacy-");
    const auto marker = temp / "top-level-ran";
    const auto recipe = temp / "local.lua";
    write_text(recipe, "local f = assert(io.open([[" + marker.string() + "]], 'w'))\n"
                       "f:write('top level'); f:close()\n"
                       "function install() return true end\n");
    auto exec = create_executor(recipe);
    ASSERT_TRUE(exec) << exec.error();
    EXPECT_TRUE(fs::is_regular_file(marker));
    EXPECT_TRUE(exec->run_hook(HookType::Install, {}).success);
    fs::remove_all(temp);
}

TEST(ExecutionBoundaryTest, OneWorkerRetainsRecipeStateAndCumulativeEffectsAcrossHooks) {
    const auto temp = make_temp_dir("libxpkg-boundary-effects-");
    const auto recipe = temp / "effects.lua";
    write_text(recipe, R"LUA(
        import('xim.libxpkg.xvm')
        import('xim.libxpkg.pkgmanager')
        local installed = false
        function install()
            installed = true
            xvm.add('first', {version = '1.0.0', args = {'with space', '--flag'},
                             envs = {MODE = 'one'}, binding = 'suite@1'})
            pkgmanager.install('xim:first@1.0.0')
            return true
        end
        function config()
            assert(installed, 'recipe state was lost between hooks')
            xvm.add('second', {type = 'files', src = 'share/data', dst = 'usr/share/data'})
            pkgmanager.remove('xim:old')
            return true
        end
    )LUA");
    // This local callback checks the worker protocol, not OS isolation.
    HookWorker worker;
    auto exec = create_executor(recipe, [&](const HookInvocation& request) { return worker.dispatch(request); });
    ASSERT_TRUE(exec) << exec.error();
    EXPECT_TRUE(exec->has_hook(HookType::Config));
    auto install = make_context(temp / "payload", "linux");
    ASSERT_TRUE(exec->run_hook(HookType::Install, install).success);
    ASSERT_EQ(exec->xvm_operations().size(), 1u);
    ASSERT_EQ(exec->install_requests().size(), 1u);
    auto config = install;
    config.install_dir = temp / "new-payload";
    ASSERT_TRUE(exec->run_hook(HookType::Config, config).success);
    auto ops = exec->xvm_operations();
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].name, "first");
    EXPECT_EQ(ops[0].args, (std::vector<std::string>{"with space", "--flag"}));
    ASSERT_EQ(ops[0].envs.size(), 1u);
    EXPECT_EQ(ops[0].envs[0], (std::pair<std::string, std::string>{"MODE", "one"}));
    EXPECT_EQ(ops[0].binding, "suite@1");
    EXPECT_EQ(ops[1].name, "second");
    EXPECT_EQ(ops[1].bindir, config.install_dir.string());
    EXPECT_EQ(ops[1].src, "share/data");
    EXPECT_EQ(ops[1].dst, "usr/share/data");
    auto requests = exec->install_requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].op, "install");
    EXPECT_EQ(requests[0].target, "xim:first@1.0.0");
    EXPECT_EQ(requests[1].op, "remove");
    EXPECT_EQ(requests[1].target, "xim:old");
    EXPECT_FALSE(exec->has_hook(HookType::Uninstall));
    fs::remove_all(temp);
}

TEST(ExecutionBoundaryTest, FailuresNeverFallBackToExecutingTheRecipeLocally) {
    const auto temp = make_temp_dir("libxpkg-boundary-failure-");
    const auto marker = temp / "must-not-run";
    const auto recipe = temp / "unsafe.lua";
    write_text(recipe, "assert(io.open([[" + marker.string() + "]], 'w')):close()\n"
                       "function install() return true end\n");
    EXPECT_FALSE(create_executor(recipe, ExecutionBoundary{}));
    auto load_failure = create_executor(recipe, [](const HookInvocation&)
        -> std::expected<HookResponse, std::string> { return std::unexpected("worker unavailable"); });
    EXPECT_FALSE(load_failure);
    auto exec = create_executor(recipe, [](const HookInvocation& request)
        -> std::expected<HookResponse, std::string> {
        if (request.action != HookAction::Load) return std::unexpected("worker died");
        HookResponse response;
        response.result.success = true;
        response.hooks[static_cast<std::size_t>(HookType::Install)] = true;
        return response;
    });
    ASSERT_TRUE(exec) << exec.error();
    const auto result = exec->run_hook(HookType::Install, {});
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("worker died"), std::string::npos);
    EXPECT_FALSE(fs::exists(marker));
    exec->set_log_level("info");
    EXPECT_FALSE(exec->run_hook(HookType::Install, {}).success);
    EXPECT_FALSE(fs::exists(marker));
    fs::remove_all(temp);
}

TEST(ExecutionBoundaryTest, WorkerRefusesMissingLoadPackageChangesAndUnknownActions) {
    HookWorker worker;
    EXPECT_FALSE(worker.dispatch({.action = HookAction::RunHook, .package = HELLO_PKG}));
    auto loaded = worker.dispatch({.action = HookAction::Load, .package = HELLO_PKG});
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_FALSE(worker.dispatch({.action = HookAction::Load, .package = HELLO_PKG}));
    EXPECT_FALSE(worker.dispatch({.action = HookAction::RunHook, .package = "another.lua"}));
    EXPECT_FALSE(worker.dispatch({.action = static_cast<HookAction>(99), .package = HELLO_PKG}));
    EXPECT_FALSE(worker.dispatch({.action = HookAction::RunHook, .package = HELLO_PKG,
                                 .hook = static_cast<HookType>(99)}));
    EXPECT_FALSE(worker.dispatch({.action = HookAction::SetLogLevel, .package = HELLO_PKG,
                                 .log_level = "info'); os.execute('unsafe') --"}));
    EXPECT_FALSE(hook_action_from_string("unknown"));
    EXPECT_EQ(hook_action_from_string("run_hook"), HookAction::RunHook);
}

TEST(ExecutionBoundaryTest, MovingADeferredExecutorRetainsTheWorkerAndHookLogContext) {
    const auto temp = make_temp_dir("libxpkg-boundary-log-");
    const auto recipe = temp / "logged.lua";
    write_text(recipe, "function install() print('worker captured this'); return true end\n");
    HookWorker worker;
    auto exec = create_executor(recipe, [&](const HookInvocation& request) { return worker.dispatch(request); });
    ASSERT_TRUE(exec) << exec.error();
    auto moved = std::move(*exec);
    moved.set_log_level("info");
    auto ctx = make_context(temp, "linux");
    ctx.hook_log = temp / "hooks" / "install.log";
    const auto result = moved.run_hook(HookType::Install, ctx);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_NE(result.output.find("worker captured this"), std::string::npos);
    std::ifstream in(ctx.hook_log);
    const std::string log{std::istreambuf_iterator<char>(in), {}};
    EXPECT_NE(log.find("worker captured this"), std::string::npos);
    EXPECT_TRUE(moved.has_hook(HookType::Install));
    fs::remove_all(temp);
}
