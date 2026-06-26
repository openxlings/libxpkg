package = {
    spec    = "2",
    name    = "v2res",
    description = "V2 fixture: XLINGS_RES with per-arch checksums (res shape)",
    type    = "package",
    archs   = {"x86_64", "aarch64"},
    status  = "stable",
    categories = {"test"},
    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                res = true,
                sha256 = {
                    x86_64  = "aaaa",
                    aarch64 = "bbbb",
                },
            },
        },
    },
}
