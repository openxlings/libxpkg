-- Fixture: exports.runtime.libdirs = {} -- a package that exports no library
-- directory (target content a host never loads), not the convention.
package = {
    spec = "1",
    name = "exportsnolibdirs",
    description = "Fixture: exports.runtime with an empty libdirs",
    licenses = {"MIT"},
    repo = "https://example.com/exportsnolibdirs",
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            exports = { runtime = { libdirs = {} } },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {
                url    = "https://example.com/exportsnolibdirs-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
        macosx = { inherits = "linux" },
    },
}
