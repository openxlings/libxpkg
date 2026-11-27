-- Lifecycle: a hook that returns false fails, and so does the install.
return {
    chapter = "lifecycle",
    summary = "install() returning false fails the step",
    package = "conf-false",
    files = {
        ["pkgs/c/conf-false.lua"] = [=[
package = {
    spec = "3", name = "conf-false", description = "an install that says it failed",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
function install() return false end
]=],
    },
    loaded = { name = "conf-false" },
    hooks = { { "install", ok = false } },
    install = "failed",
}
