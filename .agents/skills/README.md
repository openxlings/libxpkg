# libxpkg Agent Skills

用于指导 Agent 在 libxpkg 仓库中编写、审查和维护 C++23 模块、xpkg Lua 规范、测试 fixture 与构建配置。

## 可用技能

| 技能 | 说明 |
|------|------|
| [libxpkg-best-practices](libxpkg-best-practices/SKILL.md) | libxpkg 项目实践：模块边界、Lua/xpkg runtime、测试 fixture、跨平台构建与 CI |
| [mcpp-style-ref](mcpp-style-ref/SKILL.md) | 面向 mcpp 项目的 Modern/Module C++ (C++23) 命名、模块化与实践规则 |

## 使用方式

要在 Cursor 中使用，请将技能软链接或复制到项目的 `.cursor/skills/`：

```bash
mkdir -p .cursor/skills
ln -s ../../skills/mcpp-style-ref .cursor/skills/mcpp-style-ref
```

或安装为个人技能：

```bash
ln -s /path/to/mcpp-style-ref/skills/mcpp-style-ref ~/.cursor/skills/mcpp-style-ref
```
