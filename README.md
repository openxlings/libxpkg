# libxpkg

> xpkg 包描述规范的 C++23 参考实现 — `import mcpplibs.xpkg;`

[![CI](https://github.com/openxlings/libxpkg/actions/workflows/ci.yml/badge.svg)](https://github.com/openxlings/libxpkg/actions/workflows/ci.yml)

**libxpkg** 是 xpkg 格式的 C++23 参考实现，让任何 C++ 工具都能读取、索引、检查和执行 xpkg 的 recipe，而无需依赖 xlings 本身。

格式的规范只有一份：[`docs/spec/xpkg.md`](docs/spec/xpkg.md)（在这里修改；xlings 的 `docs/specs/xpkg.md` 是它逐字的拷贝，站点发布那一份）。
规范的可执行部分是一致性用例 [`tests/conformance/`](tests/conformance/README.md)。

## 模块

| 模块 | 源文件 | 功能 |
|------|--------|------|
| `mcpplibs.xpkg` | `xpkg.cppm` | 数据模型：`Package`、`PackageIndex`；架构身份表 |
| `mcpplibs.xpkg.loader` | `xpkg-loader.cppm` | 读取 recipe，构建索引；索引的声明（`index-compat.json` 的模板） |
| `mcpplibs.xpkg.index` | `xpkg-index.cppm` | 搜索、别名解析、索引合并 |
| `mcpplibs.xpkg.compat` | `xpkg-compat.cppm` | 资源解析（`resolve_resource`）；条目怎样说明架构 |
| `mcpplibs.xpkg.executor` | `xpkg-executor.cppm` | 运行 hook：`spec` 为 1、2 的环境（冻结的兼容层）与 `spec = "3"` 的环境 |
| `mcpplibs.xpkg.shell` | `xpkg-shell.cppm` | 按参数列表或经 shell 运行命令 |
| `mcpplibs.xpkg.check` | `xpkg-check.cppm` | 格式的检查器（`check_recipe`、`check_index`）；接口清单的数据（`interface_manifest`） |
| `mcpplibs.xpkg.conformance` | `xpkg-conformance.cppm` | 一致性用例：宿主经自己的安装跑同一组用例 |

## 快速开始

### 读取包元数据

```cpp
#include <iostream>
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;

int main() {
    auto pkg = mcpplibs::xpkg::load_package("path/to/hello.lua");
    if (pkg) {
        std::cout << "name: " << pkg->name << "\n";
        std::cout << "desc: " << pkg->description << "\n";
    }
    return 0;
}
```

### 完整生命周期（加载 → 索引 → 搜索 → 安装 → 卸载）

```cpp
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.index;
import mcpplibs.xpkg.executor;

using namespace mcpplibs::xpkg;

// 1. 加载包
auto pkg = load_package("pkgindex/pkgs/h/hello.lua");

// 2. 构建索引并搜索
auto idx  = build_index("pkgindex/");
auto hits = search(*idx, "hello");

// 3. 创建执行器并运行钩子
auto exec = create_executor("pkgindex/pkgs/h/hello.lua");
ExecutionContext ctx{ .pkg_name = "hello", .version = "1.0.0", ... };
exec->run_hook(HookType::Install,   ctx);
exec->run_hook(HookType::Uninstall, ctx);
```

完整示例见 [`examples/lifecycle.cpp`](examples/lifecycle.cpp)。

## 项目结构

```
libxpkg/
├── src/
│   ├── xpkg*.cppm             # 上表的模块
│   └── lua-stdlib/            # 嵌入的 Lua：模块、接口清单（manifest.lua）、
│                              # spec 3 的环境（spec3.lua）、兼容层（compat.lua）
├── docs/spec/xpkg.md          # 格式的规范
├── tests/
│   ├── conformance/           # 一致性用例（嵌入库中，宿主可取用）
│   ├── fixtures/              # 测试用的包索引
│   └── test_*.cpp
├── examples/
│   ├── basic.cpp              # 最小示例
│   └── lifecycle.cpp          # 完整生命周期演示
├── build.mcpp                 # 构建期把 lua-stdlib/ 与一致性用例嵌入为字符串
├── .agents/
│   ├── docs/                  # 开发问题排查报告
│   ├── plans/                 # 设计与实现方案
│   └── skills/                # 项目最佳实践
└── mcpp.toml
```

## 构建与测试

本库由 **mcpp** 构建。mcpp 自带受管沙箱(工具链由 `mcpp.toml` 固定),
不需要手工准备编译器,也不再有 xmake / CMake 构建入口。

```bash
xlings install -y   # 装 .xlings.json 里固定的 mcpp
mcpp build
mcpp test
```

macOS 专用的 elfpatch 用例需要 `install_name_tool`,在 Linux 上跑不了,
CI 里按名字过滤:

```bash
mcpp test -- --gtest_filter=-ExecutorTest.ApplyElfpatchAuto_*
```

**Lua stdlib 的生成**

`src/lua-stdlib/**.lua` 是唯一的源码。它需要以裸字符串字面量的形式进二进制,
这一步由 `build.mcpp` 在构建期完成:生成的 `xpkg-lua-stdlib.cppm` 写进
`MCPP_OUT_DIR`,通过 `generated=` 交给构建,**不进版本库**。

所以改完 `.lua` 直接 `mcpp build` 即可,没有"忘记重新生成"这回事 ——
仓库里只有一份拷贝,两份拷贝对不上这种状态不存在。

(它曾经存在过:0.0.47 的 `xvm.files` 只改了生成物,源 `.lua` 一直没有。)

## CI/CD

GitHub Actions 在 Linux、macOS、Windows 上构建并测试,工具链由 mcpp 沙箱提供
(Linux 上全部测试;macOS 与 Windows 上 shell、模块接口与一致性用例)。

## 作为依赖引入

在使用方的 `mcpp.toml` 里声明:

```toml
[dependencies]
"mcpplibs.xpkg" = "0.0.68"
```

## 相关链接

- [xpkg 规范](docs/spec/xpkg.md)（站点上的拷贝：xlings `docs/specs/xpkg.md`）
- [hook 接口参考](https://github.com/openxlings/xlings/blob/main/docs/generated/xpkg-hook-api.md)（由接口清单生成）
- [xim-pkgindex — 官方包索引仓库](https://github.com/openxlings/xim-pkgindex)
- [xlings — 使用 libxpkg 的包管理器](https://github.com/openxlings/xlings)
- [mcpp-style-ref — 现代 C++ 编码风格参考](https://github.com/mcpp-community/mcpp-style-ref)
- [mcpplibs/lua — C API Lua 绑定](https://github.com/mcpplibs/lua)
