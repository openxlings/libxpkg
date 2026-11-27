#include <gtest/gtest.h>
#include <cstdlib>
#include "support/min_elf.hpp"

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

// elfpatch decides what it would change from the files, then looks for its
// tool, and hands back facts and an outcome (design 2026-11-27 §9.3,
// xlings#741 UP-L1). The host judges them.

namespace {

constexpr std::uint16_t kX86_64 = 62;
constexpr std::uint16_t kAarch64 = 183;

struct Scratch {
    fs::path dir = fs::temp_directory_path() / ("xpkg-elfpatch-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { fs::create_directories(dir / "install"); fs::create_directories(dir / "empty-path"); }
    ~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }
    fs::path recipe(std::string_view body) const {
        auto file = dir / "elfpatch-probe.lua";
        std::ofstream(file) << body;
        return file;
    }
    ExecutionContext context() const {
        ExecutionContext ctx;
        ctx.pkg_name = "elfpatch-probe";
        ctx.version = "1.0.0";
        ctx.platform = "linux";
        ctx.arch = "x86_64";
        ctx.install_dir = dir / "install";
        ctx.bin_dir = dir / "empty-path";
        return ctx;
    }
};

// PATH holds `first` and the system's directories (elfpatch lists files with
// `find`), not the directories this process was started with, where a test
// runner may have put its own patchelf.
struct ScopedPath {
    std::string old;
    explicit ScopedPath(const fs::path& first) {
        if (const char* p = std::getenv("PATH")) old = p;
        ::setenv("PATH", (first.string() + ":/usr/bin:/bin").c_str(), 1);
    }
    ~ScopedPath() { ::setenv("PATH", old.c_str(), 1); }
};

// The host's own patchelf would be found after PATH; these cases need a
// machine without one.
bool host_has_patchelf() {
    return fs::is_regular_file("/usr/bin/patchelf") || fs::is_regular_file("/usr/local/bin/patchelf")
        || fs::is_regular_file("/bin/patchelf");
}

constexpr std::string_view kHeader =
    "package = { spec = \"2\", name = \"elfpatch-probe\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
    "local elfpatch = import(\"xim.libxpkg.elfpatch\")\n";

} // namespace

