package = {
    spec    = "2",
    name    = "v2tmpl",
    description = "V2 fixture: URL template + per-arch sha256 (Scheme C)",
    type    = "package",
    archs   = {"x86_64", "aarch64"},
    status  = "stable",
    categories = {"test"},
    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://ex/${name}-${version}-${os}-${arch_alias}.${ext}",
                sha256 = {
                    x86_64  = "aaaa",
                    aarch64 = "bbbb",
                },
                arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
            },
        },
    },
}
