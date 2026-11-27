-- manifest.lua: the hook interface, one row per name (design 2026-11-27 §9.9).
--
-- The one statement of what a hook can call. Three things come from it, so
-- they cannot disagree:
--   * the environment of a recipe of spec "3" (spec3.lua): a module is seen
--     through a view holding exactly the names listed here for it;
--   * the reference (xlings' docs/generated/xpkg-hook-api.md, generated);
--   * the old-to-new table the checker and the migration tool read (`compat`).
-- Tests hold the modules to it: every function a module has is listed here,
-- as a name of the interface, of the compatibility layer, or of the host.
--
-- A row: { name, kind, signature, summary, since = <libxpkg version>,
-- platforms = <what differs> }. `since` is absent for names that predate
-- 0.0.68. kind is one of
--   action       changes the machine; when what it states cannot be made true
--                it raises (it returns true, never false);
--   query        changes nothing and answers; it raises only when it cannot
--                answer ("there is none" is an answer);
--   declaration  hands something to the host, which does it after the hook;
--   pure         computes from its arguments alone. The only kind a recipe may
--                call at its top level.
return {
    modules = {
        { name = "fs", summary = "files and directories", entries = {
            { "exists",     "query", "fs.exists(path) -> boolean", "path exists; a link is followed" },
            { "is_file",    "query", "fs.is_file(path) -> boolean", "path is a regular file" },
            { "is_dir",     "query", "fs.is_dir(path) -> boolean", "path is a directory", since = "0.0.68" },
            { "is_symlink", "query", "fs.is_symlink(path) -> boolean", "path is a symbolic link" },
            { "readlink",   "query", "fs.readlink(path) -> string|nil", "the target of the link at path; nil when path is no link" },
            { "read",       "query", "fs.read(path) -> string", "the bytes of the file; raises when it cannot be read", since = "0.0.68" },
            { "list",       "query", "fs.list(dir) -> { {name, path, type}, ... }",
              "the entries of dir sorted by name, type \"file\" | \"dir\" | \"symlink\" | \"other\"; raises when dir is not a directory",
              since = "0.0.68" },
            { "mkdir",      "action", "fs.mkdir(path)", "path is a directory, its parents made; already one is done", since = "0.0.68" },
            { "remove",     "action", "fs.remove(path)", "path does not exist: a file, a link or a whole tree; never there is done",
              since = "0.0.68" },
            { "move",       "action", "fs.move(src, dst [, { replace = true }])",
              "dst is what src was and src is gone; dst is the final path, its parents made; one already there is an error unless replace; across devices it is copied and removed",
              since = "0.0.68" },
            { "copy",       "action", "fs.copy(src, dst [, { replace = true }])",
              "dst is a copy of src (a directory whole, links kept as links); dst as for move", since = "0.0.68" },
            { "symlink",    "action", "fs.symlink(target, link)", "link is a symbolic link to target",
              since = "0.0.68", platforms = "Windows may need a privilege to make one" },
            { "write",      "action", "fs.write(path, text)", "the file at path holds text, its parents made", since = "0.0.68" },
            { "chmod",      "action", "fs.chmod(path, mode)", "path has the mode: octal digits (\"755\") or \"+x\"",
              since = "0.0.68", platforms = "does nothing on Windows (after checking path exists)" },
            { "extract",    "action", "fs.extract(archive, dir [, { strip = n }])",
              "dir holds the archive's members, the first n components of each path dropped; a member with fewer is an error",
              since = "0.0.68" },
        } },
        { name = "system", summary = "programs and the install's directories", entries = {
            { "exec",  "action", "system.exec(cmd [, { cwd, env, retry, tty, capture }]) -> string|nil",
              "cmd ran and succeeded, else it raises with the command and how it ended. cmd is a list { program, arg, ... } (no shell) or one shell line; capture returns its stdout",
              platforms = "a list naming a batch file is refused on Windows" },
            { "run",   "query", "system.run(cmd [, { cwd, env }]) -> { ok, code, out [, error] }",
              "what cmd does: its exit code and stdout; a failure or a program that cannot start (code 127) is an answer",
              since = "0.0.68" },
            { "admin_step", "declaration", "system.admin_step(argv, why)",
              "a step only an administrator may take, for the host to ask about once after the install" },
            { "rundir",           "query", "system.rundir() -> string", "the directory xlings runs from" },
            { "xpkgdir",          "query", "system.xpkgdir() -> string", "the store of installed packages" },
            { "bindir",           "query", "system.bindir() -> string", "the directory of the home's program shims" },
            { "subos_sysrootdir", "query", "system.subos_sysrootdir() -> string", "the active SubOS's root" },
            { "xpkg_args",        "query", "system.xpkg_args() -> { string, ... }", "the arguments the install was given" },
        } },
        { name = "pkginfo", summary = "what is being installed", entries = {
            { "name",         "query", "pkginfo.name() -> string", "the package's name" },
            { "version",      "query", "pkginfo.version() -> string", "the version being installed" },
            { "install_dir",  "query", "pkginfo.install_dir() -> string", "where the payload is" },
            { "install_file", "query", "pkginfo.install_file() -> string", "the downloaded file (read-only)" },
            { "platform",     "query", "pkginfo.platform() -> string", "the install's platform: linux, macosx, windows", since = "0.0.68" },
            { "arch",         "query", "pkginfo.arch() -> string", "the install's architecture, by the format's name: x86_64, aarch64, ...",
              since = "0.0.68" },
            { "deps_list",        "query", "pkginfo.deps_list() -> { string, ... }", "the dependencies as the recipe wrote them" },
            { "resolved_dep",     "query", "pkginfo.resolved_dep(name [, version]) -> table|nil", "a runtime dependency as the resolver settled it" },
            { "dep_install_dir",  "query", "pkginfo.dep_install_dir(name [, version]) -> string|nil", "where a dependency's payload is" },
            { "build_dep",        "query", "pkginfo.build_dep(name [, version]) -> table|nil", "a build dependency's payload" },
            { "tool_payload_dir", "query", "pkginfo.tool_payload_dir(package [, version]) -> string|nil", "where a tool package's payload is" },
            { "with_build_deps_on_path", "action", "pkginfo.with_build_deps_on_path(names, fn)",
              "fn runs with those build dependencies' programs first on PATH" },
        } },
        { name = "path", summary = "path strings", entries = {
            { "join",        "pure", "path.join(part, ...) -> string", "the parts joined with \"/\"", since = "0.0.68" },
            { "filename",    "pure", "path.filename(p) -> string", "the last component", since = "0.0.68" },
            { "directory",   "pure", "path.directory(p) -> string", "everything before the last component", since = "0.0.68" },
            { "is_absolute", "pure", "path.is_absolute(p) -> boolean", "\"/...\" or a drive letter", since = "0.0.68" },
        } },
        { name = "log", summary = "what the recipe says", entries = {
            { "debug", "declaration", "log.debug(fmt, ...)", "a line of the hook log only" },
            { "info",  "declaration", "log.info(fmt, ...)", "a line of progress, shown with -v" },
            { "warn",  "declaration", "log.warn(fmt, ...)", "a notice to the user, shown after the summary" },
            { "error", "declaration", "log.error(fmt, ...)", "a notice to the user, shown after the summary" },
        } },
        { name = "xvm", summary = "what the home registers", entries = {
            { "add",      "declaration", "xvm.add(name [, opt])", "register a program, library or version" },
            { "files",    "declaration", "xvm.files(opt)", "place a file of the payload into the SubOS" },
            { "remove",   "declaration", "xvm.remove(name [, version])", "unregister it" },
            { "use",      "declaration", "xvm.use(name, version)", "make a version the active one" },
            { "setup",    "declaration", "xvm.setup(name, opt)", "register a package's programs, libraries and headers at once" },
            { "teardown", "declaration", "xvm.teardown(name, opt)", "unregister what setup registered" },
            { "has",      "query", "xvm.has(name [, version]) -> boolean", "it is registered" },
            { "info",     "query", "xvm.info(name [, version]) -> table|nil", "what is registered for it" },
        } },
        { name = "subos", summary = "the SubOS's environment", entries = {
            { "env", "declaration", "subos.env(opt)", "a variable every process in the SubOS gets" },
        } },
        { name = "pkgmanager", summary = "other packages", entries = {
            { "install", "declaration", "pkgmanager.install(target)", "install another package after this one" },
            { "remove",  "declaration", "pkgmanager.remove(target)", "remove another package after this one" },
        } },
        { name = "elfpatch", summary = "the loader and search paths of ELF and Mach-O files", entries = {
            { "set",       "declaration", "elfpatch.set(opt)", "how the host patches the payload after install()" },
            { "skip",      "declaration", "elfpatch.skip()", "the host patches nothing" },
            { "set_interpreter", "action", "elfpatch.set_interpreter(target, interpreter [, opt])",
              "the files' PT_INTERP is interpreter; raises when files are named and patchelf is missing" },
            { "set_rpath", "action", "elfpatch.set_rpath(target, rpath [, opt])",
              "the files' search path is rpath; raises when files are named and the tool is missing" },
            { "patch_elf_loader_rpath", "action", "elfpatch.patch_elf_loader_rpath(target, opt) -> table",
              "loader and search path together; raises when files are named and the tool is missing" },
            { "relocate_build_paths", "action", "elfpatch.relocate_build_paths(opt)", "build-machine paths in the payload's text files rewritten" },
            { "host_link_interposer", "action", "elfpatch.host_link_interposer(opt)", "an interposer linked against the host's driver" },
            { "closure_lib_paths", "query", "elfpatch.closure_lib_paths(opt) -> { string, ... }", "the library directories of the install's closure" },
            { "is_shrink", "query", "elfpatch.is_shrink() -> boolean", "the recipe asked for shrunk search paths" },
        } },
        { name = "json", summary = "JSON text", entries = {
            { "encode", "pure", "json.encode(value) -> string", "value as JSON text" },
            { "decode", "pure", "json.decode(text) -> value", "the value JSON text holds" },
        } },
        { name = "base64", summary = "base64 text", entries = {
            { "encode", "pure", "base64.encode(bytes) -> string", "bytes as base64" },
            { "decode", "pure", "base64.decode(text) -> string", "the bytes base64 text holds" },
        } },
    },

    -- The part of standard Lua a recipe of spec "3" sees: these globals, these
    -- libraries whole, and these functions of os. No io, no os.execute, no
    -- require, no load: what changes the machine goes through the modules.
    stdlib = {
        globals = {
            "assert", "error", "ipairs", "next", "pairs", "pcall", "print", "rawequal", "rawget",
            "rawlen", "rawset", "select", "setmetatable", "getmetatable", "tonumber",
            "tostring", "type", "xpcall", "import",
        },
        libraries = { "string", "table", "math", "utf8" },
        os = { "time", "date", "clock", "getenv" },
    },

    -- The names the host itself calls; not part of a recipe's interface.
    host = { "elfpatch._apply", "elfpatch.apply_auto" },

    -- The compatibility layer: names of spec "1" and "2" only, each with what
    -- a recipe of spec "3" writes instead (nil: nothing is needed).
    compat = {
        { "os.mv",         "fs.move" },
        { "os.trymv",      "fs.move" },
        { "os.cp",         "fs.copy" },
        { "os.mkdir",      "fs.mkdir" },
        { "os.tryrm",      "fs.remove" },
        { "os.isfile",     "fs.is_file" },
        { "os.isdir",      "fs.is_dir" },
        { "os.dirs",       "fs.list" },
        { "os.exec",       "system.exec" },
        { "os.execute",    "system.run" },
        { "os.iorun",      "system.run(cmd).out" },
        { "os.cd",         "system.exec's cwd option" },
        { "os.setenv",     "system.exec's env option" },
        { "os.addenv",     "system.exec's env option" },
        { "os.host",       "pkginfo.platform" },
        { "os.sleep",      nil },
        { "os.remove",     "fs.remove" },
        { "os.rename",     "fs.move" },
        { "os.tmpname",    nil },
        { "io.readfile",   "fs.read" },
        { "io.writefile",  "fs.write" },
        { "io.open",       "fs.read / fs.write" },
        { "io.popen",      "system.run" },
        { "path",          "import(\"xim.libxpkg.path\")" },
        { "format",        "string.format" },
        { "raise",         "error" },
        { "is_host",       "pkginfo.platform" },
        { "try",           "pcall" },
        { "cprint",        "log.info" },
        { "fs.is_directory", "fs.is_dir" },
        { "fs.mkdir_p",    "fs.mkdir" },
        { "fs.remove_all", "fs.remove" },
        { "fs.copy_file",  "fs.copy" },
        { "fs.entries",    "fs.list" },
        { "fs.files",      "fs.list" },
        { "fs.dirs",       "fs.list" },
        { "fs.basename",   "path.filename" },
        { "fs.dirname",    "path.directory" },
        { "system.run_in_script", "system.exec, or system.admin_step for a step that needs an administrator" },
        { "system.unix_api", nil },
        { "log.set_level", nil },
        { "log.get_level", nil },
        { "xvm.log_tag",   nil },
        { "pkgmanager.uninstall", "pkgmanager.remove" },
        { "elfpatch.auto", "elfpatch.set / elfpatch.skip" },
        { "elfpatch.is_auto", nil },
        { "json.loadfile", "json.decode(fs.read(path))" },
        { "json.savefile", "fs.write(path, json.encode(value))" },
        { "utils.filepath_to_absolute", "path.join" },
        { "utils.try_download_and_check", nil },
        { "utils.input_args_process", nil },
        { "import of an unknown module", "an error: the module's name is checked" },
    },
}
