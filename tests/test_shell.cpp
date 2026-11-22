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
