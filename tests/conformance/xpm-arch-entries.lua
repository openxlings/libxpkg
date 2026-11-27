-- xpm: an entry names the architectures it ships. One artifact for one
-- architecture is `sha256 = { x86_64 = h }`; an architecture it does not list
-- is no artifact, not this one.
return {
    chapter = "xpm",
    summary = "a fixed URL with per-architecture checksums ships exactly the architectures listed",
    package = "conf-arch",
    files = {
        ["pkgs/c/conf-arch.lua"] = [=[
package = {
    spec = "3", name = "conf-arch", description = "entries that name their architectures",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/conf-arch-1.0.0-linux-x86_64.tar.gz",
                sha256 = { x86_64 = "1111111111111111111111111111111111111111111111111111111111111111" },
            },
            ["2.0.0"] = {
                url = "https://example.invalid/conf-arch-2.0.0.tar.gz",
                sha256 = {
                    x86_64 = "2222222222222222222222222222222222222222222222222222222222222222",
                    aarch64 = "2222222222222222222222222222222222222222222222222222222222222222",
                },
            },
            ["3.0.0"] = {
                url = "https://example.invalid/conf-arch-3.0.0-${arch}.tar.gz",
                sha256 = {
                    x86_64 = "3333333333333333333333333333333333333333333333333333333333333333",
                    aarch64 = "4444444444444444444444444444444444444444444444444444444444444444",
                },
            },
        },
    },
}
]=],
    },
    loaded = { name = "conf-arch", spec = "3", platforms = "linux" },
    resolve = {
        { platform = "linux", arch = "x86_64", version = "1.0.0",
          url = "https://example.invalid/conf-arch-1.0.0-linux-x86_64.tar.gz",
          sha256 = "1111111111111111111111111111111111111111111111111111111111111111" },
        { platform = "linux", arch = "aarch64", version = "1.0.0", error = "aarch64" },
        { platform = "linux", arch = "aarch64", version = "2.0.0",
          url = "https://example.invalid/conf-arch-2.0.0.tar.gz",
          sha256 = "2222222222222222222222222222222222222222222222222222222222222222" },
        { platform = "linux", arch = "aarch64", version = "3.0.0",
          url = "https://example.invalid/conf-arch-3.0.0-aarch64.tar.gz",
          sha256 = "4444444444444444444444444444444444444444444444444444444444444444" },
        { platform = "linux", arch = "riscv64", version = "3.0.0", error = "riscv64" },
    },
}
