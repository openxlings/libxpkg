---
name: libxpkg-best-practices
description: Use when working on libxpkg — adding modules, writing tests, modifying xmake build config, working with Lua hooks, or fixing CI failures on Windows/Linux/macOS
---

# libxpkg Best Practices

## Overview

libxpkg 是 xpkg 规范的 C++23 参考实现，分为四个子模块。以下是开发过程中积累的关键模式和陷阱。

## xpkg V1 包脚本边界

xpkg 包作者的默认边界是：

- libxpkg 提供的 `xim.libxpkg.*` 模块。
- 标准 Lua 语法与标准库。
- xpkg V1 规范中的 `package` 元数据、`xpm` 平台矩阵和 lifecycle hooks。
- xpkg 运行时内置 API，例如 `import()`、`path.*`、扩展的 `os.*`/`io.*`/`string.*`、`is_host()`、`try { ... }`。

新增或审查包脚本时，优先判断它是否只依赖上述四类能力。不要把包脚本 API 描述成某个外部构建工具的 API；在规范、注释和 review 结论中使用“xpkg 内置 API”或“xpkg runtime API”。

规范入口：`docs/V1/xpackage-spec.md`。

## 模块架构

```
mcpplibs.xpkg              ← 纯 C++ 数据模型，零外部依赖
mcpplibs.xpkg.loader       ← model + lua；解析 .lua 包文件
mcpplibs.xpkg.index        ← model；纯 C++ 索引操作
mcpplibs.xpkg.executor     ← model + lua；执行 Lua 钩子
```

**规则：** 每个模块独立一个测试目标（`xpkg_model_test` 等）。模块间不允许循环依赖。

---

## 1. C++23 模块文件规范

**禁止**在模块声明后使用 `#include` 引入标准头：

```cpp
// ❌ 错误
export module mcpplibs.xpkg.loader;
#include <string>   // 非法：模块声明后不能 #include

// ✅ 正确
module;
#include "lua.h"            // 全局模块分段：只放无法模块化的 C 头文件
export module mcpplibs.xpkg.loader;
import std;                 // 标准库统一用 import std;
import mcpplibs.xpkg;
```

**全局模块分段**（`module;` 到 `export module ...;` 之间）仅用于 Lua 等 C 库头文件。

---

## 2. GCC 15 模块 ABI — 含容器成员的结构体析构函数

**症状：** `-O2` 下链接出现 `undefined reference to ~_Vector_impl` / `~_Hashtable_alloc`，`-O0` 无问题。

**根因：** GCC 15 会将类体内定义的析构函数（包括隐式 inline）标记为可内联进 BMI，导入方内联展开后产生对 `std` 内部 inline 函数的悬空调用。

**修复：** 含 `std::vector` / `std::unordered_map` 成员的结构体，析构函数必须在类体外定义：

```cpp
// xpkg.cppm — export namespace 内：仅声明
export namespace mcpplibs::xpkg {
struct Package {
    std::string name;
    std::vector<std::string> authors;
    PlatformMatrix xpm;
    ~Package();   // ← 只声明，不定义
};
} // namespace mcpplibs::xpkg

// export namespace 之外：定义为 = default
namespace mcpplibs::xpkg {
Package::~Package() = default;   // ← outlined symbol，链接方可见
}
```

**修改模块接口后必须：**
```bash
xmake clean --all
xmake build <target>
```
否则旧 BMI 缓存会导致 `Bad file data` 错误。

---

## 3. C++23 结构化绑定限制

C++23 **不支持**匿名占位符（该特性属于 C++26）：

```cpp
// ❌ C++23 不支持
for (auto& [, members] : index.mutex_groups) { ... }

// ✅ 用具名变量 + (void) 压制警告
for (auto& [key, members] : index.mutex_groups) {
    (void)key;
    ...
}
```

---

## 4. xmake 跨平台路径注入

**问题：** `$(projectdir)` 在 Windows 上展开为反斜杠路径（`D:\a\libxpkg\...`）。直接用 `add_defines` 嵌入 C 字符串字面量时，`\a` 被解释为 BEL 字符，路径损坏。

**错误写法：**
```lua
add_defines('XPKG_TEST_PKGINDEX="$(projectdir)/tests/fixtures/pkgindex"')
-- Windows 结果：D:libxpkglibxpkg/tests/...（路径损坏）
```

