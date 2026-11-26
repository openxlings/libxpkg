-- xim.libxpkg.log: logging API for xpkg scripts
local M = {}

local PREFIX = "[xim:xpkg]: "

-- Log levels: 0=debug, 1=info, 2=warn, 3=error, 4=silent
local LEVEL_DEBUG = 0
local LEVEL_INFO  = 1
local LEVEL_WARN  = 2
local LEVEL_ERROR = 3

local _level = LEVEL_INFO  -- default: show info and above

local function _format(text, ...)
    local ok, msg = pcall(string.format, text, ...)
    msg = ok and msg or tostring(text)
    return (msg:gsub("%${%w+}", ""))
end

local function _write(msg)
    io.write(PREFIX .. msg .. "\n")
    io.flush()
end

-- warn and error are what a recipe says to the person installing it (design
-- 2026-11-27 §9.2): besides the line in the hook's output, each one is kept,
-- in order, for the host to show after its summary (HookResult::notices),
-- whatever the level set for the output.
local function _notice(level, msg)
    local notices = _LIBXPKG_NOTICES
    if type(notices) == "table" then
        table.insert(notices, { level = level, text = msg })
    end
end

function M.debug(text, ...)
    if not text then return end
    if _level <= LEVEL_DEBUG then _write(_format(text, ...)) end
end

function M.info(text, ...)
    if not text then return end
    if _level <= LEVEL_INFO then _write(_format(text, ...)) end
end

function M.warn(text, ...)
    if not text then return end
    local msg = _format(text, ...)
    _notice("warn", msg)
    if _level <= LEVEL_WARN then _write("[WARN] " .. msg) end
end

function M.error(text, ...)
    if not text then return end
    local msg = _format(text, ...)
    _notice("error", msg)
    if _level <= LEVEL_ERROR then _write("[ERROR] " .. msg) end
end

-- Set log level: "debug", "info", "warn", "error", "silent"
function M.set_level(level)
    if level == "debug" or level == 0 then _level = LEVEL_DEBUG
    elseif level == "info" or level == 1 then _level = LEVEL_INFO
    elseif level == "warn" or level == 2 then _level = LEVEL_WARN
    elseif level == "error" or level == 3 then _level = LEVEL_ERROR
    elseif level == "silent" or level == 4 then _level = 4
    end
end

function M.get_level()
    return _level
end

return M
