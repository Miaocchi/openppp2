# Guardian：本机 `ppp` 进程监督器

> [Go 产品索引](../debug_CN.md) · [English](ARCHITECTURE.md) · [项目接口全景图](../../docs/reference/PROJECT_INTERFACE_MAP_CN.md)

**Status:** 实验性运维表面

**Type:** 本机进程、profile 和二进制监督器

**Last verified:** 2026-11-20（Guardian 源码）

## Guardian 是什么，不是什么

Guardian 是 `go/guardian` 下独立的 Go 1.22 模块，是围绕 C++ native `ppp` 可执行文件的本机运维产品：启动配置的子进程，捕获日志，管理 profile 和备份，执行可选的健康检查与自动重启，并提供 REST、SSE、嵌入式 Web UI 和独立 TUI 客户端。

Guardian **不**实现 VPN/隧道数据面，不替代原生运行时配置，不通过根 `go/` 后端管理用户/订阅，也不提供根后端的 C++ `server.backend` 连接。它的 API 是实验性的、等同管理员权限；不是稳定公开 SDK，也不是单独可依赖的生产安全边界。

三个产品仍然彼此独立：

| 产品 | 负责 | 不负责 |
|---|---|---|
| Native `ppp` | 隧道数据面、client/server/proxy 运行时、主机网络和运行时生命周期 | Guardian UI/API，或 Go 管理器的用户/订阅状态 |
| 根 Go 管理器（`go/`） | 用户、服务器、订阅、管理 HTTP 路由及可选的 C++ 节点控制连接 | 本机子进程监督和 Guardian profile |
| Guardian（`go/guardian/`） | 本机 `ppp` 进程生命周期、profile、二进制、日志和运维客户端 | 数据包、隧道策略、管理器数据库/Redis，或 `server.backend` 端点 |

## Guardian 快速路径

### 1. 创建或选择 Guardian 配置

使用 `-config /path/to/guardian.json` 启动 Guardian 可执行文件；默认路径是 `guardian.json`。配置文件不存在时，源码默认：

- `listen: 127.0.0.1:18080`；
- 默认启用认证，生成 JWT secret，令牌有效期 24 小时；
- `profiles`、`binaries`、`backups` 路径相对于配置文件；
- 保留 2,000 行日志，并启用备份（默认最多 10 份）。

为每个实例明确配置 native 二进制、工作目录、原生配置路径/参数和必要的环境变量。验证 profile 和二进制后再启用实例。自动发现只是尽力而为且依赖主机；发现结果不证明选中的就是预期二进制。

已有配置中的相对路径相对于配置文件目录解析。启动生成的 secret 会持久化到配置文件，配置文件以 `0600` 写入；应保护配置、profile、备份和实例环境值。

### 2. 打开 Web UI

使用默认监听时，打开 `http://127.0.0.1:18080/`。嵌入式 UI 使用 Guardian API：用配置的/生成的 auth secret 登录，然后使用实例、profile、二进制、日志和设置页面。UI 与 API 同源；Guardian 不输出 CORS 头。

UI 可以启停和重启实例，但这些操作针对的是 native 子进程。除非你明确为实例指定文件/参数，否则它们不会配置根 Go 管理器，也不会改变原生二进制的隧道语义。

### 3. （可选）使用独立 TUI

`cmd/tui/` 下的 TUI 是独立 Go 模块和 API 客户端。传入 Guardian URL 和 token，例如：

```sh
guardian-tui -api http://127.0.0.1:18080 -token <token-from-login>
```

它消费 Guardian REST/SSE，不会嵌入或启动 Guardian 守护进程。默认 API URL 就是上面的回环监听地址。

### 4. 除非明确设置边界，否则保持本机使用

- 本机操作保持默认回环监听并启用认证。
- `listen` 为非回环地址且 `auth.enabled=false` 时，Guardian 拒绝启动。
- Guardian 不配置 TLS。确需远程使用时，应在前方设置经过审查的 TLS/认证/暴露控制边界，并让反向代理保留 `Host` 头。
- `/api/v1/status` 公开。启用认证时，其他受保护 API 路由要求已签发的 Bearer token。SSE 自行校验 token/JWT；中间件豁免不代表已认证流公开。
- 浏览器应使用同源请求；带外域 Origin 的状态变更请求会被拒绝，回环监听还会把 `Host` 校验为回环地址或配置的监听主机。
- 把 API 视为等同管理员权限。实例二进制路径会执行主机进程，HTTP 健康检查端点可以访问任意主机（不跟随重定向）；不要把控制面暴露给不受信任的用户或网络。
- 配置、profile、备份和实例环境映射可能含凭据或原生隧道设置。Profile/备份文件使用 `0600`，文件所在目录以 `0700` 创建。

## 生命周期与持久化边界

`main.go` 解析配置路径、加载配置、创建管理器和 API 服务器、启动启用的实例，并处理 interrupt/SIGTERM 关闭。Guardian 不接管原生隧道状态；它观察并控制子进程。

- 启动实例时使用其二进制、工作目录、参数和环境，随后把输出写入有上限的内存环形缓冲，并发布事件/日志 SSE。
- 停止实例时向进程组发送配置的信号，等待 `stopWaitMs`；仍未退出则强制终止进程组。影响运行中子进程的变更应安排维护窗口。
- 自动重启仅在启用且退出不是由手动停止、更新、删除或关闭造成时执行；重试次数和延迟来自实例配置。停止/删除实例或关闭 Guardian 会取消待执行的重启。
- Guardian 启动时会启动启用的实例。停止或禁用的定义会保留在配置中，直到明确删除；保存设置不会使 Guardian 变成全局服务管理器。
- 已登记的二进制保存在内存中，重启后会按需要再次发现。应明确验证每个实例实际使用的路径和二进制身份。
- Linux systemd 安装/卸载入口存在于 API 中且需要 root；其他平台报告不支持。这是集成入口，不保证生产级服务管理语义。

## 表面索引与详细参考

| 区域 | 职责 | 源码/详情 |
|---|---|---|
| 配置/启动 | 加载默认值、解析路径、持久化生成的 secret、创建管理器并处理关闭 | `config.go`、`main.go`、`guardian.go` |
| 实例管理器 | 子进程生命周期、进程组停止、日志、事件、健康检查和重启策略 | `instance/` |
| Profile | 校验、持久化、备份、恢复和保留 | `profile/` |
| 二进制 | 登记以及依赖主机的发现/版本检测 | `binary/` |
| API | 认证、REST 处理器、SSE、静态 UI 和 systemd 入口 | `api/` |
| Web UI | 嵌入式 Svelte/Vite 运维客户端 | `webui/`、`webui.go` |
| TUI | 独立 REST/SSE 客户端 | `cmd/tui/` |

精确路由组和当前稳定性标签见[项目接口全景图中的 Guardian 部分](../../docs/reference/PROJECT_INTERFACE_MAP_CN.md)。Guardian 启动的原生进程见[原生启动与生命周期](../../docs/architecture/STARTUP_AND_LIFECYCLE_CN.md)和[原生配置参考](../../docs/reference/CONFIGURATION_CN.md)。另一个独立的根 Go 管理后端见[管理后端指南](../../docs/guides/MANAGEMENT_BACKEND_CN.md)和 [Go 核心索引](../ppp/debug_CN.md)。

这些实现路径描述的是本检出版本，不是兼容性承诺、native SDK，或授权将主机级进程控制暴露到远程网络。
