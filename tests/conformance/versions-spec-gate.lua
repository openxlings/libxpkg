-- Versions: a spec higher than the reader's is refused for that package alone;
-- its table still reads, so search and the index still list it.
return {
    chapter = "versions",
    summary = "a recipe of a spec newer than the reader is read, and refused before any hook runs",
    package = "conf-spec-gate",
    files = {
        ["pkgs/c/conf-spec-gate.lua"] = [=[
package = {
    spec = "4", name = "conf-spec-gate", description = "a recipe of a later spec",
    type = "package", archs = { "x86_64", "aarch64" },
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
function install() return true end
]=],
    },
    loaded = { name = "conf-spec-gate", spec = "4" },
    executor_error = "spec",
    install = "refused",
}
