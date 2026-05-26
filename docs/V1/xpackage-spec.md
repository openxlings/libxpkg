# XPackage Spec V1

This document defines the package-authoring surface supported by libxpkg for `spec = "1"` xpkg files.

## Authoring Boundary

An xpkg V1 file is a Lua file that must stay within four layers:

1. `libxpkg` runtime modules exposed through `import("xim.libxpkg.*")`.
2. Standard Lua syntax and standard libraries.
3. The xpkg V1 schema: `package` metadata, `xpm` platform matrix, resources, deps, exports, and lifecycle hooks.
4. xpkg built-in APIs documented in this file.

Package files should not depend on APIs outside this boundary. New package code should prefer portable Lua helpers and the `xim.libxpkg.*` modules over host-specific assumptions.

## File Shape

An xpkg V1 file has two parts:

```lua
package = {
    spec = "1",
    name = "hello",
    description = "Example package",
    type = "package",
    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://example.com/hello-1.0.0-linux.tar.gz",
                sha256 = "0123456789abcdef",
            },
        },
    },
}

local pkginfo = import("xim.libxpkg.pkginfo")
local xvm = import("xim.libxpkg.xvm")

function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(pkginfo.install_file(), pkginfo.install_dir())
    return true
end

function config()
    xvm.add("hello", { bindir = pkginfo.install_dir() })
    return true
end

function uninstall()
    xvm.remove("hello")
    return true
end
```

The `package` table is declarative metadata. Runtime side effects belong in hooks.

## Package Metadata

Required fields:

| Field | Type | Description |
|-------|------|-------------|
| `spec` | string | Must be `"1"` for V1 packages. |
| `name` | string | Package name. |
| `description` | string | Short package description. |
| `type` | string | One of `package`, `script`, `template`, or `config`. |
| `xpm` | table | Platform matrix. |

Common optional fields:

| Field | Type | Description |
|-------|------|-------------|
| `namespace` | string | Optional package namespace, for example `config`. |
| `status` | string | `dev`, `stable`, or `deprecated`. |
| `archs` | string array | Supported CPU architectures. |
| `categories`, `keywords`, `programs` | string array | Search and registration metadata. |
| `authors`, `maintainers`, `licenses` | string array | Ownership and licensing metadata. |
| `homepage`, `repo`, `docs` | string | Project links. |
| `xvm_enable` | boolean | Whether the package is expected to expose xvm registrations. |

## Platform Matrix

Each `xpm` key is a platform name such as `linux`, `windows`, `macosx`, `ubuntu`, or `debian`.

```lua
xpm = {
    linux = {
        deps = {
            runtime = { "xim:zlib" },
            build = { "xim:gcc" },
        },
        exports = {
            runtime = {
                loader = "lib64/ld-linux-x86-64.so.2",
                libdirs = { "lib64", "lib" },
                abi = "linux-x86_64-glibc",
            },
        },
        ["latest"] = { ref = "1.0.0" },
        ["1.0.0"] = {
            url = "https://example.com/pkg-1.0.0.tar.gz",
            sha256 = "0123456789abcdef",
            mirrors = {
                GLOBAL = "https://example.com/pkg-1.0.0.tar.gz",
                CN = "https://mirror.example.cn/pkg-1.0.0.tar.gz",
            },
        },
    },
    ubuntu = { ref = "linux" },
}
```

Supported platform fields:

| Field | Description |
|-------|-------------|
| `["latest"] = { ref = "..." }` | Version alias. |
| `["version"] = { url = "...", sha256 = "..." }` | Download resource. |
| `["version"] = "XLINGS_RES"` | Resource supplied by the package index mirror. |
| `deps = { "pkg" }` | Legacy dependency list. Consumers treat it as both runtime and build deps. |
| `deps = { runtime = {...}, build = {...} }` | Split runtime/build dependencies. |
| `exports.runtime.loader` | Dynamic loader path relative to install dir. |
| `exports.runtime.libdirs` | Runtime library directories relative to install dir. |
| `exports.runtime.abi` | ABI tag used to disambiguate loader providers. |
| `ref = "platform"` | Platform inheritance. |

