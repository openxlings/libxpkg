module;

// Platform headers for the process layer. `import std;` does not provide them,
// and the named-module purview forbids including them there.
#if defined(_WIN32)
// windows.h's min/max macros break std::min({...}) in every module that sees
// them, and only Windows ever reports it. Both defines come before the include.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <crt_externs.h>
#  else
// POSIX leaves this declaration to the application; glibc/musl only declare
// it under _GNU_SOURCE.
extern "C" char** environ;
#  endif
#endif

// One command line through the system shell (libxpkg#52).
//
// Every place libxpkg hands a recipe's or its own command line to a shell
// calls run() below: os.execute, os.exec, os.iorun, the hook log's
// redirection, the index build's checkout. Two defects had one cause, a
// command line assembled by string concatenation before cmd.exe saw it:
//
//   * cmd /c without /s strips the first and the last quote of a line that
//     starts with a quote and holds more than one pair, so
//     `"C:\a b\tool.exe" --version` ran as `C:\a b\tool.exe" --version`;
//   * os.iorun appended ` 2>nul > "<tmp>"` to the caller's line, and the
//     stripping above moved that redirect inside a quoted argument, so the
//     output was never written and iorun returned "".
//
// Here the line is wrapped once and /s makes cmd strip exactly that wrapper,
// so the line runs as written, quoted program path or not; and output is read
// through a pipe set up in the process layer, so nothing is appended to the
// caller's line and a redirect the caller wrote keeps its meaning.
export module mcpplibs.xpkg.shell;
import mcpplibs.capi.lua;
import std;

namespace lua = mcpplibs::capi::lua;

export namespace mcpplibs::xpkg::shell {

#if defined(_WIN32)
using NativeFile = void*;  // HANDLE
inline NativeFile const kNoFile = reinterpret_cast<void*>(static_cast<std::intptr_t>(-1));
#else
using NativeFile = int;
inline constexpr NativeFile kNoFile = -1;
#endif

// Where one of the child's output streams goes.
enum class Sink {
    Inherit,  // this process's own stream
    Null,     // the null device
    File,     // Streams::file (the null device when it is kNoFile)
    Capture,  // a pipe this process reads into Run::output
};

struct Streams {
    Sink out = Sink::Inherit;
    Sink err = Sink::Inherit;
    NativeFile file = kNoFile;
};

// How a command ended, in Lua 5.4's os.execute vocabulary, plus one case of
// its own: a command this layer will not start as written (Refused).
struct Status {
    enum class Kind { Exit, Signal, Failed, Refused };
    Kind kind = Kind::Failed;
    int code = 0;          // exit status, signal number, or the OS error
    std::string message;   // Failed and Refused only
};

struct Run {
    Status status;
    std::string output;    // what the streams set to Capture wrote
};

// What to start. A program and its arguments (`argv`) are passed as they are,
// with no shell between: no quoting, no word splitting, no expansion. The
// program is found on PATH (the child's, so an `env` that sets PATH decides)
// unless it is a path; on Windows also with each extension of PATHEXT, and
// the current directory is not searched. A batch file is Refused there: cmd
// parses a batch file's arguments again, and no quoting is safe for both
// parsers. Without `argv`, `line` is one shell line (run() below).
struct Command {
    std::vector<std::string> argv;
    std::string line;
    std::filesystem::path cwd;  // empty: this process's
    // Set in the child's environment on top of this process's (a name already
    // there is replaced; on Windows names compare without case).
    std::vector<std::pair<std::string, std::string>> env;
};

// Start a command and wait for it. stdin is inherited.
Run spawn(const Command& command, const Streams& streams = {});

// The command line Windows' C runtime (CommandLineToArgvW) splits back into
// exactly `argv`: an argument with a space, a tab or a quote, or an empty one,
// is quoted, and backslashes are doubled only before a quote. Plain text on
// every platform; spawn() uses it on Windows.
std::string windows_command_line(const std::vector<std::string>& argv);

// Run `line` through the system shell and wait for it: `cmd /d /s /c "<line>"`
// on Windows (the shell COMSPEC names, else cmd.exe), `/bin/sh -c <line>`
// elsewhere. stdin is inherited.
Run run(std::string_view line, const Streams& streams = {});

// Lua bindings, the same function behind each.
//
// os.execute([cmd]): Lua 5.4's results -- true|nil, "exit"|"signal", code;
// with no command, whether a shell exists (always true).
int lua_execute(lua::State* L);
// os.iorun(cmd) -> string: the command's stdout; its stderr is discarded.
int lua_iorun(lua::State* L);
// Push the three results of os.execute for `status`.
void push_status(lua::State* L, const Status& status);

} // export namespace mcpplibs::xpkg::shell

