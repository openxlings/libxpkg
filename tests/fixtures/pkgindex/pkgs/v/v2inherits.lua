package = {
    spec = "1",
    name = "v2inherits",
    xpm = {
        linux = {
            deps = { "xim:zlib" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = { url = "https://example.test/1.0.0.tar.gz", sha256 = "aa" },
            ["1.1.0"] = { url = "https://example.test/1.1.0.tar.gz", sha256 = "cc", min_client = "2026.10.16.2" },
        },
        -- V1 platform reference
        macosx = { ref = "linux" },
        -- inherits, with a version of its own
        windows = {
            inherits = "linux",
            ["2.0.0"] = { url = "https://example.test/2.0.0.zip", sha256 = "bb" },
        },
        -- a chain, and a cycle
        ubuntu = { ref = "debian" },
        debian = { inherits = "linux" },
        a = { ref = "b" },
        b = { ref = "a" },
    },
}
