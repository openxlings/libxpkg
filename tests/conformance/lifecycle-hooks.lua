-- Lifecycle: installed, install, config and uninstall, each on the install
-- directory, in the order a host runs them.
return {
    chapter = "lifecycle",
    summary = "the four hooks of spec 3 run on the install directory; installed() answers what is there",
    package = "conf-hooks",
    files = {
        ["pkgs/c/conf-hooks.lua"] = [=[
package = {
    spec = "3", name = "conf-hooks", description = "the hooks of spec 3",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
local function marker(name) return path.join(pkginfo.install_dir(), "share", name) end
function installed()
    if fs.is_file(marker("installed")) then return fs.read(marker("installed")) end
    return nil
end
function install()
    fs.write(marker("installed"), pkginfo.version())
    return true
end
function config()
    fs.write(marker("configured"), "yes")
    return true
end
function uninstall()
    fs.remove(marker("configured"))
    return true
end
]=],
    },
    loaded = { name = "conf-hooks", spec = "3" },
    hooks = {
        { "install", after = { { "share/installed", "file", text = "1.0.0" } } },
        { "config", after = { { "share/configured", "file", text = "yes" } } },
        { "uninstall", after = { { "share/configured", "absent" }, { "share/installed", "file" } } },
    },
    install = "installed",
    payload = { { "share/installed", "file", text = "1.0.0" }, { "share/configured", "file", text = "yes" } },
}
