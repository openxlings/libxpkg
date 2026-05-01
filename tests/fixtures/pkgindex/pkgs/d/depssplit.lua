package = {
    spec    = "1",
    name    = "depssplit",
    description = "Split-form deps with separate runtime/build lists",
    licenses = {"MIT"},
    repo = "https://example.com/depssplit",
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            deps = {
                runtime = { "node", "npm" },
                build   = { "gcc", "patchelf" },
            },
            ["latest"] = { ref = "2.0.0" },
            ["2.0.0"]  = {
                url = "https://example.com/depssplit-2.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
