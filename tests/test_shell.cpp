// One command line through the shell (libxpkg#52): the function every
// os.execute, os.exec and os.iorun call reaches. Runs on Linux, macOS and
// Windows; the shapes below are the ones cmd /c without /s mangled.
#include <gtest/gtest.h>
import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.shell;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;
namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

fs::path make_temp_dir(std::string_view prefix) {
    auto dir = fs::temp_directory_path() / fs::path(prefix);
    dir += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "failed to write " << path.string();
    out << content;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A program in a directory whose name holds a space; it prints its marker and
// its first argument.
fs::path make_tool(const fs::path& root) {
    const fs::path dir = root / "a b";
    fs::create_directories(dir);
    if constexpr (kWindows) {
        const fs::path tool = dir / "tool.bat";
        write_text(tool, "@echo hello-from-tool %~1\r\n");
        return tool;
    } else {
        const fs::path tool = dir / "tool";
        write_text(tool, "#!/bin/sh\necho hello-from-tool \"$1\"\n");
        fs::permissions(tool, fs::perms::owner_all, fs::perm_options::replace);
        return tool;
    }
}

std::string quoted(const fs::path& p) { return "\"" + p.string() + "\""; }

} // namespace

// The shape of libxpkg#52: the line starts with a quote and holds more than
// one pair. Without /s, cmd stripped the first and the last quote and ran
// nothing; here the program runs and its output is read back.
TEST(ShellTest, QuotedProgramPathWithSpacesReadsBackItsOutput) {
    const fs::path temp = make_temp_dir("libxpkg-shell-quoted-");
    const fs::path tool = make_tool(temp);

    const auto ran = shell::run(quoted(tool) + " \"x y\"",
                                { shell::Sink::Capture, shell::Sink::Null });
    EXPECT_EQ(ran.status.kind, shell::Status::Kind::Exit);
    EXPECT_EQ(ran.status.code, 0);
    EXPECT_NE(ran.output.find("hello-from-tool x y"), std::string::npos) << ran.output;

    fs::remove_all(temp);
}

// Output is read through a pipe, never by appending a redirect to the line:
// a redirect the caller wrote still sends the output where it says.
TEST(ShellTest, ACallerSuppliedRedirectStillWorks) {
    const fs::path temp = make_temp_dir("libxpkg-shell-redirect-");
    const fs::path tool = make_tool(temp);
    const fs::path out = temp / "a b" / "out file.txt";

    const auto ran = shell::run(quoted(tool) + " z > " + quoted(out),
                                { shell::Sink::Capture, shell::Sink::Null });
    EXPECT_EQ(ran.status.kind, shell::Status::Kind::Exit);
    EXPECT_EQ(ran.status.code, 0);
    EXPECT_EQ(ran.output, "");
    EXPECT_NE(read_text(out).find("hello-from-tool z"), std::string::npos);

    // The caller's 2>&1 lands stderr in the captured stdout.
    const auto viaGroup = shell::run(kWindows ? "(echo to-stderr 1>&2) 2>&1"
                                              : "{ echo to-stderr 1>&2; } 2>&1",
                                     { shell::Sink::Capture, shell::Sink::Null });
    EXPECT_NE(viaGroup.output.find("to-stderr"), std::string::npos) << viaGroup.output;

    fs::remove_all(temp);
}

TEST(ShellTest, ExitStatusIsReported) {
    const auto ok = shell::run("exit 0", { shell::Sink::Null, shell::Sink::Null });
    EXPECT_EQ(ok.status.kind, shell::Status::Kind::Exit);
    EXPECT_EQ(ok.status.code, 0);
    const auto three = shell::run("exit 3", { shell::Sink::Null, shell::Sink::Null });
    EXPECT_EQ(three.status.kind, shell::Status::Kind::Exit);
    EXPECT_EQ(three.status.code, 3);
}

TEST(ShellTest, StderrIsNotCapturedUnlessAsked) {
    const auto ran = shell::run("echo out-line && echo err-line 1>&2",
                                { shell::Sink::Capture, shell::Sink::Null });
    EXPECT_NE(ran.output.find("out-line"), std::string::npos);
    EXPECT_EQ(ran.output.find("err-line"), std::string::npos);
}