## Lifecycle Hooks

Hooks are optional unless the package type or behavior needs them:

| Hook | Purpose |
|------|---------|
| `installed()` | Return installed version string, or `nil`/`false` when absent. |
| `build()` | Build from source or transform downloaded content before install. |
| `install()` | Install files into `pkginfo.install_dir()`. |
| `config()` | Register commands, libraries, headers, or config side effects. |
| `uninstall()` | Remove registrations and package-owned files. |

For `type = "config"` packages, keep `install()` trivial when there is no payload:

```lua
function install()
    return true
end
```

Put configuration writes, tool registration, and validation in `config()`.

## Importing libxpkg Modules

Use `import("xim.libxpkg.<module>")` for xpkg runtime modules. Prefer assigning the return value to a local variable:

```lua
local pkginfo = import("xim.libxpkg.pkginfo")
local log = import("xim.libxpkg.log")
```

Available modules include:

| Module | Main APIs |
|--------|-----------|
| `pkginfo` | `name()`, `version()`, `install_file()`, `install_dir([dep, version])`, `dep_install_dir(dep, version)`, `deps_list()`, `build_dep(name, version)`, `with_build_deps_on_path(names, fn)` |
| `xvm` | `add(name, opt)`, `remove(name, version)`, `has(name, version)`, `info(name, version)`, `setup(name, opt)`, `teardown(name, opt)` |
| `system` | `exec(cmd, opt)`, `rundir()`, `xpkgdir()`, `bindir()`, `xpkg_args()`, `subos_sysrootdir()`, `run_in_script(content, admin)`, `unix_api()` |
| `log` | `debug()`, `info()`, `warn()`, `error()`, `set_level()`, `get_level()` |
| `json` | `decode()`, `encode()`, `loadfile()`, `savefile()` |
| `pkgmanager` | `install(target)`, `remove(target)`, `uninstall(target)` |
| `utils` | `filepath_to_absolute()`, `try_download_and_check()`, `input_args_process()` |
| `elfpatch` | ELF and Mach-O relocation helpers for binary packages. |
| `base64` | `encode()`, `decode()` |

## Built-In APIs

xpkg V1 provides a small built-in runtime for package scripts:

| API | Description |
|-----|-------------|
| `import(path)` | Loads an xpkg module such as `xim.libxpkg.pkginfo`. |
| `os.isfile(path)`, `os.isdir(path)` | File/directory checks. |
| `os.tryrm(path)`, `os.mkdir(path)` | Best-effort removal and directory creation. |
| `os.mv(src, dst)`, `os.trymv(src, dst)`, `os.cp(src, dst)` | File/directory movement and copy helpers. |
| `os.dirs(pattern)` | Directory glob helper. |
| `os.host()` | Current runtime platform name. |
| `os.exec(cmd)`, `os.iorun(cmd)` | Command execution helpers. |
| `path.join(...)`, `path.filename(path)`, `path.directory(path)`, `path.is_absolute(path)` | Path helpers. |
| `io.readfile(path)`, `io.writefile(path, content)` | Whole-file IO helpers. |
| `string.split(s, sep, plain)`, `string.trim(s)`, `string.replace(s, old, new)` | String helpers. |
| `cprint(fmt, ...)`, `format(...)`, `raise(msg)`, `try { ... }` | Convenience helpers retained by the xpkg runtime. |
| `is_host(name)` | Shortcut for matching the current runtime platform. |

Standard Lua APIs such as `string`, `table`, `math`, `io.open`, `os.getenv`, `pcall`, and `error` are available and are part of the supported authoring surface.

## Authoring Rules

- Prefer `xim.libxpkg.*` modules for package lifecycle integration.
- Prefer standard Lua for local parsing, table manipulation, string processing, and error handling.
- Keep top-level code declarative; side effects should run inside lifecycle hooks.
- Do not write secrets to logs. Use `log.info()`/`log.warn()`/`log.error()` for status messages.
- Do not mutate user shell profiles directly. Register commands through `xvm` and package dependencies through `xpm`.
- For config packages, preserve existing user config files where possible and back them up before overwriting.
