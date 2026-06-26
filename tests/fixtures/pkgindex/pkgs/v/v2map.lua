package = {
    spec    = "2",
    name    = "v2map",
    description = "V2 fixture: per-arch resource map (Scheme B)",
    type    = "package",
    archs   = {"x86_64", "aarch64"},
    status  = "stable",
    categories = {"test"},
    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                x86_64  = { url = "https://ex/v2map-1.0.0-linux-x86_64.tar.gz",  sha256 = "aaaa" },
                aarch64 = { url = "https://ex/v2map-1.0.0-linux-aarch64.tar.gz", sha256 = "bbbb" },
            },
        },
    },
}
