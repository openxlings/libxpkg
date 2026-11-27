-- xim.libxpkg.path: path strings (pure functions; nothing touches the disk)
--
-- One implementation: the global `path` of the compatibility layer is this
-- module (compat.lua), so both environments join and split paths alike.
local M = {}

-- The parts joined with "/", a separator already at the end of one dropped;
-- nil and empty parts after the first are skipped.
function M.join(...)
    local parts = {...}
    local sep = "/"
    local result = parts[1] or ""
    for i = 2, #parts do
        if parts[i] and parts[i] ~= "" then
            result = result:gsub("[/\\]+$", "") .. sep .. parts[i]
        end
    end
    return result
end

-- The last component ("a/b.tar.gz" -> "b.tar.gz"); "" when there is none.
function M.filename(p)
    return (p or ""):match("[^/\\]+$") or ""
end

-- Everything before the last component ("a/b/c" -> "a/b"); "" when there is none.
function M.directory(p)
    return (p or ""):match("^(.*)[/\\][^/\\]+$") or ""
end

-- "/..." or a drive letter ("C:\\...", "C:/...").
function M.is_absolute(p)
    return (p or ""):sub(1,1) == "/" or (p or ""):match("^%a:[/\\]") ~= nil
end

return M
