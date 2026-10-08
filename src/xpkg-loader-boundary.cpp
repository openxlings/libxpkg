module mcpplibs.xpkg.loader;

import std;
import mcpplibs.xpkg;

namespace mcpplibs::xpkg {

std::expected<Package, std::string>
load_package(const fs::path& pkg_path, const LoaderContext& context, MetadataBoundary boundary) {
    if (!boundary) return std::unexpected("metadata execution boundary is empty");
    try {
        auto result = boundary({.package = pkg_path, .context = context});
        if (!result) return std::unexpected("metadata boundary: " + result.error());
        return result;
    } catch (const std::exception& error) {
        return std::unexpected("metadata boundary: " + std::string(error.what()));
    } catch (...) {
        return std::unexpected("metadata boundary failed");
    }
}

std::expected<PackageIndex, std::string>
build_index(const fs::path& repo_dir, const std::string& defaultNamespace,
            const LoaderContext& context, const LoaderBoundaries& boundaries,
            const BuildOutput& buildOutput) {
    return loader_detail::build_index_impl(repo_dir, defaultNamespace, buildOutput, context, &boundaries);
}

}  // namespace mcpplibs::xpkg

namespace mcpplibs::xpkg::loader_detail {

std::expected<PackageIndex, std::string>
build_index_impl(const fs::path& repo_dir, const std::string& defaultNamespace,
                 const BuildOutput& buildOutput, const LoaderContext& context,
                 const LoaderBoundaries* boundaries) {
    PackageIndex index;
    auto pkgs_dir = repo_dir / "pkgs";
    if (!fs::is_directory(pkgs_dir))
        return std::unexpected("pkgs/ directory not found in: " + repo_dir.string());

    if (boundaries) {
        if (!boundaries->metadata) return std::unexpected("metadata execution boundary is empty");
        std::error_code ec;
        const auto script = repo_dir / "pkgindex-build.lua";
        const auto status = fs::symlink_status(script, ec);
        const bool missing = status.type() == fs::file_type::not_found
            && (!ec || ec == std::errc::no_such_file_or_directory);
        if (!missing) {
            if (ec) return std::unexpected(script.string() + ": " + ec.message());
            if (!boundaries->index_build)
                return std::unexpected("index build script requires an execution boundary: " + script.string());
            try {
                const auto built = boundaries->index_build(repo_dir, buildOutput);
                if (!built) return std::unexpected("index build boundary: " + built.error());
            } catch (const std::exception& error) {
                return std::unexpected("index build boundary: " + std::string(error.what()));
            } catch (...) {
                return std::unexpected("index build boundary failed");
            }
        }
    } else {
        run_pkgindex_build(repo_dir, buildOutput);
    }

    std::vector<fs::path> packagePaths;
    for (auto& letter_dir : fs::directory_iterator(pkgs_dir)) {
        if (!letter_dir.is_directory()) continue;
        for (auto& entry : fs::directory_iterator(letter_dir)) {
            if (entry.path().extension() != ".lua") continue;
            packagePaths.push_back(entry.path().lexically_normal());
        }
    }
    std::ranges::sort(packagePaths);

    for (auto& packagePath : packagePaths) {
        auto result = boundaries ? load_package(packagePath, context, boundaries->metadata)
                                 : load_package(packagePath);
        if (!result) {
            if (boundaries) return std::unexpected(packagePath.string() + ": " + result.error());
            continue;  // legacy callers skip malformed packages
        }
        auto& pkg = *result;

        PackageIdentity identity {
            .namespaceName = pkg.namespace_.empty()
                ? defaultNamespace
                : pkg.namespace_,
            .name = pkg.name,
        };
        auto canonicalName = identity.canonical_name();

        IndexEntry indexEntry;
        indexEntry.identity = std::move(identity);
        indexEntry.canonicalName = canonicalName;
        indexEntry.entryKey = canonicalName;
        indexEntry.name = pkg.name;
        indexEntry.path = packagePath;
        indexEntry.type = pkg.type;
        indexEntry.description = pkg.description;

        auto existing = index.entries.find(indexEntry.entryKey);
        if (existing != index.entries.end()) {
            return std::unexpected(std::format(
                "duplicate package identity '{}': '{}' conflicts with '{}'",
                canonicalName,
                existing->second.path.string(),
                packagePath.string()));
        }

        index.entries.emplace(indexEntry.entryKey, std::move(indexEntry));
        index.identityEntries[canonicalName].push_back(canonicalName);
        index.shortNames[pkg.name].push_back(canonicalName);
    }

    for (auto& [_, candidates] : index.identityEntries) {
        std::ranges::sort(candidates);
        auto uniqueEnd = std::ranges::unique(candidates).begin();
        candidates.erase(uniqueEnd, candidates.end());
    }
    for (auto& [_, candidates] : index.shortNames) {
        std::ranges::sort(candidates);
        auto uniqueEnd = std::ranges::unique(candidates).begin();
        candidates.erase(uniqueEnd, candidates.end());
    }

    return index;
}

}  // namespace mcpplibs::xpkg::loader_detail
