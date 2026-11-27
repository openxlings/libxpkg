-- The index: a recipe imports a lib of its index (xim.pkgindex.<name>); the
-- lib runs in an environment of the recipe's spec.
return {
    chapter = "index",
    summary = "a lib of the index is imported by name",
    package = "conf-libs",
    files = {
        ["libs/conflib.lua"] = [=[
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
local M = {}
function M.stamp(dir, text) fs.write(path.join(dir, "share", "stamp"), text) end
return M
]=],
        ["pkgs/c/conf-libs.lua"] = [=[
package = {
    spec = "3", name = "conf-libs", description = "a recipe using a lib of its index",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.pkgindex.conflib")
function install()
    conflib.stamp(pkginfo.install_dir(), "from the lib")
    return true
end
]=],
    },
    loaded = { spec = "3" },
    hooks = { { "install", after = { { "share/stamp", "file", text = "from the lib" } } } },
    install = "installed",
    payload = { { "share/stamp", "file", text = "from the lib" } },
}
