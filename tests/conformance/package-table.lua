-- The package table: its fields as the loader reads them.
return {
    chapter = "package",
    summary = "the fields of the package table, as read",
    package = "conf-table",
    files = {
        ["pkgs/c/conf-table.lua"] = [=[
package = {
    spec = "3", name = "conf-table", description = "the fields of a package table",
    type = "package", status = "stable",
    archs = { "x86_64", "aarch64" }, programs = { "conf-table" },
    licenses = { "Apache-2.0" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    fs.write(path.join(pkginfo.install_dir(), "share", "name"), pkginfo.name() .. "@" .. pkginfo.version())
    fs.write(path.join(pkginfo.install_dir(), "bin", "conf-table"), "#!/bin/sh\necho conf-table\n")
    fs.chmod(path.join(pkginfo.install_dir(), "bin", "conf-table"), "755")
    return true
end
-- A package that lists programs registers them; a host fails an install
-- that registers none of them.
function config()
    xvm.add("conf-table", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall()
    xvm.remove("conf-table")
    return true
end
]=],
    },
    loaded = {
        name = "conf-table", spec = "3", description = "the fields of a package table",
        type = "package", status = "stable", archs = "x86_64,aarch64", programs = "conf-table",
        platforms = "linux,macosx,windows",
    },
    hooks = {
        { "install", after = { { "share/name", "file", text = "conf-table@1.0.0" }, { "bin/conf-table", "file" } } },
        { "config" },
    },
    install = "installed",
    payload = { { "share/name", "file", text = "conf-table@1.0.0" }, { "bin/conf-table", "file" } },
}
