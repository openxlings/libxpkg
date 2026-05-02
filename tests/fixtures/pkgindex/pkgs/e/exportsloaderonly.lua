-- Fixture: exports.runtime with only `loader` declared (libdirs and abi
-- omitted — testing graceful default for partial declarations).
package = {
    spec = "1",
    name = "exportsloaderonly",
    description = "Fixture: exports.runtime with only loader",
    licenses = {"MIT"},
    repo = "https://example.com/exportsloaderonly",
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            deps = { runtime = {} },
            exports = {
                runtime = {
                    loader = "lib/ld-musl-x86_64.so.1",
                    -- libdirs / abi intentionally omitted
                },
            },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {
                url    = "https://example.com/exportsloaderonly-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
