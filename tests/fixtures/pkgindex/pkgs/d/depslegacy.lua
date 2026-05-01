package = {
    spec    = "1",
    name    = "depslegacy",
    description = "Legacy array-form deps (compat path: fans out to runtime + build)",
    licenses = {"MIT"},
    repo = "https://example.com/depslegacy",
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            deps = { "node", "npm" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {
                url = "https://example.com/depslegacy-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
