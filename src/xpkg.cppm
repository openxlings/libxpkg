module;
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <expected>
#include <optional>

export module mcpplibs.xpkg;

export namespace mcpplibs::xpkg {

// The highest `spec` this library reads. "1" and "2" run with the
// compatibility layer; "3" runs in the environment without it (design
// 2026-11-27 §9.6). A recipe of a higher spec is one for a newer client: a
// host refuses it and names the update.
inline constexpr int kHighestSpec = 3;

// The number a recipe's `spec` names ("2" -> 2), or nullopt when it names none:
// empty, or not a whole number. A recipe that names none is read as before
// any spec existed, with the compatibility layer.
std::optional<int> spec_number(std::string_view spec);

enum class PackageType   { Package, Script, Template, Config, Subos };
enum class PackageStatus { Dev, Stable, Deprecated };

// One CPU architecture as the xpkg format names it: the name recipes and hosts
// agree on, the names upstreams use for it, and how an ELF header says it.
//
// There is one table of these (arch_identities()). The loader reads it to
// canonicalize names and to know which keys of a version entry are per-arch
// resources; elfpatch reads it to tell a file built for this machine from one
// built for another; a host reads it to check that a payload is its machine's
// (design 2026-11-27 §5.1). An architecture added here is known to all of them
// at once. Properties that only one program needs (a standard loader path, a
// boot file name) stay in that program, keyed by `name`.
struct ArchIdentity {
    std::string_view name;                      // canonical, e.g. "x86_64"
    std::span<const std::string_view> aliases;  // other spellings, lower case
    std::uint16_t elf_machine;                  // e_machine
    int elf_bits;                               // the ELF class: 32 or 64
};

std::span<const ArchIdentity> arch_identities();
// The identity a canonical name or an alias names (case-insensitive), or null.
const ArchIdentity* find_arch(std::string_view name);
// The identity an ELF header's e_machine and class describe, or null.
const ArchIdentity* find_arch_by_elf(std::uint16_t machine, int bits);

// Canonicalize a CPU-arch token to its name in the identity table, so recipes
// can use upstream names verbatim (amd64, arm64, i686, loong64, ...). A token
// the table does not know is returned lower-cased. Case-insensitive.
std::string normalize_arch(std::string_view raw);

// True when two arch tokens denote the same canonical arch (e.g. "arm64"
// and "aarch64"). Used by the install-time resolver and `package.archs`
// validation so authors and hosts can disagree on spelling.
bool arch_matches(std::string_view a, std::string_view b);

// A single arch's download resource. Lighter than PlatformResource (no
// `ref`, no nested archs) so PlatformResource::archs stays a map of a
// COMPLETE type — std::unordered_map does not support incomplete value
// types, which a recursive PlatformResource-in-PlatformResource would be.
struct ArchResource {
    std::string url;
    std::string sha256;
    std::unordered_map<std::string, std::string> mirrors;  // region: GLOBAL/CN
};

struct PlatformResource {
    std::string url;     // single url | URL template ("...${arch}...") | "XLINGS_RES" | "" when `archs` used
    std::string sha256;  // single-arch sha256 | "" when sha256_by_arch used
    std::string ref;     // version alias, e.g. "latest" -> "1.0.0"
    std::unordered_map<std::string, std::string> mirrors;  // e.g. "GLOBAL"->url, "CN"->url
    // ---- V2 multi-arch additions. All empty/false => legacy V1 resource,
    // parsed and resolved exactly as before. ----
    std::unordered_map<std::string, ArchResource> archs;        // canonical-arch -> resource (Scheme B: per-arch map)
    std::unordered_map<std::string, std::string> sha256_by_arch; // canonical-arch -> sha256 (Scheme C: template / res)
    std::unordered_map<std::string, std::string> arch_alias;     // canonical-arch -> upstream token for ${arch_alias}
    bool is_res = false;                                         // res=true: XLINGS_RES with per-arch checksums
    // Packaging revision of this version's payload: a change to what the
    // recipe installs that keeps the upstream version. 0 when the entry
    // states none. A client records the revision it installed and treats a
    // different recipe revision as not installed (xpkg V2 spec, "revision").
    int revision = 0;
    // The oldest packaging revision of this version that is still usable
    // (`floor = N`, 0 <= N <= revision): a client keeps an installed payload
    // whose recorded revision is at least the floor, and replaces one below
    // it. 0 when the entry states none -- every installed revision stays
    // usable, and a raised `revision` only changes what a new install gets
    // (xpkg V2 spec, "floor"). A floor above the revision reads as the
    // revision.
    int floor = 0;
    // The oldest client that can use this version (`min_client =
    // "2026.10.16.1"`): a client resolving a package with no version named
    // passes over the versions it cannot use. Empty when the entry states none.
    std::string min_client;
    // How the host lands the download in the install directory (`strip = n`,
    // spec "3"): the first n components of every member's path are dropped.
    // -1 when the entry states none (the platform's, else 0, applies; see
    // PlatformMatrix::platform_strip and resolve_resource).
    int strip = -1;
};

// What this package exposes to consumers/xlings at install/runtime.
// All paths are RELATIVE to the package's install_dir; xlings joins them
// with the actual on-disk install path on the target machine to get the
// final absolute path. Per the design doc (2026-05-02-elfpatch-exports-design.md),
// providers only declare fields they actually expose — `{lib64, lib}`
// libdir convention covers 99% of packages and need not be re-declared.
struct ExportsRuntime {
    // Dynamic linker (PT_INTERP) path, relative to install_dir.
    // Only libc-class providers (glibc, musl) declare this.
    std::string loader;
    // Library search dirs (RPATH closure). Empty == fall back to the
    // {lib64, lib} convention. Only declare if the package's layout
    // diverges from convention.
    std::vector<std::string> libdirs;
    // ABI tag, used to disambiguate when multiple deps provide loaders
    // (e.g. "linux-x86_64-glibc" vs "linux-x86_64-musl"). The consumer
    // can request a specific abi via `elfpatch.set({interp_from = ...})`.
    std::string abi;
};

struct ExportsBlock {
    ExportsRuntime runtime;
    // Future extension points: data (ssl_certs / locale / ...),
    // build (include / cmake / pkgconfig). Not in v1.
};

struct PlatformMatrix {
    // Optional resource defaults. `source` applies to every platform while a
    // platform-specific value overrides it. Values are either "xlings-res"
    // or a URL template; version entries keep the existing model unchanged.
    // String form remains the canonical/default source for V1 and V2.
    // `source_mirrors` is populated when source is a GLOBAL/CN map.
    std::string source;
    std::unordered_map<std::string, std::string> source_mirrors;
    std::unordered_map<std::string, std::string> platform_sources;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> platform_source_mirrors;
    // platform -> version -> resource
    std::unordered_map<std::string,
        std::unordered_map<std::string, PlatformResource>> entries;
    // platform -> list of dep names. `deps` is the **effective union** of
    // `runtime_deps` and `build_deps`, populated by the loader so legacy
    // consumers reading `deps` keep working unchanged.
    std::unordered_map<std::string, std::vector<std::string>> deps;
    // platform -> list of dep names that are needed at the consumer's
    // run-time (activated in subos workspace, exposed via shim/PATH or
    // linked at consumer-build).
    std::unordered_map<std::string, std::vector<std::string>> runtime_deps;
    // platform -> list of dep names that are needed only while THIS
    // package is being installed/built. The installer downloads them
    // to the xpkgs store but does NOT register them in the xvm DB or
    // the active workspace, so the user's tool versions stay untouched.
    // Install hooks access them via injected env vars / pkginfo API.
    std::unordered_map<std::string, std::vector<std::string>> build_deps;
    // platform -> what this package exposes (loader, libdirs, abi, ...)
    // See ExportsBlock above. Empty for packages that don't declare anything;
    // the predicate-driven elfpatch trigger uses these to decide whether
    // a consumer needs INTERP/RPATH patching after install.
    std::unordered_map<std::string, ExportsBlock> exports;
    // platform inheritance, e.g. "ubuntu" -> "linux"
    std::unordered_map<std::string, std::string> inherits;
    // platform -> `strip` stated for all of its versions (an entry's own wins).
    std::unordered_map<std::string, int> platform_strip;
    // Declared outside struct body → outlined symbol in module object.
    // Prevents test/consumer TUs from inlining the body and then generating
    // unsatisfied calls to std internal inline helpers (e.g. ~_Vector_impl).
    ~PlatformMatrix();
};

struct Package {
    std::string spec;
    std::string name;
    std::string description;
    PackageType  type   = PackageType::Package;
    PackageStatus status = PackageStatus::Dev;
    std::string namespace_;
    std::string homepage, repo, docs;
    std::vector<std::string> authors, maintainers, licenses;
    std::vector<std::string> categories, keywords, programs, archs;
    bool xvm_enable = false;
    PlatformMatrix xpm;
    // Namespaced data for other programs (`extensions = { luban = {...} }`):
    // each first-level key names the program that reads it. libxpkg and xlings
    // keep it and hand it on unchanged, never interpreting it; it does not
    // change what is installed (xpkg V2 spec, "extensions"). JSON text of an
    // object with sorted keys; empty when the recipe states none.
    std::string extensions;
    // Host files whose contents the package's config() reads to decide what it
    // configures (`host_facts = { linux = { "/sys/module/nvidia/version" } }`):
    // platform -> absolute paths, in the order written. A client records a
    // digest of these files when it configures the package and compares it
    // where the configuration is used; a different digest means config() runs
    // again (xpkg V2 spec, "host_facts"). Data, never a hook: checking it runs
    // none of the recipe's code. Empty when the recipe states none.
    std::unordered_map<std::string, std::vector<std::string>> host_facts;
    // What the loader dropped while reading the recipe, one line each (an
    // `extensions` value JSON cannot carry, an `extensions` over the limit, a
    // `host_facts` entry that is not an absolute path).
    std::vector<std::string> warnings;
    ~Package();
};

struct PackageIdentity {
    std::string namespaceName;
    std::string name;

