package = {
    spec    = "2",
    name    = "v2revision",
    description = "V2 fixture: packaging revision on version entries",
    type    = "package",
    archs   = {"x86_64", "aarch64"},
    status  = "stable",
    categories = {"test"},
    xpm = {
        linux = {
            ["latest"] = { ref = "1.2.0" },
            ["1.0.0"] = {
                url    = "https://ex/v2revision-1.0.0.tar.gz",
                sha256 = "aaaa",
            },
            ["1.1.0"] = {
                url      = "https://ex/v2revision-1.1.0.tar.gz",
                sha256   = "bbbb",
                revision = 2,
            },
            ["1.2.0"] = {
                x86_64  = { url = "https://ex/v2revision-1.2.0-x86_64.tar.gz",  sha256 = "cccc" },
                aarch64 = { url = "https://ex/v2revision-1.2.0-aarch64.tar.gz", sha256 = "dddd" },
                revision = 1,
            },
            ["1.3.0"] = {
                url      = "https://ex/v2revision-1.3.0.tar.gz",
                sha256   = "eeee",
                revision = "3",
            },
            ["1.4.0"] = {
                url      = "https://ex/v2revision-1.4.0.tar.gz",
                sha256   = "ffff",
                revision = -1,
            },
        },
    },
}