TEST(ElfpatchOutcomeTest, FilesToPatchAndNoToolIsAFailureThatNamesTheTool) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    if (host_has_patchelf()) GTEST_SKIP() << "this machine has a patchelf of its own";
    Scratch s;
    write_min_elf64(s.dir / "loader" / "ld.so", kX86_64, false, 0);
    write_min_elf64(s.dir / "install" / "bin" / "app", kX86_64, true, 1);
    auto exec = create_executor(s.recipe(std::string(kHeader) +
        "function install() elfpatch.set({ interpreter = \"" + (s.dir / "loader" / "ld.so").generic_string() +
        "\" }) return true end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ScopedPath path(s.dir / "empty-path");
    ASSERT_TRUE(exec->run_hook(HookType::Install, s.context()).success);
    auto r = exec->apply_elfpatch_auto();
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_TRUE(r.elfpatch.has_value());
    EXPECT_EQ(r.elfpatch->outcome, "failed");
    EXPECT_EQ(r.elfpatch->tool_missing, "patchelf");
    EXPECT_NE(r.elfpatch->reason.find("patchelf is needed to patch 1 file(s)"), std::string::npos)
        << r.elfpatch->reason;
    EXPECT_EQ(r.elfpatch->patched, 0);
}

// Nothing to patch: no tool is needed, and its absence is no failure.
TEST(ElfpatchOutcomeTest, NothingToPatchNeedsNoTool) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    if (host_has_patchelf()) GTEST_SKIP() << "this machine has a patchelf of its own";
    Scratch s;
    write_min_elf64(s.dir / "loader" / "ld.so", kX86_64, false, 0);
    std::ofstream(s.dir / "install" / "README") << "text, not ELF";
    auto exec = create_executor(s.recipe(std::string(kHeader) +
        "function install() elfpatch.set({ interpreter = \"" + (s.dir / "loader" / "ld.so").generic_string() +
        "\" }) return true end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ScopedPath path(s.dir / "empty-path");
    ASSERT_TRUE(exec->run_hook(HookType::Install, s.context()).success);
    auto r = exec->apply_elfpatch_auto();
    ASSERT_TRUE(r.elfpatch.has_value());
    EXPECT_EQ(r.elfpatch->outcome, "done");
    EXPECT_TRUE(r.elfpatch->tool_missing.empty());
}

// A recipe that names a file to change is told when it cannot be changed.
TEST(ElfpatchOutcomeTest, AnExplicitCallWithoutTheToolFailsTheHook) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    if (host_has_patchelf()) GTEST_SKIP() << "this machine has a patchelf of its own";
    Scratch s;
    write_min_elf64(s.dir / "install" / "bin" / "app", kX86_64, true, 1);
    auto exec = create_executor(s.recipe(std::string(kHeader) +
        "function install()\n"
        "    elfpatch.set_interpreter(\"" + (s.dir / "install" / "bin" / "app").generic_string() + "\", \"/lib64/ld.so\")\n"
        "    return true\n"
        "end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ScopedPath path(s.dir / "empty-path");
    auto r = exec->run_hook(HookType::Install, s.context());
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("elfpatch.set_interpreter: patchelf is needed to change 1 file(s)"), std::string::npos)
        << r.error;
    EXPECT_NE(r.error.find("elfpatch-probe.lua:4"), std::string::npos) << "the recipe's line: " << r.error;
}

// What is left alone is counted by why: facts for the host's machine rule.
TEST(ElfpatchOutcomeTest, LeftAloneFilesAreCountedByWhy) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    Scratch s;
    const auto tools = s.dir / "tools";
    fs::create_directories(tools);
    {
        std::ofstream f(tools / "patchelf");
        f << "#!/bin/sh\nif [ \"$1\" = \"--print-interpreter\" ]; then echo /lib64/ld.so; fi\nexit 0\n";
    }
    fs::permissions(tools / "patchelf", fs::perms::owner_all, fs::perm_options::replace);
    write_min_elf64(s.dir / "loader" / "ld.so", kX86_64, false, 0);
    write_min_elf64(s.dir / "install" / "bin" / "app", kX86_64, true, 1);
    write_min_elf64(s.dir / "install" / "prebuilds" / "arm64.node", kAarch64, false, 1);
    write_min_elf64(s.dir / "install" / "bin" / "static", kX86_64, false, 0);
    write_min_elf64(s.dir / "install" / "bin" / "listed", kX86_64, true, 1);
    auto exec = create_executor(s.recipe(std::string(kHeader) +
        "function install() elfpatch.set({ interpreter = \"" + (s.dir / "loader" / "ld.so").generic_string() +
        "\", skip = { \"bin/listed\" } }) return true end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ScopedPath path(tools);
    ASSERT_TRUE(exec->run_hook(HookType::Install, s.context()).success);
    auto r = exec->apply_elfpatch_auto();
    ASSERT_TRUE(r.elfpatch.has_value());
    EXPECT_EQ(r.elfpatch->outcome, "done");
    EXPECT_EQ(r.elfpatch->scanned, 4);
    EXPECT_EQ(r.elfpatch->patched, 1);
    EXPECT_EQ(r.elfpatch->skipped_foreign, 1);
    EXPECT_EQ(r.elfpatch->skipped_static, 1);
    EXPECT_EQ(r.elfpatch->skipped_listed, 1);
    EXPECT_EQ(r.output, "4 1 0");
}

// Not applicable is skipped, with the reason.
TEST(ElfpatchOutcomeTest, NoLoaderProviderIsSkippedAndSaysWhy) {
    Scratch s;
    auto exec = create_executor(s.recipe(std::string(kHeader) + "function install() return true end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ASSERT_TRUE(exec->run_hook(HookType::Install, s.context()).success);
    auto r = exec->apply_elfpatch_auto();
    ASSERT_TRUE(r.elfpatch.has_value());
    EXPECT_EQ(r.elfpatch->outcome, "skipped");
#if defined(_WIN32)
    EXPECT_NE(r.elfpatch->reason.find("Windows"), std::string::npos) << r.elfpatch->reason;
#else
    EXPECT_EQ(r.elfpatch->reason, "no runtime dependency provides a loader");
#endif
}

// The machine of the install decides which files are foreign when no loader
// file can be read (the table of architectures, not a guess at a name).
TEST(ElfpatchOutcomeTest, TheInstallsArchDecidesForeignWithoutALoaderFile) {
#if !defined(__linux__)
    GTEST_SKIP() << "the ELF patch path runs on linux hosts only";
#endif
    Scratch s;
    const auto tools = s.dir / "tools";
    fs::create_directories(tools);
    {
        std::ofstream f(tools / "patchelf");
        f << "#!/bin/sh\nif [ \"$1\" = \"--print-interpreter\" ]; then echo /lib/ld.so; fi\nexit 0\n";
    }
    fs::permissions(tools / "patchelf", fs::perms::owner_all, fs::perm_options::replace);
    write_min_elf64(s.dir / "install" / "bin" / "app", kX86_64, true, 1);
    write_min_elf64(s.dir / "install" / "bin" / "app-arm", kAarch64, true, 1);
    auto exec = create_executor(s.recipe(std::string(kHeader) +
        "function install() elfpatch.set({ interpreter = \"/lib/no-such-loader.so\" }) return true end\n"));
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ScopedPath path(tools);
    auto ctx = s.context();
    ctx.arch = "arm64";  // an alias: the table knows it is aarch64
    ASSERT_TRUE(exec->run_hook(HookType::Install, ctx).success);
    auto r = exec->apply_elfpatch_auto();
    ASSERT_TRUE(r.elfpatch.has_value());
    EXPECT_EQ(r.elfpatch->skipped_foreign, 1);
    EXPECT_EQ(r.elfpatch->patched, 1);
}
