# OPENPPP2

可选依赖：实验性 corocc L0 协程后端**默认关闭**，默认仍使用 Boost fcontext。
参见[构建与测试](docs/development/COROCC_BACKEND_CN.md)及
[设计说明](docs/adr/0003-corocc-coroutine-backend_CN.md)。

> Status: Current
> Type: Navigation
> Last verified: 2026-07-22
> 文档索引：[按任务查找文档](docs/README_CN.md) · English: [English](README.md)

[English](README.md) | 简体中文

OPENPPP2 是一个 C++ 网络运行时。根目录 CMake 项目构建原生 `ppp` 可执行文件和静态
`openppp2_lib` 库。本页是仓库入口；配置、运维和实现细节请从下方当前有效的任务页面进入。

## 一条首次使用路径

首次运行原生 `ppp` 时，按以下顺序阅读：

1. [快速开始](docs/getting-started/README_CN.md)
2. [编写最小配置](docs/getting-started/CONFIGURE_CN.md)
3. [配置参考](docs/reference/CONFIGURATION_CN.md)与 [CLI 参考](docs/reference/CLI_REFERENCE_CN.md)——启动选定的客户端、服务端或代理模式
4. [运维清单与故障排查](docs/operations/OPERATIONS_CN.md)

## 按职责选择入口

| 职责或需求 | 当前规范入口 |
|---|---|
| 用户：安装、配置和使用 `ppp` | [快速开始](docs/getting-started/README_CN.md) · [任务指南](docs/guides/README_CN.md) |
| 运维人员：部署、观测、安全和排障 | [部署与运维](docs/operations/README_CN.md) |
| 开发者：理解或修改运行时 | [当前架构](docs/architecture/README_CN.md) · [开发文档](docs/development/README_CN.md) · [源码阅读指南](docs/development/SOURCE_READING_GUIDE_CN.md) |
| 参考：配置、CLI、接口和数据格式 | [参考手册](docs/reference/README_CN.md) · [项目接口全景图](docs/reference/PROJECT_INTERFACE_MAP_CN.md) |
| 历史依据与证据 | [历史归档](docs/archive/README_CN.md) · [ADR](docs/adr/README_CN.md) · [设计索引](docs/design/README_CN.md) |

## 核心与配套表面

- **核心 `ppp` 运行时：**`main.cpp`、`ppp/`、`common/`，以及根 CMake 按 Windows、
  Linux 或 Darwin 选择的平台源码。根项目仅在配置 `-DENABLE_TESTS=ON` 时纳入 `tests/`；
  支持的测试流程见[开发文档](docs/development/README_CN.md)。
- **Guardian：**`go/` 和 `go/guardian/` 是带有各自清单和文档的 Go 模块，属于配套表面，
  不是根 CMake 目标。
- **Android：**`android/` 包含仓库内的 Flutter 应用与 Android/NDK 构建文件。**iOS：**
  `ios/` 包含 Swift package target。
- **桌面端：**`desktop/client/` 使用独立的 Svelte/Vite 和 Tauri 清单。

这些配套表面拥有独立的构建和就绪边界。在将 Guardian、Android、iOS 或桌面端作为生产部署
路径前，请先阅读其本地文档和清单；构建核心 `ppp` 不会构建它们。

## `ppp` 启动链

常规运行路径（不含帮助或一次性辅助路径）如下：

```text
main.cpp
  -> ppp::facade::RunApplication()
  -> PppApplication::GetInstance().Run()
  -> PreparedArgumentEnvironment()
  -> Executors::Run()
  -> RunPreparedApplication()
  -> PppApplication::Main()
```

参数准备阶段会加载配置、解析应用模式并准备网络接口上下文。随后常规路径进入 `Main()`，执行
宿主预检并启动选定的客户端、服务端或代理运行时。实现级顺序和关闭行为请参阅[启动与生命周期]
(docs/architecture/STARTUP_AND_LIFECYCLE_CN.md)。

## 当前文档与历史文档

上方链接的当前任务页和分类页是配置、CLI、运维及开发指导的规范入口。`docs/archive/`、
`docs/adr/` 和 `docs/design/` 下的内容保存历史依据、决策、计划或证据；除非当前页面明确
引用，否则不能替代当前说明。依赖文档前请核对其 Status、Type、Last verified、语言配对和
父级索引元数据。英中文导航页保持语义配对。
