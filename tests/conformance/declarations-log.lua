-- Declarations: log.warn and log.error are notices to the user, handed to the
-- host with the hook's result; log.info is progress and is not.
return {
    chapter = "declarations",
    summary = "log.warn becomes a notice; log.info does not",
    package = "conf-log",
    files = {
        ["pkgs/c/conf-log.lua"] = [=[
package = {
    spec = "3", name = "conf-log", description = "what a recipe tells the user",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.log")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
function install()
    log.info("landing %s", pkginfo.name())
    log.warn("restart the shell for %s", pkginfo.name())
    fs.write(path.join(pkginfo.install_dir(), "share", "said"), "yes")
    return true
end
]=],
    },
    loaded = { spec = "3" },
    hooks = { { "install", notices = { { "warn", "restart the shell for conf-log" } } } },
    install = "installed",
    payload = { { "share/said", "file", text = "yes" } },
}
