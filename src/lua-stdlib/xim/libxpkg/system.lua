-- xim.libxpkg.system: system operations API
local M = {}

-- Run a command the way os.execute does. When the host asked for a hook log
-- (ExecutionContext.hook_log) os.execute sends the child's stdout/stderr to
-- that file; `tty` opts one command out, for the few that must reach the
-- terminal (a prompt, a password, output the user has to read). The executor
-- keeps the original os.execute under an internal name; where that name is
-- absent nothing redirects os.execute either, so falling back is the same call.
local function _execute(cmd, tty)
    if tty and _LIBXPKG_EXEC_INHERIT then return _LIBXPKG_EXEC_INHERIT(cmd) end
    return os.execute(cmd)
end

-- opt.retry : extra attempts after the first failure
-- opt.tty   : true = the command's output is not redirected into the hook log
function M.exec(cmd, opt)
    opt = opt or {}
    local retries = opt.retry or 0
    local attempts = retries + 1
    for i = 1, attempts do
        local ret = _execute(cmd, opt.tty)
        if ret == 0 or ret == true then return end
        if i == attempts then
            error("exec failed after " .. attempts .. " attempt(s): " .. tostring(cmd))
        end
    end
end

function M.rundir()           return _RUNTIME and _RUNTIME.run_dir or nil end
function M.xpkgdir()          return _RUNTIME and _RUNTIME.xpkg_dir or nil end
function M.bindir()           return _RUNTIME and _RUNTIME.bin_dir or nil end
function M.xpkg_args()        return (_RUNTIME and _RUNTIME.args) or {} end
function M.subos_sysrootdir() return _RUNTIME and _RUNTIME.subos_sysrootdir or nil end

-- admin = true runs the script under sudo, which prompts on the terminal, so
-- it is never redirected into the hook log.
function M.run_in_script(content, admin)
    local tmpfile = os.tmpname()
    -- write content to temp file
    if not io.writefile(tmpfile, content) then
        error("run_in_script: failed to write temp script")
    end
    local ok, err = pcall(function()
        os.execute("chmod +x " .. tmpfile)
        local prefix = (admin == true) and "sudo " or ""
        local ret = _execute(prefix .. tmpfile, admin == true)
        if ret ~= 0 and ret ~= true then
            error("script failed with code: " .. tostring(ret))
        end
    end)
    os.remove(tmpfile)  -- always cleanup
    if not ok then error(err) end
end

function M.unix_api()
    return {
        append_to_shell_profile = function(config)
            if not config then return end
            if type(config) == "string" then
                config = { posix = config, fish = config }
            end
            local profile_dir = _RUNTIME.run_dir or "/tmp"
            local posix = path.join(profile_dir, "xlings-profile.sh")
            local fish  = path.join(profile_dir, "xlings-profile.fish")
            if config.posix and os.isfile(posix) then
                local cur = io.readfile(posix) or ""
                if not cur:find(config.posix, 1, true) then
                    io.writefile(posix, cur .. "\n" .. config.posix)
                end
            end
            if config.fish and os.isfile(fish) then
                local cur = io.readfile(fish) or ""
                if not cur:find(config.fish, 1, true) then
                    io.writefile(fish, cur .. "\n" .. config.fish)
                end
            end
        end
    }
end

return M
