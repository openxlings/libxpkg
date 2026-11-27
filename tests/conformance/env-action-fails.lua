-- The hook environment: an action that cannot make its statement true raises;
-- the hook fails with the action and its path, and so does the install.
return {
    chapter = "environment",
    summary = "fs.move onto a path that exists raises without { replace = true }",
    package = "conf-action-fails",
    files = {
        ["pkgs/c/conf-action-fails.lua"] = [=[
package = {
    spec = "3", name = "conf-action-fails", description = "an action that cannot be done",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
function install()
    local dir = pkginfo.install_dir()
    fs.write(path.join(dir, "a"), "a")
    fs.write(path.join(dir, "b"), "b")
    fs.move(path.join(dir, "a"), path.join(dir, "b"))
    return true
end
]=],
    },
    loaded = { spec = "3" },
    hooks = { { "install", ok = false, error = "fs.move" } },
    install = "failed",
}