**正确写法：** 使用 `on_config` 回调 + `gsub` 规范化为正斜杠：
```lua
on_config(function(target)
    -- Normalize to forward slashes: Windows $(projectdir) contains backslashes
    -- which are misinterpreted as C escape sequences in string literals.
    local dir = path.join(os.projectdir(), "tests", "fixtures", "pkgindex")
    dir = dir:gsub("\\", "/")
    target:add("defines", 'XPKG_TEST_PKGINDEX="' .. dir .. '"')
end)
```

所有注入路径的 `add_defines` 均须改为此模式。

---

## 5. Lua 钩子文件规范

xpkg 包 `.lua` 文件应使用 `libxpkg + 标准 Lua + xpkg 规范 + xpkg 内置 API`。`import()` 可加载 `xim.libxpkg.*` 模块；推荐捕获返回值为局部变量，避免隐式全局造成测试和复用困难：

```lua
-- 不推荐：依赖隐式全局
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

-- 推荐：捕获为局部变量
local pkginfo = import("xim.libxpkg.pkginfo")
local xvm     = import("xim.libxpkg.xvm")
```

包脚本允许使用：
- 标准 Lua：`string/table/math/io/os.getenv/pcall/error` 等。
- xpkg 内置 API：`path.join`、`os.isfile`、`os.isdir`、`os.tryrm`、`os.mv`、`io.readfile`、`io.writefile`、`is_host`、`try { ... }` 等。
- libxpkg 模块：`pkginfo/xvm/system/log/json/pkgmanager/utils/elfpatch/base64`。

**钩子函数模板：**
```lua
-- installed() — 检查是否已安装，返回版本字符串或 nil
function installed()
    local dir = pkginfo.install_dir()
    if dir and os.isfile(dir .. "/.installed") then
        return pkginfo.version()
    end
    return nil
end

-- install() — 安装逻辑
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(dir .. "/.installed", pkginfo.version())
    return true
end

-- config() — 注册命令/库/header 或写配置
function config()
    local dir = pkginfo.install_dir()
    if xvm then xvm.add(pkginfo.name(), { bindir = dir }) end
    return true
end

-- uninstall() — 卸载逻辑
function uninstall()
    local dir = pkginfo.install_dir()
    os.remove(dir .. "/.installed")
    if xvm and xvm.has(pkginfo.name()) then
        xvm.remove(pkginfo.name())
    end
    return true
end
```

---

## 6. 测试 Fixture 结构

`tests/fixtures/pkgindex/` 是自包含测试索引，不依赖外部仓库：

```
tests/fixtures/pkgindex/
└── pkgs/
    └── h/
        └── hello.lua    # 完整钩子示例
```

测试文件通过宏获取路径（带 `#ifndef` 回退）：
```cpp
#ifndef XPKG_TEST_PKGINDEX
#  define XPKG_TEST_PKGINDEX "tests/fixtures/pkgindex"
#endif
static const fs::path PKGINDEX{ XPKG_TEST_PKGINDEX };
```

---

## 7. CI — 并行构建竞态

**问题：** 依赖链极短的目标（如 `basic`，仅依赖 `mcpplibs-xpkg`）在并行构建时，`std.gcm` / `std.ifc` 尚未就绪就开始编译，报 `could not find module 'std'`。

**修复：** 对依赖链短的示例/工具目标，使用 `#include` 替代 `import std;`：
```cpp
// examples/basic.cpp — 避免 import std; 的并行竞态
#include <iostream>
#include <string>
import mcpplibs.xpkg;
```

功能更丰富的目标（`lifecycle`）通过 `add_deps` 传递完整依赖链，`std` 模块会提前就绪，可安全使用 `import std;`。

---

## 快速参考

| 场景 | 解决方案 |
|------|----------|
| 链接失败 `~_Vector_impl` `-O2` | 析构函数类体外 `= default` |
| 神秘构建错误 `Bad file data` | `xmake clean --all` |
| 路径在 Windows 损坏 | `on_config` + `gsub("\\", "/")` |
| Lua hook 中 pkginfo 为 nil | `local pkginfo = import(...)` |
| CI `import std;` 找不到模块 | 改用 `#include` 或加全依赖链 |
| C++23 匿名绑定 `[,x]` | 改为 `[key, x]` + `(void)key` |
