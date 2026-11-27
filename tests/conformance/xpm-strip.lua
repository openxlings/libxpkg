-- xpm: `strip = n` drops the first n components of every member's path when
-- the host lands the download (spec "3"). An entry's own strip wins over its
-- platform's; none is 0.
return {
    chapter = "xpm",
    summary = "strip is read from the entry, else its platform, else 0",
    package = "conf-strip",
    files = {
        ["pkgs/c/conf-strip.lua"] = [=[
package = {
    spec = "3", name = "conf-strip", description = "the layout of a download, declared",
    type = "package", archs = { "x86_64" },
    xpm = {
        linux = {
            strip = 1,
            ["1.0.0"] = { url = "https://example.invalid/conf-strip-1.0.0.tar.gz",
                          sha256 = { x86_64 = "5555555555555555555555555555555555555555555555555555555555555555" } },
            ["2.0.0"] = { url = "https://example.invalid/conf-strip-2.0.0.tar.gz", strip = 2,
                          sha256 = { x86_64 = "6666666666666666666666666666666666666666666666666666666666666666" } },
        },
        windows = {
            ["1.0.0"] = { url = "https://example.invalid/conf-strip-1.0.0.zip",
                          sha256 = "7777777777777777777777777777777777777777777777777777777777777777" },
        },
    },
}
]=],
    },
    loaded = { name = "conf-strip", platforms = "linux,windows" },
    resolve = {
        { platform = "linux", arch = "x86_64", version = "1.0.0", strip = 1 },
        { platform = "linux", arch = "x86_64", version = "2.0.0", strip = 2 },
        { platform = "windows", arch = "x86_64", version = "1.0.0", strip = 0,
          url = "https://example.invalid/conf-strip-1.0.0.zip" },
    },
}
