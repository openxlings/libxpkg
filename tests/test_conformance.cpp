// XPKG-CONFORMANCE: the format's conformance cases (tests/conformance/,
// design 2026-11-27 §9.9) through this library: each recipe read, its
// resources resolved and its hooks run by the executor, and what each case
// states holds. A host runs the same cases through its own install.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.executor;
import mcpplibs.xpkg.conformance;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;
namespace cf = mcpplibs::xpkg::conformance;

namespace {

#if defined(_WIN32)
constexpr std::string_view kPlatform = "windows";
#elif defined(__APPLE__)
constexpr std::string_view kPlatform = "macosx";
#else
constexpr std::string_view kPlatform = "linux";
#endif

std::optional<HookType> hook_type(std::string_view hook) {
    if (hook == "installed") return HookType::Installed;
    if (hook == "install") return HookType::Install;
    if (hook == "config") return HookType::Config;
    if (hook == "uninstall") return HookType::Uninstall;
    return std::nullopt;
}

struct Dir {
    fs::path root;
    explicit Dir(std::string_view name)
        : root(fs::temp_directory_path() / std::format("xpkg-conformance-{}-{}", name,
              std::chrono::steady_clock::now().time_since_epoch().count())) {
        fs::create_directories(root);
    }
    ~Dir() { std::error_code ec; fs::remove_all(root, ec); }
};

void run_case(const cf::Case& c) {
    SCOPED_TRACE(std::format("case {} ({}): {}", c.name, c.chapter, c.summary));
    Dir dir(c.name);
    const auto index = dir.root / "index";
    const auto recipe = cf::materialize(c, index);
    ASSERT_TRUE(fs::is_regular_file(recipe)) << recipe;

    auto loaded = load_package(recipe);
    if (auto e = c.loaded.find("error"); e != c.loaded.end()) {
        ASSERT_FALSE(loaded.has_value()) << "the recipe loads";
        EXPECT_NE(loaded.error().find(e->second), std::string::npos) << loaded.error();
        return;
    }
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    const auto fields = cf::fields(*loaded);
    for (const auto& [key, want] : c.loaded) {
        const auto got = fields.find(key);
        ASSERT_NE(got, fields.end()) << "no field " << key;
        EXPECT_EQ(got->second, want) << "field " << key;
    }

    for (const auto& r : c.resolve) {
        SCOPED_TRACE(std::format("resolve {}-{} {}", r.platform, r.arch, r.version));
        auto rr = resolve_resource(loaded->xpm, { .name = loaded->name, .version = r.version,
                                                  .platform = r.platform, .arch = r.arch });
        if (!r.error.empty()) {
            EXPECT_FALSE(rr.has_value()) << "resolves to " << (rr ? rr->url : "");
            if (!rr) EXPECT_NE(rr.error().find(r.error), std::string::npos) << rr.error();
            continue;
        }
        ASSERT_TRUE(rr.has_value()) << rr.error();
        if (!r.url.empty()) EXPECT_EQ(rr->url, r.url);
        if (!r.sha256.empty()) EXPECT_EQ(rr->sha256, r.sha256);
        if (r.strip) EXPECT_EQ(rr->strip, *r.strip);
    }

    auto exec = create_executor(recipe);
    if (!c.executor_error.empty()) {
        ASSERT_FALSE(exec.has_value()) << "an executor is made";
        EXPECT_NE(exec.error().find(c.executor_error), std::string::npos) << exec.error();
        return;
    }
    if (c.hooks.empty()) return;
    ASSERT_TRUE(exec.has_value()) << exec.error();

    ExecutionContext ctx;
    ctx.pkg_name = loaded->name;
    ctx.version = "1.0.0";
    ctx.platform = std::string(kPlatform);
    ctx.arch = "x86_64";
    ctx.install_dir = dir.root / "install";
    ctx.pkgindex_dir = index.string();
    fs::create_directories(ctx.install_dir);
    for (const auto& step : c.hooks) {
        SCOPED_TRACE("hook " + step.hook);
        const auto type = hook_type(step.hook);
        ASSERT_TRUE(type.has_value()) << "no hook " << step.hook;
        const auto result = exec->run_hook(*type, ctx);
        EXPECT_EQ(result.success, step.ok) << result.error << "\n" << result.output;
        if (!step.error.empty()) EXPECT_NE(result.error.find(step.error), std::string::npos) << result.error;
        EXPECT_EQ(result.call_failures.size(), step.call_failures);
        for (const auto& want : step.notices) {
            const bool said = std::ranges::any_of(result.notices, [&](const HookNotice& n) {
                return n.level == want.level && n.text.find(want.contains) != std::string::npos;
            });
            EXPECT_TRUE(said) << "no " << want.level << " notice with " << want.contains;
        }
        if (step.notices.empty()) EXPECT_TRUE(result.notices.empty()) << result.notices.front().text;
        for (const auto& problem : cf::unmet(step.after, ctx.install_dir)) ADD_FAILURE() << problem;
    }
}

} // namespace

TEST(ConformanceTest, EveryChapterHasCases) {
    std::set<std::string> chapters;
    for (const auto& c : cf::cases()) chapters.insert(c.chapter);
    EXPECT_EQ(chapters, (std::set<std::string>{"versions", "package", "xpm", "lifecycle", "environment",
                                               "declarations", "index"}));
}

TEST(ConformanceTest, EveryCaseHolds) {
    ASSERT_FALSE(cf::cases().empty());
    for (const auto& c : cf::cases()) run_case(c);
}

TEST(ConformanceTest, AnExpectationSaysWhatDoesNotHold) {
    Dir dir("unmet");
    std::ofstream(dir.root / "f", std::ios::binary) << "x";
    const auto problems = cf::unmet({ { .path = "f", .is = "file", .text = "y" },
                                      { .path = "d", .is = "dir" },
                                      { .path = "f", .is = "absent" } }, dir.root);
    ASSERT_EQ(problems.size(), 3u);
    EXPECT_NE(problems[1].find("d: is absent, expected dir"), std::string::npos) << problems[1];
}