    std::string canonical_name() const;
};

struct IndexEntry {
    PackageIdentity identity;
    std::string canonicalName;
    std::string entryKey;
    std::string name;         // descriptor package.name
    std::string version;
    std::filesystem::path path;
    PackageType type  = PackageType::Package;
    std::string description;
    bool installed    = false;
    std::string ref;          // alias target, e.g. "vscode@1.85.0"
};

struct PackageIndex {
    std::unordered_map<std::string, IndexEntry> entries;
    std::unordered_map<std::string, std::vector<std::string>> identityEntries;
    std::unordered_map<std::string, std::vector<std::string>> shortNames;
    std::unordered_map<std::string, std::vector<std::string>> mutex_groups;
    ~PackageIndex();
};

struct RepoConfig {
    std::string name;          // namespace; empty for main repo
    std::string url_global, url_cn;
    std::filesystem::path local_path;
};

struct IndexRepos {
    RepoConfig main_repo;
    std::vector<RepoConfig> sub_repos;
    ~IndexRepos();
};

} // namespace mcpplibs::xpkg

// Out-of-line destructor definitions (not inline, not in-class-body).
// GCC emits these as outlined symbols in the module's compiled object.
// Importing TUs call the outlined versions, so std internals like
// ~_Vector_impl() and ~_Hashtable_alloc() are only needed at the
// xpkg.cppm compilation site — where all headers are in scope.
namespace mcpplibs::xpkg {
PlatformMatrix::~PlatformMatrix() = default;
Package::~Package()       = default;
PackageIndex::~PackageIndex() = default;
IndexRepos::~IndexRepos() = default;

std::string PackageIdentity::canonical_name() const {
    if (namespaceName.empty()) return name;
    return namespaceName + ":" + name;
}

namespace {

constexpr std::string_view kX86_64Aliases[]  = {"amd64", "x64", "x86-64"};
constexpr std::string_view kAarch64Aliases[] = {"arm64", "armv8"};
constexpr std::string_view kX86Aliases[]     = {"i386", "i686"};
constexpr std::string_view kArmAliases[]     = {"armv7", "armv7l", "armhf"};
constexpr std::string_view kLoongAliases[]   = {"loong64"};

// e_machine values from the ELF specification (EM_*).
constexpr std::array<ArchIdentity, 6> kArchIdentities{{
    {"x86_64",      kX86_64Aliases,  62,  64},
    {"aarch64",     kAarch64Aliases, 183, 64},
    {"x86",         kX86Aliases,     3,   32},
    {"arm",         kArmAliases,     40,  32},
    {"riscv64",     {},              243, 64},
    {"loongarch64", kLoongAliases,   258, 64},
}};

std::string lower_(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw)
        s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return s;
}

} // namespace

std::span<const ArchIdentity> arch_identities() { return kArchIdentities; }

const ArchIdentity* find_arch(std::string_view name) {
    const auto s = lower_(name);
    for (const auto& arch : kArchIdentities) {
        if (arch.name == s) return &arch;
        for (const auto alias : arch.aliases)
            if (alias == s) return &arch;
    }
    return nullptr;
}

const ArchIdentity* find_arch_by_elf(std::uint16_t machine, int bits) {
    for (const auto& arch : kArchIdentities)
        if (arch.elf_machine == machine && arch.elf_bits == bits) return &arch;
    return nullptr;
}

std::string normalize_arch(std::string_view raw) {
    if (const auto* arch = find_arch(raw)) return std::string(arch->name);
    return lower_(raw);
}

std::optional<int> spec_number(std::string_view spec) {
    if (spec.empty() || spec.size() > 4) return std::nullopt;
    int n = 0;
    for (char c : spec) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + (c - '0');
    }
    return n;
}

bool arch_matches(std::string_view a, std::string_view b) {
    return normalize_arch(a) == normalize_arch(b);
}
}