// The same lines through the recipe API: os.iorun returns the output and
// os.execute reports Lua 5.4's results, both through shell::run.
TEST(ShellTest, OsIorunAndOsExecuteRunTheLineAsWritten) {
    const fs::path temp = make_temp_dir("libxpkg-shell-lua-");
    const fs::path tool = make_tool(temp);
    const fs::path out = temp / "a b" / "exec out.txt";
    const fs::path pkg = temp / "shell.lua";
    write_text(pkg,
        "package = { name = \"shell\", xpm = { linux = { [\"0.0.1\"] = {} } } }\n"
        "function install()\n"
        "    print('IORUN[' .. os.iorun([==[" + quoted(tool) + " \"x y\"]==]) .. ']')\n"
        "    local ok, how, code = os.execute([==[" + quoted(tool) + " w > " + quoted(out) + "]==])\n"
        "    print('EXEC', tostring(ok), how, code)\n"
        "    local bad, badHow, badCode = os.execute('exit 4')\n"
        "    print('FAIL', tostring(bad), badHow, badCode)\n"
        "    print('SHELL', tostring(os.execute()))\n"
        "    return true\n"
        "end\n");

    auto exec = create_executor(pkg);
    ASSERT_TRUE(exec.has_value()) << exec.error();
    ExecutionContext ctx;
    ctx.pkg_name = "shell";
    ctx.version = "0.0.1";
    ctx.platform = kWindows ? "windows" : "linux";
    ctx.install_dir = temp / "install";
    const auto result = exec->run_hook(HookType::Install, ctx);
    ASSERT_TRUE(result.success) << result.error << "\n" << result.output;
    EXPECT_NE(result.output.find("IORUN[hello-from-tool x y"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("EXEC\ttrue\texit\t0"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("FAIL\tnil\texit\t4"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("SHELL\ttrue"), std::string::npos) << result.output;
    EXPECT_NE(read_text(out).find("hello-from-tool w"), std::string::npos);

    fs::remove_all(temp);
}

// ---- A program and its arguments, no shell between (design 2026-11-27 §9.6) ----

namespace {

// Each argument the child received, one per line in brackets: through
// `sh -c '...' sh <args>` on POSIX, where the arguments are positional
// parameters and never parsed again. On Windows the quoting is checked
// against the C runtime's own rules below instead.
shell::Command echo_args(std::vector<std::string> args) {
    shell::Command c;
    c.argv = { "sh", "-c", "for a in \"$@\"; do printf '[%s]\\n' \"$a\"; done", "sh" };
    for (auto& a : args) c.argv.push_back(std::move(a));
    return c;
}

} // namespace

TEST(SpawnTest, ArgumentsReachTheProgramAsTheyAre) {
    if constexpr (kWindows) GTEST_SKIP() << "the quoting is checked by WindowsCommandLineQuotesAsTheRuntimeSplits";
    const auto ran = shell::spawn(echo_args({ "a b", "quote\"d", "%PATH%", "x & y", "$HOME", "", "back\\slash\\" }),
                                  { shell::Sink::Capture, shell::Sink::Null });
    ASSERT_EQ(ran.status.kind, shell::Status::Kind::Exit) << ran.status.message;
    EXPECT_EQ(ran.output, "[a b]\n[quote\"d]\n[%PATH%]\n[x & y]\n[$HOME]\n[]\n[back\\slash\\]\n");
}

TEST(SpawnTest, WindowsCommandLineQuotesAsTheRuntimeSplits) {
    // Microsoft's documented examples of how the C runtime splits a line.
    EXPECT_EQ(shell::windows_command_line({ "a b", "c" }), "\"a b\" c");
    EXPECT_EQ(shell::windows_command_line({ "a\"b" }), "\"a\\\"b\"");
    EXPECT_EQ(shell::windows_command_line({ "a\\b" }), "a\\b");
    EXPECT_EQ(shell::windows_command_line({ "a b\\" }), "\"a b\\\\\"");
    EXPECT_EQ(shell::windows_command_line({ "" }), "\"\"");
    EXPECT_EQ(shell::windows_command_line({ "x & y", "%PATH%" }), "\"x & y\" %PATH%");
    EXPECT_EQ(shell::windows_command_line({ "C:\\Program Files\\x\\" }), "\"C:\\Program Files\\x\\\\\"");
}

TEST(SpawnTest, TheWorkingDirectoryAndEnvironmentAreTheChilds) {
    const fs::path temp = make_temp_dir("libxpkg-spawn-cwd-");
    shell::Command c;
    c.cwd = temp;
    c.env = { { "XPKG_SPAWN_PROBE", "a b" } };
    if constexpr (kWindows) c.argv = { "cmd.exe", "/d", "/c", "cd && echo %XPKG_SPAWN_PROBE%" };
    else c.argv = { "sh", "-c", "pwd; printf '%s\\n' \"$XPKG_SPAWN_PROBE\"" };
    const auto ran = shell::spawn(c, { shell::Sink::Capture, shell::Sink::Null });
    ASSERT_EQ(ran.status.kind, shell::Status::Kind::Exit) << ran.status.message;
    EXPECT_EQ(ran.status.code, 0);
    std::istringstream lines(ran.output);
    std::string dir, value;
    std::getline(lines, dir);
    std::getline(lines, value);
    if (!dir.empty() && dir.back() == '\r') dir.pop_back();
    if (!value.empty() && value.back() == '\r') value.pop_back();
    EXPECT_EQ(fs::canonical(dir), fs::canonical(temp)) << ran.output;
    EXPECT_EQ(value, "a b");
    EXPECT_EQ(std::getenv("XPKG_SPAWN_PROBE"), nullptr) << "the parent's environment is unchanged";
    fs::remove_all(temp);
}

TEST(SpawnTest, AMissingProgramOrDirectoryIsAFailureToStart) {
    const auto missing = shell::spawn({ .argv = { "no-such-program-for-libxpkg" } }, { shell::Sink::Null, shell::Sink::Null });
    EXPECT_EQ(missing.status.kind, shell::Status::Kind::Failed);
    EXPECT_EQ(missing.status.message, "no program named no-such-program-for-libxpkg on PATH");
    const fs::path temp = make_temp_dir("libxpkg-spawn-nocwd-");
    const auto nocwd = shell::spawn({ .argv = { kWindows ? "cmd.exe" : "sh" }, .cwd = temp / "absent" },
                                    { shell::Sink::Null, shell::Sink::Null });
    EXPECT_EQ(nocwd.status.kind, shell::Status::Kind::Failed);
    EXPECT_NE(nocwd.status.message.find("is not a directory"), std::string::npos) << nocwd.status.message;
    fs::remove_all(temp);
}

// The child's PATH finds the program, so an `env` that sets PATH decides.
TEST(SpawnTest, TheChildsPathFindsTheProgram) {
    const fs::path temp = make_temp_dir("libxpkg-spawn-path-");
    shell::Command c;
    if constexpr (kWindows) {
        fs::copy_file(fs::path(std::getenv("SystemRoot")) / "System32" / "cmd.exe", temp / "xpkg-probe.exe");
        c.argv = { "xpkg-probe", "/d", "/c", "exit 7" };
    } else {
        write_text(temp / "xpkg-probe", "#!/bin/sh\nexit 7\n");
        fs::permissions(temp / "xpkg-probe", fs::perms::owner_all, fs::perm_options::replace);
        c.argv = { "xpkg-probe" };
    }
    c.env = { { "PATH", temp.string() } };
    const auto ran = shell::spawn(c, { shell::Sink::Null, shell::Sink::Null });
    ASSERT_EQ(ran.status.kind, shell::Status::Kind::Exit) << ran.status.message;
    EXPECT_EQ(ran.status.code, 7);
    fs::remove_all(temp);
}

// A batch file re-parses its arguments in cmd: a list is refused, with the way out.
TEST(SpawnTest, ABatchFileTakesNoArgumentList) {
    if constexpr (!kWindows) GTEST_SKIP() << "batch files are Windows'";
    const fs::path temp = make_temp_dir("libxpkg-spawn-bat-");
    const fs::path tool = make_tool(temp);  // tool.bat
    const auto ran = shell::spawn({ .argv = { tool.string(), "x" } }, { shell::Sink::Null, shell::Sink::Null });
    EXPECT_EQ(ran.status.kind, shell::Status::Kind::Refused);
    EXPECT_NE(ran.status.message.find("pass the command as one string"), std::string::npos) << ran.status.message;
    fs::remove_all(temp);
}
