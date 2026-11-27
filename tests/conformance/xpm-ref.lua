-- xpm: `ref` makes a key name another version; a version whose ref names
-- itself is that version (its payload made by its hooks), not a cycle.
return {
    chapter = "xpm",
    summary = "a ref alias resolves to the version it names; a self-ref is the version itself",
    package = "conf-ref",
    files = {
        ["pkgs/c/conf-ref.lua"] = [=[
package = {
    spec = "3", name = "conf-ref", description = "keys that name other versions",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = {
        linux = {
            ["latest"] = { ref = "1.1.0" },
            ["1.1.0"] = { url = "https://example.invalid/conf-ref-1.1.0.tar.gz",
                          sha256 = { x86_64 = "8888888888888888888888888888888888888888888888888888888888888888",
                                     aarch64 = "8888888888888888888888888888888888888888888888888888888888888888" } },
            ["2.0.0"] = { ref = "2.0.0" },
        },
    },
}
]=],
    },
    loaded = { name = "conf-ref" },
    resolve = {
        { platform = "linux", arch = "aarch64", version = "latest",
          url = "https://example.invalid/conf-ref-1.1.0.tar.gz",
          sha256 = "8888888888888888888888888888888888888888888888888888888888888888" },
        -- No download: the version is made by its hooks (a host installs it so),
        -- and no ref cycle.
        { platform = "linux", arch = "x86_64", version = "2.0.0", error = "neither url nor source" },
    },
}
