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
