-- Versions: a recipe that states no spec is read as spec "1", in the
-- environment of spec 1 and 2 (the compatibility layer is there).
return {
    chapter = "versions",
    summary = "no spec reads as spec 1, with the compatibility layer",
    package = "conf-no-spec",
    files = {
        ["pkgs/c/conf-no-spec.lua"] = [=[
package = {
    name = "conf-no-spec", description = "a recipe with no spec",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
function install()
    os.mkdir(path.join(pkginfo.install_dir(), "share"))
    io.writefile(path.join(pkginfo.install_dir(), "share", "spec"), "1")
    return true
end
]=],
    },
    loaded = { name = "conf-no-spec", spec = "" },
    hooks = {
        { "install", after = { { "share/spec", "file", text = "1" } } },
    },
    install = "installed",
    payload = { { "share/spec", "file", text = "1" } },
}
