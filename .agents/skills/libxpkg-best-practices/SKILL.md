---
name: libxpkg-best-practices
description: Use when working on libxpkg — adding modules, writing tests, the mcpp build (mcpp.toml, build.mcpp), working with Lua hooks and their two environments, or fixing CI failures on Windows/Linux/macOS
---

# libxpkg Best Practices

## Overview

libxpkg 是 xpkg 规范的 C++23 参考实现，分为四个子模块。以下是开发过程中积累的关键模式和陷阱。

## 模块架构

```
mcpplibs.xpkg              ← 纯 C++ 数据模型（含架构身份表），零外部依赖
mcpplibs.xpkg.compat       ← model；条目的解析（resolve_resource）与架构证据
mcpplibs.xpkg.loader       ← model + lua；读 recipe 的 package 表、建索引
mcpplibs.xpkg.index        ← model；纯 C++ 索引操作
mcpplibs.xpkg.shell        ← lua；进程层：一行 shell、或程序加参数列表
mcpplibs.xpkg.fs           ← shell + lua；hook 能用的文件函数（fs 模块、兼容层的 os.*）
mcpplibs.xpkg.executor     ← 以上 + lua；执行 Lua 钩子，两种环境
```

**规则：** 测试按文件分（`tests/test_*.cpp` 各是一个测试程序，`mcpp test <名字>` 单跑）。
模块间不允许循环依赖。

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

**修改模块接口后：** `mcpp build` 按依赖重建 BMI。若遇到 `Bad file data` 一类陈旧缓存的错误，
删掉 `target/` 再 `mcpp build`。

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

## 4. 测试里的路径

测试在项目根运行（`mcpp test`），fixture 用相对路径（`tests/fixtures/pkgindex`，见第 6 节）。
临时文件放在 `fs::temp_directory_path()` 下、每个用例一个目录，用完删除。
跨平台的用例（进程层、模块接口：`tests/test_shell.cpp`、`tests/test_api.cpp`）在 CI 的三个平台上都跑，
其余在 Linux 上跑（`.github/workflows/ci.yml`）。

---

## 5. Lua 钩子文件与两种环境

recipe 的 `spec` 选择 hook 的环境（设计 2026-11-27 §9.6，规范 `docs/spec/xpkg.md`）：

- `spec = "3"`：标准 Lua 的纯计算部分加 `import` 进来的模块，没有兼容层；一次调用是动作、询问或声明之一，
  动作没做到就报错，容错只用 `pcall`。
- `spec` 为 1、2：原样冻结的兼容层（`src/lua-stdlib/compat.lua`）：`os.mv`、`os.cp`、`is_host` 等靠返回值报告失败。

`import("xim.libxpkg.<名字>")` 把模块绑到它最后一段名字上（`fs`、`pkginfo`），也返回它：

```lua
package = { spec = "3", name = "demo", xpm = { linux = { ["1.0.0"] = { url = "...", sha256 = { x86_64 = "..." }, strip = 1 } } } }

import("xim.libxpkg.fs")
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    -- 落地已由宿主完成：内容在 pkginfo.install_dir() 里
    fs.chmod(path.join(pkginfo.install_dir(), "bin", "demo"), "+x")
end

function config()
    xvm.add("demo")
end
```

**别把动作当成会返回 false 的调用：** 新接口的动作失败即报错，返回值没有需要检查的。
要容忍失败就写 `pcall(fs.remove, p)`。

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

功能更丰富的目标（`lifecycle`）依赖整个库，`std` 模块会提前就绪，可安全使用 `import std;`。

---

## 快速参考

| 场景 | 解决方案 |
|------|----------|
| 链接失败 `~_Vector_impl` `-O2` | 析构函数类体外 `= default` |
| 神秘构建错误 `Bad file data` | 删掉 `target/` 再 `mcpp build` |
| 一个用例要在三个平台上跑 | 放进 `test_shell.cpp` 或 `test_api.cpp` |
| `spec = "3"` 的 hook 里调用不存在 | 它属于兼容层；查规范里的对照表 |
| CI `import std;` 找不到模块 | 改用 `#include` 或加全依赖链 |
| C++23 匿名绑定 `[,x]` | 改为 `[key, x]` + `(void)key` |
