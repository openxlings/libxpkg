module;
#include <gtest/gtest.h>
#include <filesystem>
#include <string_view>

export module xpkg.test.loader;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

#ifndef XPKG_TEST_PKGINDEX
#  define XPKG_TEST_PKGINDEX tests/fixtures/pkgindex
#endif

#ifndef XPKG_TEST_PKGINDEX_BUILD
#  define XPKG_TEST_PKGINDEX_BUILD tests/fixtures/pkgindex-build
#endif

#define XPKG_STRINGIFY_IMPL(x) #x
#define XPKG_STRINGIFY(x) XPKG_STRINGIFY_IMPL(x)

constexpr std::string_view normalize_pkgindex_macro(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

static const fs::path PKGINDEX{
    std::string(normalize_pkgindex_macro(XPKG_STRINGIFY(XPKG_TEST_PKGINDEX)))
};

static const fs::path PKGINDEX_BUILD{
    std::string(normalize_pkgindex_macro(XPKG_STRINGIFY(XPKG_TEST_PKGINDEX_BUILD)))
};

TEST(LoaderTest, LoadPackage_MissingFile) {
    auto result = load_package("/nonexistent/pkg.lua");
    EXPECT_FALSE(result.has_value());
}

TEST(LoaderTest, LoadPackage_Hello) {
    auto result = load_package(PKGINDEX / "pkgs/h/hello.lua");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->name, "hello");
    EXPECT_EQ(result->type, PackageType::Package);
    EXPECT_EQ(result->status, PackageStatus::Stable);
    EXPECT_FALSE(result->xpm.entries.empty());
    EXPECT_TRUE(result->xvm_enable);
}

TEST(LoaderTest, LoadPackage_HasLinuxPlatform) {
    auto result = load_package(PKGINDEX / "pkgs/h/hello.lua");
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result->xpm.entries.count("linux"), 0u);
}

TEST(LoaderTest, BuildIndex_ReturnsEntries) {
    auto result = build_index(PKGINDEX);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_GT(result->entries.size(), 0u);
    EXPECT_GT(result->entries.count("hello"), 0u);
}

TEST(LoaderTest, BuildIndex_PkgindexBuild_OsFiles) {
    // Tests that build_index works with pkgindex-build.lua that uses os.files()
    // This validates the C++ std::filesystem implementation works cross-platform
    auto result = build_index(PKGINDEX_BUILD);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_GT(result->entries.count("testbuild"), 0u);
}

TEST(LoaderTest, BuildIndex_PkgindexBuild_TemplateAppended) {
    // After pkgindex-build runs, the testbuild package should have xpm data
    // from the appended template
    auto result = build_index(PKGINDEX_BUILD);
    ASSERT_TRUE(result.has_value()) << result.error();
    auto pkg = load_package(result->entries.at("testbuild").path);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    // Template adds xpm with linux/windows/macosx platforms
    EXPECT_FALSE(pkg->xpm.entries.empty()) << "template xpm should have been appended by pkgindex-build";
}

// Legacy array form: `deps = { "node", "npm" }` must populate
// runtime_deps AND build_deps identically (loader fan-out) so
// pre-split consumers keep getting the same dep set.
TEST(LoaderTest, LoadPackage_DepsLegacy_FansOutToBoth) {
    auto result = load_package(PKGINDEX / "pkgs/d/depslegacy.lua");
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& xpm = result->xpm;
    auto rt = xpm.runtime_deps.find("linux");
    auto bd = xpm.build_deps.find("linux");
    auto un = xpm.deps.find("linux");
    ASSERT_NE(rt, xpm.runtime_deps.end());
    ASSERT_NE(bd, xpm.build_deps.end());
    ASSERT_NE(un, xpm.deps.end());

    std::vector<std::string> expected{"node", "npm"};
    EXPECT_EQ(rt->second, expected);
    EXPECT_EQ(bd->second, expected);
    EXPECT_EQ(un->second, expected);
}

// Split form: deps = { runtime = {...}, build = {...} } must keep
// the two lists separate, and the legacy `deps` field must hold
// their union (preserving insertion order: runtime first, then build).
TEST(LoaderTest, LoadPackage_DepsSplit_KeepsSeparation) {
    auto result = load_package(PKGINDEX / "pkgs/d/depssplit.lua");
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& xpm = result->xpm;
    auto rt = xpm.runtime_deps.find("linux");
    auto bd = xpm.build_deps.find("linux");
    auto un = xpm.deps.find("linux");
    ASSERT_NE(rt, xpm.runtime_deps.end());
    ASSERT_NE(bd, xpm.build_deps.end());
    ASSERT_NE(un, xpm.deps.end());

    std::vector<std::string> expectedRt{"node", "npm"};
    std::vector<std::string> expectedBd{"gcc", "patchelf"};
    EXPECT_EQ(rt->second, expectedRt);
    EXPECT_EQ(bd->second, expectedBd);

    std::vector<std::string> expectedUnion{"node", "npm", "gcc", "patchelf"};
    EXPECT_EQ(un->second, expectedUnion);
}

// exports.runtime with all sub-fields populated must round-trip through the
// parser. This is the "happy path" for declarative provider metadata.
TEST(LoaderTest, LoadPackage_ExportsFull) {
    auto result = load_package(PKGINDEX / "pkgs/e/exportsfull.lua");
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& xpm = result->xpm;
    auto eit = xpm.exports.find("linux");
    ASSERT_NE(eit, xpm.exports.end());
    auto& rt = eit->second.runtime;

    EXPECT_EQ(rt.loader, "lib64/ld-linux-x86-64.so.2");
    EXPECT_EQ(rt.abi, "linux-x86_64-glibc");
    std::vector<std::string> expectedLibdirs{"lib64", "lib", "usr/lib"};
    EXPECT_EQ(rt.libdirs, expectedLibdirs);
}

// Partial declaration: only `loader` set, libdirs/abi omitted — must parse
// without error and leave the omitted fields empty (consumers fall back to
// the {lib64, lib} convention for libdirs).
TEST(LoaderTest, LoadPackage_ExportsLoaderOnly) {
    auto result = load_package(PKGINDEX / "pkgs/e/exportsloaderonly.lua");
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& xpm = result->xpm;
    auto eit = xpm.exports.find("linux");
    ASSERT_NE(eit, xpm.exports.end());
    auto& rt = eit->second.runtime;

    EXPECT_EQ(rt.loader, "lib/ld-musl-x86_64.so.1");
    EXPECT_TRUE(rt.libdirs.empty());
    EXPECT_TRUE(rt.abi.empty());
}

// Packages without an `exports` block must remain valid; the platform's
// exports map entry simply doesn't exist (consumers see "no provider
// declared" and the predicate trigger falls through to no-op).
TEST(LoaderTest, LoadPackage_NoExports) {
    auto result = load_package(PKGINDEX / "pkgs/h/hello.lua");
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& xpm = result->xpm;
    EXPECT_EQ(xpm.exports.find("linux"), xpm.exports.end());
}
