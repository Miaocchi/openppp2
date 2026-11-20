# OpenPPP2 产品：native `ppp`、Go 管理后端与 Guardian

> [English](debug.md) · [原生文档](../docs/README_CN.md) · [管理后端](../docs/guides/MANAGEMENT_BACKEND_CN.md) · [Guardian 架构](guardian/ARCHITECTURE_CN.md)

**Status:** 实验性/内部配套表面

**Type:** 按任务组织的产品索引

**Last verified:** 2026-11-20（本地源码及所链接文档）

OpenPPP2 有三个相关但彼此独立的产品。阅读配置或运维说明前，先选择正确的产品：

| 产品 | 用途 | 不是什么 |
|---|---|---|
| **Native `ppp`** | 实现 VPN/隧道数据面、传输、主机网络和运行时生命周期的 C++ client/server/proxy 进程。 | 不是 Go 服务、订阅/管理数据库，也不是进程守护器。 |
| **Go 管理后端**（`go/`） | 提供 HTTP/管理能力，并可选连接 C++ 节点：管理用户、服务器、订阅，以及受管节点的鉴权/流量统计。 | 不是 VPN 数据面，不是 Guardian，也不是通用的 `ppp` 启动器。 |
| **Guardian**（`go/guardian/`） | 在本机管理配置的 native `ppp` 子进程、profile、二进制、日志和生命周期操作；提供 REST/SSE、嵌入式 Web UI 和独立 TUI 客户端。 | 不是原生运行时，不是 Go 管理后端，也不是生产级服务管理器或远程控制安全边界的替代品。 |

native 可执行文件由根 CMake 项目构建。`go/` 与 `go/guardian/` 是独立 Go 模块，配置文件、API 和生命周期所有权都不同。这里没有任何页面定义稳定 SDK 或 ABI。

## 按任务选择

| 目标 | 从这里开始 | 配置边界 |
|---|---|---|
| 运行隧道 client、server 或 proxy | [原生快速开始](../docs/getting-started/README_CN.md) 和[配置参考](../docs/reference/CONFIGURATION_CN.md) | 使用 native `ppp` 的配置与 CLI；生命周期见[启动与生命周期](../docs/architecture/STARTUP_AND_LIFECYCLE_CN.md)。 |
| 管理用户、VPN 服务器、订阅或 C++ 节点连接 | [管理后端指南](../docs/guides/MANAGEMENT_BACKEND_CN.md) 和 [Go 核心索引](ppp/debug_CN.md) | 使用根 Go 配置；原生配置中的 `server.backend` 是可选的受管模式连接。 |
| 启停本机 `ppp` 进程、编辑 profile、查看日志或使用本机运维 UI | [Guardian 架构](guardian/ARCHITECTURE_CN.md) | 使用 `guardian.json`（或 `-config` 路径）、实例 profile 和二进制记录；不能与根 Go 管理器的配置互换。 |

## 产品边界

### Native `ppp`

Native `ppp` 负责数据包转发以及 client/server/proxy 运行时。其配置选择角色，并控制传输、隧道密钥、路由、DNS 和其他运行时策略。原生进程可以作为 server 节点连接根 Go 管理器；该控制连接不会让原生进程变成 Guardian，也不会把数据面移入 Go。

精确字段、命令行行为、权限和关闭语义请读原生文档：

- [原生文档索引](../docs/README_CN.md)
- [配置参考](../docs/reference/CONFIGURATION_CN.md)
- [CLI 参考](../docs/reference/CLI_REFERENCE_CN.md)
- [启动与生命周期](../docs/architecture/STARTUP_AND_LIFECYCLE_CN.md)
- [项目接口全景图](../docs/reference/PROJECT_INTERFACE_MAP_CN.md)

### Go 管理后端（`go/`）

根 Go 程序创建 `ppp.NewManagedServer()` 并提供管理面。第一个位置参数选择配置文件；没有参数时，加载器会尝试 `appsettings.json`，再在适用时使用内置默认值。

后端有两种明确模式：

- **受管模式：**同时具备完整的 `database.master` 和 Redis Sentinel 地址及 master 名称。它初始化数据库/缓存支持的用户和服务器，接受 C++ 节点 WebSocket 连接，并运行受管服务器任务。
- **独立模式：**既没有数据库也没有 Redis 配置。它把订阅/管理状态保存到本地（默认 `manager-data.json`），并拒绝 C++ 节点 WebSocket 连接；这不是数据库服务的精简降级版。

数据库/Redis 的部分配置会导致启动错误，不会进入独立模式。管理器的 HTTP/管理路由、存储、节点协议和安全注意事项见[管理后端指南](../docs/guides/MANAGEMENT_BACKEND_CN.md)、[Go 核心索引](ppp/debug_CN.md)和[基础设施索引](io/debug_CN.md)。不要用这些路由证明 Guardian 正在运行，或证明某个原生子进程受到了监督。

### Guardian（`go/guardian/`）

Guardian 是本机进程管理产品。它启动配置的原生二进制，捕获输出，可选执行健康检查和重启，保存 profile/备份，并通过自己的 API 和运维客户端报告状态。它没有根管理器的数据库/Redis 模式，也不能作为原生 `server.backend` 使用。

Guardian 的快速路径、生命周期行为和本机安全边界见 [Guardian 架构](guardian/ARCHITECTURE_CN.md)。[项目接口全景图](../docs/reference/PROJECT_INTERFACE_MAP_CN.md)将 Guardian 与管理器记录为不同的实验性/稳定表面；源码路径是实现证据，不是兼容性承诺。

## 不要混用这些配置

- Native `ppp` 使用原生 JSON/CLI 配置；Guardian 使用 `guardian.json` 和实例启动设置。
- 根 Go 管理器的 `admin.token`、数据库、Redis 和节点连接 key，与 Guardian 生成的 JWT secret 和 API token 无关。
- Guardian 可以启动原生二进制，但不会提供该二进制的隧道配置，也不会变成它的管理后端。
- 原生 `server.backend` 指向根 Go 管理器的 C++ 控制连接，不指向 Guardian 的 REST/SSE API。

三个表面都可能涉及密钥和主机/网络权限。请将各自的监听器、配置文件、profile、环境映射、订阅 URL 和后端 key 保持在预期信任边界内。详细安全事实请以各产品页面为准；本页只是索引，不是部署保证。
