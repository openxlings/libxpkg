-- Fixture: exports block with all sub-fields populated. Used by the
-- LoaderTest.LoadPackage_ExportsFull test to verify the full parse path.
package = {
    spec = "1",
    name = "exportsfull",
    description = "Fixture: exports.runtime with loader+libdirs+abi",
    licenses = {"MIT"},
    repo = "https://example.com/exportsfull",
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            deps = { runtime = { "scode:something@1.0" } },
            exports = {
                runtime = {
                    loader  = "lib64/ld-linux-x86-64.so.2",
                    libdirs = { "lib64", "lib", "usr/lib" },
                    abi     = "linux-x86_64-glibc",
                },
            },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {
                url    = "https://example.com/exportsfull-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