namespace mcpplibs::xpkg::shell {

#if defined(_WIN32)

namespace {

HANDLE open_null(SECURITY_ATTRIBUTES& sa) {
    return ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              wide.data(), size);
    return wide;
}

std::wstring lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

std::wstring variable(const wchar_t* name) {
    const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(name, value.data(), size);
    value.resize(got);
    return value;
}

std::vector<std::wstring> split(const std::wstring& list, wchar_t separator) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = list.find(separator, start);
        auto part = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!part.empty()) parts.push_back(std::move(part));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

bool is_file(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The program `name` names: itself when it is a path, else the first match on
// PATH; with each PATHEXT extension when it has none. Empty when none exists.
std::wstring find_program(const std::wstring& name, const std::wstring& path) {
    std::wstring pathext = variable(L"PATHEXT");
    if (pathext.empty()) pathext = L".COM;.EXE;.BAT;.CMD";
    const auto extensions = split(pathext, L';');
    const bool has_extension = std::filesystem::path(name).has_extension();
    auto match = [&](const std::wstring& base) -> std::wstring {
        if (is_file(base)) return base;
        if (!has_extension)
            for (const auto& ext : extensions)
                if (is_file(base + ext)) return base + ext;
        return {};
    };
    const bool is_path = name.find_first_of(L"\\/") != std::wstring::npos
                      || (name.size() >= 2 && name[1] == L':');
    if (is_path) return match(name);
    for (const auto& dir : split(path, L';')) {
        std::wstring base = dir;
        if (base.back() != L'\\' && base.back() != L'/') base += L'\\';
        if (auto found = match(base + name); !found.empty()) return found;
    }
    return {};
}

// This process's environment with `overrides` applied, as the sorted,
// double-NUL-terminated block CreateProcessW takes.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (wchar_t* block = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
            std::wstring entry(p);
            // "=C:=C:\dir" entries keep their leading '='.
            const auto eq = entry.find(L'=', 1);
            entries.emplace_back(entry.substr(0, eq),
                                 eq == std::wstring::npos ? std::wstring{} : entry.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : overrides) {
        const auto wide = widen(name);
        std::erase_if(entries, [&](const auto& e) { return lower(e.first) == lower(wide); });
        entries.emplace_back(wide, widen(value));
    }
    std::ranges::sort(entries, [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
    std::wstring out;
    for (const auto& [name, value] : entries) {
        out += name;
        out += L'=';
        out += value;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

} // namespace

Run spawn(const Command& command, const Streams& streams) {
    Run result;
    Status& status = result.status;

    std::wstring application;
    std::wstring commandLine;
    std::string shown;  // what an error message calls the program
    if (!command.argv.empty()) {
        std::wstring path;
        bool path_overridden = false;
        for (const auto& [name, value] : command.env) {
            if (lower(widen(name)) == L"path") { path = widen(value); path_overridden = true; }
        }
        if (!path_overridden) path = variable(L"PATH");
        shown = command.argv.front();
        application = find_program(widen(command.argv.front()), path);
        if (application.empty()) {
            status.kind = Status::Kind::Failed;
            status.code = ERROR_FILE_NOT_FOUND;
            status.message = "no program named " + shown + " on PATH";
            return result;
        }
        const auto ext = lower(std::filesystem::path(application).extension().wstring());
        if (ext == L".bat" || ext == L".cmd") {
            status.kind = Status::Kind::Refused;
            status.message = shown + " is a batch file, and cmd parses a batch file's arguments again: "
                             "pass the command as one string instead of a list";
            return result;
        }
        commandLine = widen(windows_command_line(command.argv));
    } else {
        // `"<COMSPEC>" /d /s /c "<line>"`. /s: cmd removes the first and the
        // last quote of what follows /c and nothing else, which is the wrapper
        // added here, so the line reaches cmd's parser exactly as the caller
        // wrote it. /d: no AutoRun command from the registry runs in front of
        // it. With COMSPEC unset, cmd.exe is found on the search path; a bare
        // name as lpApplicationName would only be looked for in the current
        // directory, so CreateProcess parses the command line instead.
        const std::wstring comspec = variable(L"COMSPEC");
        shown = comspec.empty() ? "cmd.exe" : "COMSPEC";
        if (!comspec.empty()) application = comspec;
        commandLine = L"\"" + (comspec.empty() ? std::wstring(L"cmd.exe") : comspec) + L"\" /d /s /c \"";
        commandLine += widen(command.line);
        commandLine += L"\"";
    }

    std::wstring cwd;
    if (!command.cwd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(command.cwd, ec)) {
            status.kind = Status::Kind::Failed;
            status.code = ERROR_DIRECTORY;
            status.message = "the working directory " + command.cwd.string() + " is not a directory";
            return result;
        }
        cwd = command.cwd.wstring();
    }
    std::wstring environment;
    if (!command.env.empty()) environment = environment_block(command.env);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = INVALID_HANDLE_VALUE;
    HANDLE writeEnd = INVALID_HANDLE_VALUE;
    const bool capture = streams.out == Sink::Capture || streams.err == Sink::Capture;
    if (capture) {
        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            status.code = static_cast<int>(::GetLastError());
            status.message = "cannot create a pipe (error " + std::to_string(status.code) + ")";
            return result;
        }
        // Only the child's end is inherited; the child holding the read end
        // too would keep the pipe open after it exits.
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE nul = INVALID_HANDLE_VALUE;
    auto nullDevice = [&]() {
        if (nul == INVALID_HANDLE_VALUE) nul = open_null(sa);
        return nul;
    };
    auto handleFor = [&](Sink sink, DWORD inherited) -> HANDLE {
        switch (sink) {
            case Sink::Inherit: return ::GetStdHandle(inherited);
            case Sink::Null:    return nullDevice();
            case Sink::File:
                return streams.file == kNoFile ? nullDevice() : static_cast<HANDLE>(streams.file);
            case Sink::Capture: return writeEnd;
        }
        return nullDevice();
    };

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (streams.out != Sink::Inherit || streams.err != Sink::Inherit) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = handleFor(streams.out, STD_OUTPUT_HANDLE);
        startup.hStdError = handleFor(streams.err, STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(application.empty() ? nullptr : application.c_str(),
                                          commandLine.data(), nullptr, nullptr, TRUE,
                                          environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
                                          environment.empty() ? nullptr : environment.data(),
                                          cwd.empty() ? nullptr : cwd.c_str(),
                                          &startup, &process);
    // The child has its copy; with the parent's closed, the read below ends
    // when the child (and anything it started holding the handle) is done.
    if (writeEnd != INVALID_HANDLE_VALUE) ::CloseHandle(writeEnd);
    if (started) {
        ::CloseHandle(process.hThread);
        if (readEnd != INVALID_HANDLE_VALUE) {
            char buffer[4096];
            DWORD got = 0;
            while (::ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
                result.output.append(buffer, got);
            }
        }
        ::WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        ::GetExitCodeProcess(process.hProcess, &exitCode);
        ::CloseHandle(process.hProcess);
        status.kind = Status::Kind::Exit;
        status.code = static_cast<int>(exitCode);
    } else {
        status.code = static_cast<int>(::GetLastError());
        status.message = "cannot start " + shown + " (error " + std::to_string(status.code) + ")";
    }
    if (readEnd != INVALID_HANDLE_VALUE) ::CloseHandle(readEnd);
    if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
    return result;
}

#else

namespace {

int route(posix_spawn_file_actions_t& actions, Sink sink, int fd,
          NativeFile file, int writeEnd) {
    switch (sink) {
        case Sink::Inherit: return 0;
        case Sink::Null:
            return ::posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_WRONLY, 0);
        case Sink::File:
            if (file == kNoFile) {
                return ::posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_WRONLY, 0);
            }
            return ::posix_spawn_file_actions_adddup2(&actions, file, fd);
        case Sink::Capture:
            return ::posix_spawn_file_actions_adddup2(&actions, writeEnd, fd);
    }
    return 0;
}

int make_pipe(int fds[2]) {
    if (::pipe(fds) != 0) return errno;
    // Close-on-exec on both ends: dup2 onto fd 1/2 clears the flag on the copy
    // the child uses, and nothing else the child starts holds the pipe open.
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}


char** parent_environment() {
#  if defined(__APPLE__)
    return *_NSGetEnviron();
#  else
    return environ;
#  endif
}

// The first executable regular file named `name` in the ':'-separated `path`
// (an empty entry is the current directory, as for execvp), or `name` itself
// when it holds a '/'. Empty when none exists.
std::string find_program(const std::string& name, std::string_view path) {
    if (name.find('/') != std::string::npos) return name;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        std::string dir(path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        std::string candidate = (dir.empty() ? std::string(".") : dir) + "/" + name;
        std::error_code ec;
        if (::access(candidate.c_str(), X_OK) == 0 && std::filesystem::is_regular_file(candidate, ec))
            return candidate;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

} // namespace

Run spawn(const Command& command, const Streams& streams) {
    Run result;
    Status& status = result.status;

    std::vector<std::string> words;
    std::string program;
    if (!command.argv.empty()) {
        std::string path;
        bool path_overridden = false;
        for (const auto& [name, value] : command.env)
            if (name == "PATH") { path = value; path_overridden = true; }
        if (!path_overridden) {
            const char* p = std::getenv("PATH");
            path = p ? p : "/usr/bin:/bin";
        }
        program = find_program(command.argv.front(), path);
        if (program.empty()) {
            status.kind = Status::Kind::Failed;
            status.code = ENOENT;
            status.message = "no program named " + command.argv.front() + " on PATH";
            return result;
        }
        words = command.argv;
    } else {
        program = "/bin/sh";
        words = { "sh", "-c", command.line };
    }
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    std::vector<char*> envp;
    char** child_env = parent_environment();
    if (!command.env.empty()) {
        for (char** e = child_env; e && *e; ++e) {
            const std::string_view entry(*e);
            const auto name = entry.substr(0, entry.find('='));
            const bool replaced = std::ranges::any_of(command.env, [&](const auto& kv) { return kv.first == name; });
            if (!replaced) environment.emplace_back(entry);
        }
        for (const auto& [name, value] : command.env) environment.push_back(name + "=" + value);
        for (auto& entry : environment) envp.push_back(entry.data());
        envp.push_back(nullptr);
        child_env = envp.data();
    }

    if (!command.cwd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(command.cwd, ec)) {
            status.kind = Status::Kind::Failed;
            status.code = ENOTDIR;
            status.message = "the working directory " + command.cwd.string() + " is not a directory";
            return result;
        }
    }

    int fds[2] = { -1, -1 };
    const bool capture = streams.out == Sink::Capture || streams.err == Sink::Capture;
    int error = capture ? make_pipe(fds) : 0;

    posix_spawn_file_actions_t actions;
    bool actionsMade = false;
    if (error == 0) {
        error = ::posix_spawn_file_actions_init(&actions);
        actionsMade = error == 0;
    }
    if (error == 0) error = route(actions, streams.out, 1, streams.file, fds[1]);
    if (error == 0) error = route(actions, streams.err, 2, streams.file, fds[1]);
    if (error == 0 && !command.cwd.empty())
        error = ::posix_spawn_file_actions_addchdir_np(&actions, command.cwd.c_str());
    pid_t pid = 0;
    if (error == 0) error = ::posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), child_env);
    if (actionsMade) ::posix_spawn_file_actions_destroy(&actions);
    if (fds[1] >= 0) ::close(fds[1]);

    if (error == 0 && fds[0] >= 0) {
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
            if (got > 0) {
                result.output.append(buffer, static_cast<std::size_t>(got));
            } else if (got < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
    if (fds[0] >= 0) ::close(fds[0]);

    if (error == 0) {
        int raw = 0;
        pid_t reaped = 0;
        do {
            reaped = ::waitpid(pid, &raw, 0);
        } while (reaped < 0 && errno == EINTR);
        if (reaped < 0) {
            error = errno;
        } else if (WIFEXITED(raw)) {
            status.kind = Status::Kind::Exit;
            status.code = WEXITSTATUS(raw);
        } else if (WIFSIGNALED(raw)) {
            status.kind = Status::Kind::Signal;
            status.code = WTERMSIG(raw);
        } else {
            status.kind = Status::Kind::Exit;
            status.code = raw;
        }
    }
    if (status.kind == Status::Kind::Failed) {
        status.code = error;
        status.message = std::strerror(error);
    }
    return result;
}

#endif

Run run(std::string_view line, const Streams& streams) {
    return spawn(Command{ .line = std::string(line) }, streams);
}

std::string windows_command_line(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty()) out += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            out += arg;
            continue;
        }
        out += '"';
        for (auto it = arg.begin();; ++it) {
            std::size_t backslashes = 0;
            while (it != arg.end() && *it == '\\') {
                ++it;
                ++backslashes;
            }
            if (it == arg.end()) {
                out.append(backslashes * 2, '\\');
                break;
            }
            if (*it == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out += *it;
            } else {
                out.append(backslashes, '\\');
                out += *it;
            }
        }
        out += '"';
    }
    return out;
}

