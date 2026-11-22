-- xim.libxpkg.utils: utility functions
local M = {}

local function _get_log()
    return _LIBXPKG_MODULES and _LIBXPKG_MODULES["log"]
end

function M.filepath_to_absolute(filepath)
    if path.is_absolute(filepath) then return filepath end
    return path.join(os.getenv("PWD") or ".", filepath)
end

function M.try_download_and_check(url, dir, sha256)
    local log = _get_log()
    local filename = url:match("[^/]+$") or "download"
    local dest = path.join(dir, filename)
    local ret = os.execute(string.format('curl -fsSL -o "%s" "%s"', dest, url))
    if ret ~= 0 and ret ~= true then
        if log then log.error("download failed: %s", url) end
        return false
    end
    if sha256 then
        local out = os.iorun(string.format('sha256sum "%s"', dest)) or ""
        local actual = out:match("^(%x+)")
        if actual ~= sha256 then
            if log then log.error("sha256 mismatch for %s", dest) end
            return false
        end
    end
    return true
end

function M.input_args_process(cmds_kv, args)
    local result = {}
    local i = 1
    local arglist = args or {}
    while i <= #arglist do
        local arg = arglist[i]
        -- --key=value format
        local k, v = arg:match("^(%-%-[%w%-]+)=(.+)$")
        if k and cmds_kv[k] ~= nil then
            result[k] = v
            i = i + 1
        elseif arg:match("^%-%-") and cmds_kv[arg] ~= nil then
            -- --key value format
            if i < #arglist then
                result[arg] = arglist[i + 1]
                i = i + 2
            else
                result[arg] = true
                i = i + 1
            end
        else
            i = i + 1
        end
    end
    return true, result
end

return M
