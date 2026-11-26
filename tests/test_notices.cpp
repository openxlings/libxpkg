#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

struct Recipe {
    fs::path dir;
    fs::path file;
    explicit Recipe(std::string_view body) {
        dir = fs::temp_directory_path() / ("xpkg-notices-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "install");
        file = dir / "notice.lua";
        std::ofstream(file) << body;
    }
    ~Recipe() { std::error_code ec; fs::remove_all(dir, ec); }
    ExecutionContext context() const {
        ExecutionContext ctx;
        ctx.pkg_name = "notice";
        ctx.version = "1.0.0";
        ctx.platform = "linux";
        ctx.arch = "x86_64";
        ctx.install_dir = dir / "install";
        return ctx;
    }
};

constexpr std::string_view kHeader =
    "package = { spec = \"2\", name = \"notice\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
    "import(\"xim.libxpkg.log\")\n";

} // namespace

// log.warn and log.error are what a recipe says to the user: kept in order,
// with their level and formatted text, beside the output that still has them.
TEST(NoticesTest, WarnAndErrorAreKeptInOrderAndInfoIsNot) {
    Recipe r(std::string(kHeader) +
             "function install()\n"
             "    log.info('progress %d', 1)\n"
             "    log.warn('the license of %s needs accepting', 'demo')\n"
             "    log.debug('detail')\n"
             "    log.error('a step left for you: %s', 'run demo --init')\n"
             "    return true\n"
             "end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto result = exec->run_hook(HookType::Install, r.context());
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.notices.size(), 2u);
    EXPECT_EQ(result.notices[0].level, "warn");
    EXPECT_EQ(result.notices[0].text, "the license of demo needs accepting");
    EXPECT_EQ(result.notices[1].level, "error");
    EXPECT_EQ(result.notices[1].text, "a step left for you: run demo --init");
    EXPECT_NE(result.output.find("[WARN] the license of demo needs accepting"), std::string::npos)
        << result.output;
}

// The level set for the output decides what the output shows, not what the
// recipe said: a quiet host still gets the notices.
TEST(NoticesTest, TheOutputLevelDoesNotDropNotices) {
    Recipe r(std::string(kHeader) +
             "function install() log.warn('still said') return true end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    exec->set_log_level("silent");
    auto result = exec->run_hook(HookType::Install, r.context());
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.notices.size(), 1u);
    EXPECT_EQ(result.notices[0].text, "still said");
    EXPECT_EQ(result.output.find("still said"), std::string::npos) << result.output;
}

// A hook that fails after saying something still hands it back, and the
// next hook starts with none.
TEST(NoticesTest, AFailedHookKeepsItsNoticesAndTheNextStartsEmpty) {
    Recipe r(std::string(kHeader) +
             "function install() log.warn('before the failure') error('broke') end\n"
             "function config() return true end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto failed = exec->run_hook(HookType::Install, r.context());
    EXPECT_FALSE(failed.success);
    ASSERT_EQ(failed.notices.size(), 1u);
    EXPECT_EQ(failed.notices[0].text, "before the failure");
    auto next = exec->run_hook(HookType::Config, r.context());
    EXPECT_TRUE(next.success) << next.error;
    EXPECT_TRUE(next.notices.empty());
}

TEST(NoticesTest, ManyNoticesAreBoundedAndCounted) {
    Recipe r(std::string(kHeader) +
             "function install() for i = 1, 150 do log.warn('line %d', i) end return true end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto result = exec->run_hook(HookType::Install, r.context());
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.notices.size(), kMaxHookNotices);
    EXPECT_EQ(result.notices.front().text, "line 1");
    EXPECT_EQ(result.notices.back().text, "51 more lines; the hook's output has them");
}

// The host's own warning about the hook log is not something the recipe said.
TEST(NoticesTest, TheHostsLogWarningIsNotANotice) {
    Recipe r(std::string(kHeader) + "function install() return true end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto ctx = r.context();
    // A log under a regular file cannot be created.
    std::ofstream(r.dir / "a-file") << "x";
    ctx.hook_log = r.dir / "a-file" / "hook.log";
    auto result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_NE(result.output.find("hook output log unavailable"), std::string::npos) << result.output;
    EXPECT_TRUE(result.notices.empty());
}

TEST(NoticesTest, AScriptsNoticesAreKept) {
    Recipe r("package = { spec = \"2\", name = \"notice\", type = \"script\", xpm = { linux = { [\"1.0.0\"] = {} } } }\n"
             "import(\"xim.libxpkg.log\")\n"
             "function xpkg_main(...) log.warn('from a script') end\n");
    auto exec = create_executor(r.file);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    auto result = exec->run_script(r.context());
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.notices.size(), 1u);
    EXPECT_EQ(result.notices[0].text, "from a script");
}
