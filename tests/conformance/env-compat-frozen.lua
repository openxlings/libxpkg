-- The hook environment of spec 1 and 2: the compatibility layer behaves as it
-- always did (a failed os.mv does not stop the hook) and each failed call is
-- reported with the hook's result.
return {
    chapter = "environment",
    summary = "spec 2's os.mv of a missing file returns and is reported; io.writefile and os.tryrm work",
    package = "conf-compat",
    files = {
        ["pkgs/c/conf-compat.lua"] = [=[
package = {
    spec = "2", name = "conf-compat", description = "the frozen compatibility layer",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
function install()
    local dir = pkginfo.install_dir()
    os.mv(path.join(dir, "missing"), path.join(dir, "moved"))
    os.mkdir(path.join(dir, "share"))
    io.writefile(path.join(dir, "share", "frozen"), "yes")
    os.tryrm(path.join(dir, "also-missing"))
    return true
end
]=],
    },
    loaded = { spec = "2" },
    hooks = { { "install", call_failures = 1, after = { { "share/frozen", "file", text = "yes" }, { "moved", "absent" } } } },
    install = "installed",
    payload = { { "share/frozen", "file", text = "yes" } },
}
