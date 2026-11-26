#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

fs::path write_recipe(std::string_view name, std::string_view body) {
    auto dir = fs::temp_directory_path() / ("xpkg-arch-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    auto file = dir / (std::string(name) + ".lua");
    std::ofstream(file) << body;
    return file;
}

} // namespace

// The identity table is the one statement of what an architecture is: its
// name, the names upstreams use, and how an ELF header says it.
TEST(ArchIdentityTest, EveryNameAndAliasFindsItsIdentity) {
    for (const auto& arch : arch_identities()) {
        ASSERT_EQ(find_arch(arch.name), &arch) << arch.name;
        for (const auto alias : arch.aliases)
            EXPECT_EQ(find_arch(alias), &arch) << alias << " -> " << arch.name;
        EXPECT_EQ(find_arch_by_elf(arch.elf_machine, arch.elf_bits), &arch) << arch.name;
        EXPECT_EQ(normalize_arch(arch.name), arch.name);
    }
}

TEST(ArchIdentityTest, ElfFactsOfTheHostArchitectures) {
    const auto* x86_64 = find_arch("amd64");
    ASSERT_NE(x86_64, nullptr);
    EXPECT_EQ(x86_64->name, "x86_64");
    EXPECT_EQ(x86_64->elf_machine, 62);
    EXPECT_EQ(x86_64->elf_bits, 64);
    const auto* aarch64 = find_arch("ARM64");
    ASSERT_NE(aarch64, nullptr);
    EXPECT_EQ(aarch64->name, "aarch64");
    EXPECT_EQ(aarch64->elf_machine, 183);
    // The same e_machine at another class is another architecture (or none).
    EXPECT_EQ(find_arch_by_elf(62, 32), nullptr);
    EXPECT_EQ(find_arch_by_elf(243, 64)->name, "riscv64");
    EXPECT_EQ(find_arch("sparc"), nullptr);
}

TEST(ArchIdentityTest, NormalizeKnowsEveryArchitectureOfTheTable) {
    EXPECT_EQ(normalize_arch("loong64"), "loongarch64");
    EXPECT_EQ(normalize_arch("armhf"), "arm");
    EXPECT_EQ(normalize_arch("i686"), "x86");
    EXPECT_EQ(normalize_arch("RISCV64"), "riscv64");
    EXPECT_EQ(normalize_arch("Sparc64"), "sparc64");  // unknown: lower-cased, kept
}

// A per-arch resource map takes every architecture of the table, not the
// three the loader used to list: an entry for riscv64 is a resource, not a
// key the loader drops.
TEST(ArchIdentityTest, PerArchMapTakesEveryArchitectureOfTheTable) {
    const auto recipe = write_recipe("archmap", R"(
package = {
    spec = "2", name = "archmap", type = "package",
    xpm = { linux = { ["1.0.0"] = {
        x86_64  = { url = "https://ex/a-x86_64.tar.gz",  sha256 = "aa" },
        riscv64 = { url = "https://ex/a-riscv64.tar.gz", sha256 = "bb" },
        loong64 = { url = "https://ex/a-loong64.tar.gz", sha256 = "cc" },
        notes   = { text = "not an architecture" },
    } } },
}
)");
    auto pkg = load_package(recipe);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    const auto& entry = pkg->xpm.entries.at("linux").at("1.0.0");
    ASSERT_EQ(entry.archs.size(), 3u);
    EXPECT_EQ(entry.archs.at("riscv64").sha256, "bb");
    EXPECT_EQ(entry.archs.at("loongarch64").url, "https://ex/a-loong64.tar.gz");
    auto resolved = resolve_resource(pkg->xpm, {.name = "archmap", .version = "1.0.0",
                                                .platform = "linux", .arch = "riscv64"});
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(resolved->url, "https://ex/a-riscv64.tar.gz");
    fs::remove_all(recipe.parent_path());
}