void push_status(lua::State* L, const Status& status) {
    switch (status.kind) {
        case Status::Kind::Exit:
            // luaL_execresult: success is true, any other status is fail.
            if (status.code == 0) lua::pushboolean(L, 1);
            else lua::pushnil(L);
            lua::pushstring(L, "exit");
            break;
        case Status::Kind::Signal:
            lua::pushnil(L);
            lua::pushstring(L, "signal");
            break;
        case Status::Kind::Failed:
        case Status::Kind::Refused:
            // luaL_fileresult: fail, message, errno.
            lua::pushnil(L);
            lua::pushstring(L, status.message.c_str());
            break;
    }
    lua::pushinteger(L, status.code);
}

int lua_execute(lua::State* L) {
    unsigned long long length = 0;
    // Nothing with a destructor may be live across this call: a bad argument
    // raises through it, exactly as it does in Lua's own os.execute.
    const char* cmd = lua::L_optlstring(L, 1, nullptr, &length);
    if (!cmd) {
        lua::pushboolean(L, 1);
        return 1;
    }
    const Status status = run(std::string_view(cmd, length)).status;
    push_status(L, status);
    return 3;
}

int lua_iorun(lua::State* L) {
    unsigned long long length = 0;
    const char* cmd = lua::tolstring(L, 1, &length);
    if (!cmd) {
        lua::pushstring(L, "");
        return 1;
    }
    const Run ran = run(std::string_view(cmd, length), { Sink::Capture, Sink::Null });
    lua::pushlstring(L, ran.output.data(), ran.output.size());
    return 1;
}

} // namespace mcpplibs::xpkg::shell
