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

-- A command is one of two things (design 2026-11-27 §9.6):
--   { "tar", "-xf", archive, "-C", dir }  a program found on PATH and its
--       arguments, each passed as it is: no shell, nothing to quote. The
--       recommended form, the one system.admin_step takes.
--   "make -j4 2>&1 | tee build.log"       one shell line (sh -c, cmd /c), for
--       the pipes and redirects that need a shell.
-- opt.cwd     the directory it runs in (this process's is not changed)
-- opt.env     { NAME = "value" } set in its environment, on top of this one
-- opt.tty     its output reaches the terminal, not the hook log
-- opt.capture its stdout is returned (exec) instead of logged
-- The executor's process layer runs it (_LIBXPKG_SPAWN): status, code,
-- output, message.
local function _spawn(cmd, opt, capture)
    if _LIBXPKG_SPAWN then
        return _LIBXPKG_SPAWN(cmd, { cwd = opt.cwd, env = opt.env, tty = opt.tty, capture = capture })
    end
    -- No process layer bound (a state made without an executor): a line only.
    if type(cmd) ~= "string" or opt.cwd or opt.env or capture then
        return "refused", 0, "", "this state has no process layer for a list, cwd, env or capture"
    end
    local ok, how, code = _execute(cmd, opt.tty)
    if ok == true or ok == 0 then return "exit", 0, "" end
    return how == "signal" and "signal" or "exit", code or 1, ""
end

local function _describe(cmd)
    if type(cmd) ~= "table" then return tostring(cmd) end
    local words = {}
    for _, w in ipairs(cmd) do table.insert(words, tostring(w)) end
    return table.concat(words, " ")
end

local function _ended(status, code, why)
    if status == "exit" then return "exited with " .. tostring(code) end
    if status == "signal" then return "was killed by signal " .. tostring(code) end
    return why or "could not start"
end

-- An action: the command ran and succeeded, or this raises (at the caller's
-- line) with the command and how it ended.
-- opt.retry : extra attempts after the first failure
function M.exec(cmd, opt)
    opt = opt or {}
    local attempts = (opt.retry or 0) + 1
    for i = 1, attempts do
        local status, code, out, why = _spawn(cmd, opt, opt.capture == true)
        if status == "refused" then error("system.exec: " .. tostring(why), 2) end
        if status == "exit" and code == 0 then
            if opt.capture then return out end
            return
        end
        if i == attempts then
            error(string.format("system.exec: %s: %s%s", _describe(cmd), _ended(status, code, why),
                attempts > 1 and string.format(" (%d attempts)", attempts) or ""), 2)
        end
    end
end

-- A query: what does this command do? { ok, code, out } -- out is its stdout,
-- ok is a zero exit status. A command that fails, or cannot start (code 127,
-- with `error`), is an answer, never an error; a malformed command is.
function M.run(cmd, opt)
    opt = opt or {}
    local status, code, out, why = _spawn(cmd, opt, true)
    if status == "refused" then error("system.run: " .. tostring(why), 2) end
    if status == "failed" then return { ok = false, code = 127, out = "", error = why } end
    return { ok = status == "exit" and code == 0, code = code, out = out or "" }
end

-- A step only an administrator may take (chown root a sandbox helper, a
-- setuid bit), DECLARED, never run here: the host decides who is asked, when
-- and whether anyone can be (a recipe's own `sudo` waits on a terminal that
-- an unattended install does not have). `argv` is the command as a list,
-- `why` one line a person reads before saying yes. It reaches the host as a
-- deferred request: op "admin", target = why, then each word, separated by
-- the ASCII unit separator (0x1F).
function M.admin_step(argv, why)
    if type(argv) ~= "table" or #argv == 0 then error("system.admin_step: argv is a non-empty list") end
    local words = { tostring(why or "") }
    for _, w in ipairs(argv) do
        w = tostring(w)
        if w:find("\31", 1, true) then error("system.admin_step: a word holds the separator") end
        table.insert(words, w)
    end
    if not _INSTALL_REQUESTS then _INSTALL_REQUESTS = {} end
    table.insert(_INSTALL_REQUESTS, {op = "admin", target = table.concat(words, "\31")})
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
