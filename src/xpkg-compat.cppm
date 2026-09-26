module;

export module mcpplibs.xpkg.compat;
import mcpplibs.xpkg;
import std;

export namespace mcpplibs::xpkg {

enum class SourceKind {
    None,
    ExplicitUrl,
    XlingsRes,
    UrlTemplate,
};

struct ResourceContext {
    std::string name;
    std::string version;
    std::string platform;
    std::string arch;
    std::string ext;
};

struct ResolvedResource {
    SourceKind kind = SourceKind::None;
    std::string version;
    std::string url;
    std::string sha256;
    std::unordered_map<std::string, std::string> mirrors;
    // The packaging revision of the entry that supplies the payload: for a
    // `ref` alias, the revision of the entry the alias resolves to.
    int revision = 0;
};

std::expected<ResolvedResource, std::string> resolve_resource(
    const PlatformMatrix& matrix,
    const ResourceContext& context);

} // namespace mcpplibs::xpkg

namespace mcpplibs::xpkg {
namespace {

bool is_xlings_res(std::string_view value) {
    return value == "xlings-res" || value == "XLINGS_RES";
}

std::string replace_all(
    std::string value,
    std::string_view needle,
    std::string_view replacement) {
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return value;
}

std::string expand_template(
    std::string value,
    const ResourceContext& context,
    std::string_view version,
    std::string_view arch,
    std::string_view arch_alias) {
    auto ext = context.ext;
    if (ext.empty())
        ext = context.platform == "windows" ? "zip" : "tar.gz";
    value = replace_all(std::move(value), "${name}", context.name);
    value = replace_all(std::move(value), "${version}", version);
    value = replace_all(std::move(value), "${os}", context.platform);
    value = replace_all(std::move(value), "${arch}", arch);
    value = replace_all(std::move(value), "${arch_alias}", arch_alias);
    return replace_all(std::move(value), "${ext}", ext);
}

} // namespace

std::expected<ResolvedResource, std::string> resolve_resource(
    const PlatformMatrix& matrix,
    const ResourceContext& context) {
    auto platform_it = matrix.entries.find(context.platform);
    if (platform_it == matrix.entries.end())
        return std::unexpected("unsupported platform: " + context.platform);

    const auto& versions = platform_it->second;
    auto version_it = versions.find(context.version);
    if (version_it == versions.end())
        return std::unexpected("unknown version: " + context.version);

    std::unordered_set<std::string> visited;
    std::string resolved_version = context.version;
    const PlatformResource* resource = &version_it->second;
    while (!resource->ref.empty()) {
        if (!visited.insert(resolved_version).second)
            return std::unexpected("resource ref cycle at version: " + resolved_version);
        resolved_version = resource->ref;
        version_it = versions.find(resolved_version);
        if (version_it == versions.end())
            return std::unexpected("resource ref target not found: " + resolved_version);
        resource = &version_it->second;
    }

    const auto arch = normalize_arch(context.arch);
    const auto arch_it = resource->archs.find(arch);
    if (!resource->archs.empty() && arch_it == resource->archs.end())
        return std::unexpected(
            "no resource for arch '" + arch + "' in version "
            + resolved_version);
    if (!resource->sha256_by_arch.empty()
            && !resource->sha256_by_arch.contains(arch))
        return std::unexpected(
            "no checksum for arch '" + arch + "' in version "
            + resolved_version);
    const ArchResource* arch_resource =
        arch_it == resource->archs.end() ? nullptr : &arch_it->second;

    ResolvedResource result;
    result.version = resolved_version;
    result.revision = resource->revision;
    if (arch_resource != nullptr) {
        result.url = arch_resource->url;
        result.sha256 = arch_resource->sha256;
        result.mirrors = arch_resource->mirrors;
    } else {
        result.url = resource->url;
        result.sha256 = resource->sha256;
        result.mirrors = resource->mirrors;
        if (auto hash = resource->sha256_by_arch.find(arch);
                hash != resource->sha256_by_arch.end())
            result.sha256 = hash->second;
    }

    auto arch_alias = arch;
    if (auto alias = resource->arch_alias.find(arch);
            alias != resource->arch_alias.end())
        arch_alias = alias->second;

    std::string source;
    std::unordered_map<std::string, std::string> source_mirrors;
    if (auto source_it = matrix.platform_sources.find(context.platform);
            source_it != matrix.platform_sources.end())
        source = source_it->second;
    else
        source = matrix.source;

    const bool has_platform_source =
        matrix.platform_sources.contains(context.platform);

    if (has_platform_source) {
        if (auto mirrors_it = matrix.platform_source_mirrors.find(context.platform);
                mirrors_it != matrix.platform_source_mirrors.end())
            source_mirrors = mirrors_it->second;
    } else {
        source_mirrors = matrix.source_mirrors;
    }

    if (result.mirrors.empty() && !source_mirrors.empty())
        result.mirrors = source_mirrors;

    if (result.url.empty() && (resource->is_res || is_xlings_res(source))) {
        result.kind = SourceKind::XlingsRes;
    } else if (is_xlings_res(result.url)) {
        result.kind = SourceKind::XlingsRes;
        result.url.clear();
    } else {
        if (result.url.empty())
            result.url = source;
        if (!result.url.empty()) {
            const bool templated = result.url.find("${") != std::string::npos;
            result.kind = templated ? SourceKind::UrlTemplate
                                    : SourceKind::ExplicitUrl;
            result.url = expand_template(
                std::move(result.url), context, resolved_version, arch, arch_alias);
        }
    }

    for (auto& [region, url] : result.mirrors)
        url = expand_template(
            std::move(url), context, resolved_version, arch, arch_alias);

    if (result.kind == SourceKind::None)
        return std::unexpected(
            "resource has neither url nor source for " + context.platform
            + "@" + resolved_version);
    return result;
}

} // namespace mcpplibs::xpkg
