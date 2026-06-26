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

// ---------------------------------------------------------------------------
// Arch normalization
// ---------------------------------------------------------------------------

TEST(ArchTest, NormalizesAliases) {
    EXPECT_EQ(normalize_arch("amd64"),   "x86_64");
    EXPECT_EQ(normalize_arch("x64"),     "x86_64");
    EXPECT_EQ(normalize_arch("x86-64"),  "x86_64");
    EXPECT_EQ(normalize_arch("x86_64"),  "x86_64");   // canonical passthrough
    EXPECT_EQ(normalize_arch("arm64"),   "aarch64");
    EXPECT_EQ(normalize_arch("armv8"),   "aarch64");
    EXPECT_EQ(normalize_arch("aarch64"), "aarch64");
    EXPECT_EQ(normalize_arch("AArch64"), "aarch64");  // case-insensitive
}

TEST(ArchTest, ArchMatchesAcrossSpellings) {
    EXPECT_TRUE(arch_matches("arm64", "aarch64"));
    EXPECT_TRUE(arch_matches("amd64", "x86_64"));
    EXPECT_FALSE(arch_matches("x86_64", "aarch64"));
}

// ---------------------------------------------------------------------------
// V2 multi-arch xpm shapes
// ---------------------------------------------------------------------------

// Scheme B: per-arch resource map. Each arch carries its own url + sha256;
// the single-arch url/sha256 stay empty. Arch keys normalize to canonical.
TEST(LoaderTest, V2_PerArchMap_ParsesBothArches) {
    auto result = load_package(PKGINDEX / "pkgs/v/v2map.lua");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& r = result->xpm.entries.at("linux").at("1.0.0");
    EXPECT_TRUE(r.url.empty());
    EXPECT_TRUE(r.sha256.empty());
    ASSERT_EQ(r.archs.size(), 2u);
    EXPECT_EQ(r.archs.at("x86_64").url,    "https://ex/v2map-1.0.0-linux-x86_64.tar.gz");
    EXPECT_EQ(r.archs.at("x86_64").sha256, "aaaa");
    EXPECT_EQ(r.archs.at("aarch64").url,   "https://ex/v2map-1.0.0-linux-aarch64.tar.gz");
    EXPECT_EQ(r.archs.at("aarch64").sha256, "bbbb");
}

// Scheme C: a URL template plus a per-arch sha256 table and an arch_alias
// map. The template string is kept verbatim (expanded only at install time).
TEST(LoaderTest, V2_Template_ParsesShaMapAndAlias) {
    auto result = load_package(PKGINDEX / "pkgs/v/v2tmpl.lua");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& r = result->xpm.entries.at("linux").at("1.0.0");
    EXPECT_EQ(r.url, "https://ex/${name}-${version}-${os}-${arch_alias}.${ext}");
    EXPECT_TRUE(r.sha256.empty());  // sha256 was a table, not a string
    ASSERT_EQ(r.sha256_by_arch.size(), 2u);
    EXPECT_EQ(r.sha256_by_arch.at("x86_64"),  "aaaa");
    EXPECT_EQ(r.sha256_by_arch.at("aarch64"), "bbbb");
    EXPECT_EQ(r.arch_alias.at("x86_64"),  "amd64");
    EXPECT_EQ(r.arch_alias.at("aarch64"), "arm64");
    EXPECT_FALSE(r.is_res);
}

// res shape: XLINGS_RES auto-URL plus per-arch checksums (closes the
// XLINGS_RES "no sha256" gap). is_res flags install-time URL synthesis.
TEST(LoaderTest, V2_Res_ParsesFlagAndShaMap) {
    auto result = load_package(PKGINDEX / "pkgs/v/v2res.lua");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& r = result->xpm.entries.at("linux").at("1.0.0");
    EXPECT_TRUE(r.is_res);
    ASSERT_EQ(r.sha256_by_arch.size(), 2u);
    EXPECT_EQ(r.sha256_by_arch.at("x86_64"),  "aaaa");
    EXPECT_EQ(r.sha256_by_arch.at("aarch64"), "bbbb");
    EXPECT_TRUE(r.archs.empty());  // res shape is not a per-arch map
}

// Legacy single-arch entries must be entirely unaffected by V2 parsing.
TEST(LoaderTest, V2_LegacySingleArch_Unchanged) {
    auto result = load_package(PKGINDEX / "pkgs/h/hello.lua");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& r = result->xpm.entries.at("linux").at("1.0.0");
    EXPECT_EQ(r.url, "https://example.com/hello-1.0.0-linux.tar.gz");
    EXPECT_FALSE(r.sha256.empty());
    EXPECT_TRUE(r.archs.empty());
    EXPECT_TRUE(r.sha256_by_arch.empty());
    EXPECT_FALSE(r.is_res);
}
