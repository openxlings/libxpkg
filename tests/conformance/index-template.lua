-- The index: index-compat.json declares a recipe template, read after every
-- recipe of pkgs/ as if written at its end; the file on disk is not changed.
return {
    chapter = "index",
    summary = "a declared template gives every recipe its hooks",
    package = "conf-template",
    files = {
        ["index-compat.json"] = [=[
{ "recipes": { "template": "template.lua" } }
]=],
        ["template.lua"] = [=[
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
function install()
    fs.write(path.join(pkginfo.install_dir(), "share", "templated"), pkginfo.name())
    return true
end
]=],
        ["pkgs/c/conf-template.lua"] = [=[
package = {
    spec = "3", name = "conf-template", description = "a recipe whose hooks are the index's template",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
]=],
    },
    loaded = { name = "conf-template", spec = "3" },
    hooks = { { "install", after = { { "share/templated", "file", text = "conf-template" } } } },
    install = "installed",
    payload = { { "share/templated", "file", text = "conf-template" } },
}
