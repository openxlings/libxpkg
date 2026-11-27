-- The hook environment: a recipe of spec "3" has none of the compatibility
-- layer; calling one of its names is an error, and the install fails.
return {
    chapter = "environment",
    summary = "os.mv is not in the environment of spec 3",
    package = "conf-no-compat",
    files = {
        ["pkgs/c/conf-no-compat.lua"] = [=[
package = {
    spec = "3", name = "conf-no-compat", description = "a name of the compatibility layer in spec 3",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
function install()
    os.mv(pkginfo.install_dir(), pkginfo.install_dir() .. ".old")
    return true
end
]=],
    },
    loaded = { spec = "3" },
    hooks = { { "install", ok = false, error = "mv" } },
    install = "failed",
}
