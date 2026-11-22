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

// How a command ended, in Lua 5.4's os.execute vocabulary.
struct Status {
    enum class Kind { Exit, Signal, Failed };
    Kind kind = Kind::Failed;
    int code = 0;          // exit status, signal number, or the OS error
    std::string message;   // Failed only
};

struct Run {
    Status status;
    std::string output;    // what the streams set to Capture wrote
};

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

} // namespace

Run run(std::string_view line, const Streams& streams) {
    Run result;
    Status& status = result.status;

    // `"<COMSPEC>" /d /s /c "<line>"`. /s: cmd removes the first and the last
    // quote of what follows /c and nothing else, which is the wrapper added
    // here, so the line reaches cmd's parser exactly as the caller wrote it.
    // /d: no AutoRun command from the registry runs in front of it. The narrow
    // API is what system() used; a host that runs with the UTF-8 code page
    // (xlings' manifest) passes UTF-8 through it unchanged.
    const char* env = std::getenv("COMSPEC");
    const bool fromEnv = env && *env;
    const std::string comspec = fromEnv ? env : "cmd.exe";
    std::string commandLine = "\"" + comspec + "\" /d /s /c \"";
    commandLine.append(line);
    commandLine += "\"";

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

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    if (streams.out != Sink::Inherit || streams.err != Sink::Inherit) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = handleFor(streams.out, STD_OUTPUT_HANDLE);
        startup.hStdError = handleFor(streams.err, STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    // With COMSPEC unset, cmd.exe is found on the search path; a bare name as
    // lpApplicationName would only be looked for in the current directory, so
    // CreateProcess parses the command line instead.
    const BOOL started = ::CreateProcessA(fromEnv ? comspec.c_str() : nullptr,
                                          commandLine.data(),
                                          nullptr, nullptr, TRUE, 0, nullptr, nullptr,
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
        status.message = "cannot start " + comspec + " (error " +
                         std::to_string(status.code) + ")";
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

} // namespace

Run run(std::string_view line, const Streams& streams) {
    Run result;
    Status& status = result.status;
    const std::string cmd(line);

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
    pid_t pid = 0;
    if (error == 0) {
        char* const argv[] = { const_cast<char*>("sh"), const_cast<char*>("-c"),
                               const_cast<char*>(cmd.c_str()), nullptr };
#  if defined(__APPLE__)
        char** const envp = *_NSGetEnviron();
#  else
        char** const envp = environ;
#  endif
        error = ::posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, envp);
    }
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
