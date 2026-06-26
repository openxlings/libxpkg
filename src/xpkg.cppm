module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <expected>

export module mcpplibs.xpkg;

export namespace mcpplibs::xpkg {

enum class PackageType   { Package, Script, Template, Config, Subos };
enum class PackageStatus { Dev, Stable, Deprecated };

// Canonicalize a CPU-arch token to xlings' internal spelling.
// Accepts common aliases so recipes can use upstream names verbatim:
//   amd64 / x64 / x86-64 / x86_64  -> "x86_64"
//   arm64 / armv8  / aarch64       -> "aarch64"
//   x86   / i386   / i686          -> "x86"
// Unknown tokens are returned lower-cased unchanged. Case-insensitive.
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
    ~Package();
};

struct IndexEntry {
    std::string name;         // e.g. "vscode@1.85.0"
    std::string version;
    std::filesystem::path path;
    PackageType type  = PackageType::Package;
    std::string description;
    bool installed    = false;
    std::string ref;          // alias target, e.g. "vscode@1.85.0"
};

struct PackageIndex {
    std::unordered_map<std::string, IndexEntry> entries;
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

std::string normalize_arch(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw)
        s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (s == "amd64" || s == "x64" || s == "x86-64" || s == "x86_64") return "x86_64";
    if (s == "arm64" || s == "armv8" || s == "aarch64")               return "aarch64";
    if (s == "x86"   || s == "i386" || s == "i686")                   return "x86";
    return s;
}

bool arch_matches(std::string_view a, std::string_view b) {
    return normalize_arch(a) == normalize_arch(b);
}
}
