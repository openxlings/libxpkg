-- The hook environment: an action's statement holds after it returns -- the
-- directory is there, the file holds the text, the source of a move is gone.
-- Tolerating a failure is written with pcall.
return {
    chapter = "environment",
    summary = "fs actions make what they state true; pcall is how a failure is tolerated",
    package = "conf-actions",
    files = {
        ["pkgs/c/conf-actions.lua"] = [=[
package = {
    spec = "3", name = "conf-actions", description = "the fs actions",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
function install()
    local dir = pkginfo.install_dir()
    fs.mkdir(path.join(dir, "bin"))
    fs.mkdir(path.join(dir, "bin"))                       -- already a directory: done
    fs.write(path.join(dir, "tmp", "tool"), "tool")
    fs.move(path.join(dir, "tmp", "tool"), path.join(dir, "bin", "conf-actions"))
    fs.chmod(path.join(dir, "bin", "conf-actions"), "+x")
    fs.copy(path.join(dir, "bin"), path.join(dir, "copy"))
    fs.remove(path.join(dir, "tmp"))
    fs.remove(path.join(dir, "never-there"))              -- not there: done
    fs.write(path.join(dir, "taken"), "first")
    local ok = pcall(fs.move, path.join(dir, "copy", "conf-actions"), path.join(dir, "taken"))
    fs.write(path.join(dir, "share", "tolerated"), ok and "moved" or "kept")
    local names = {}
    for _, e in ipairs(fs.list(dir)) do names[#names + 1] = e.name .. ":" .. e.type end
    fs.write(path.join(dir, "share", "list"), table.concat(names, " "))
    return true
end
]=],
    },
    loaded = { spec = "3" },
    hooks = {
        { "install", after = {
            { "bin/conf-actions", "file", text = "tool" },
            { "copy/conf-actions", "file", text = "tool" },
            { "tmp", "absent" },
            { "taken", "file", text = "first" },
            { "share/tolerated", "file", text = "kept" },
            { "share/list", "file", text = "bin:dir copy:dir share:dir taken:file" },
        } },
    },
    install = "installed",
    payload = {
        { "bin/conf-actions", "file", text = "tool" },
        { "tmp", "absent" },
        { "share/tolerated", "file", text = "kept" },
    },
}
