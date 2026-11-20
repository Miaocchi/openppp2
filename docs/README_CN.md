# 按任务查找文档

> Status: Current
> Type: Navigation
> Last verified: 2026-10-07
> Parent index: [仓库 README](../README_CN.md) · English: [Documentation by Task](README.md)

[English](README.md)

本页是原生 `ppp` 目录的当前任务优先索引。仓库[根 README](../README_CN.md)说明核心与配套
表面的边界并保留原生启动链；本页只负责把读者导向细节，避免重复那段启动顺序。

## 一条首次使用路径

首次运行原生程序时，按以下顺序阅读：

1. [下载安装与首次运行](getting-started/USER_MANUAL_CN.md)
2. [编写配置文件](getting-started/CONFIGURE_CN.md)
3. [配置参考](reference/CONFIGURATION_CN.md)与 [CLI 参考](reference/CLI_REFERENCE_CN.md)
4. [运维清单与故障排查](operations/OPERATIONS_CN.md)

## 按职责选择入口

| 职责或需求 | 当前规范页面 |
|---|---|
| 用户：安装、首次运行和配置 | [快速开始](getting-started/README_CN.md) |
| 用户：路由、DNS、代理、管理或平台行为 | [任务指南](guides/README_CN.md) |
| 运维人员：部署、安全、监控和事故处理 | [部署与运维](operations/README_CN.md) |
| 开发者：运行时、传输、协议、并发和源码 | [当前架构](architecture/README_CN.md) · [开发文档](development/README_CN.md) |
| 参考：配置、CLI、错误、协议和数据格式 | [参考手册](reference/README_CN.md) |
| 接口边界与已知缺口 | [项目接口全景图](reference/PROJECT_INTERFACE_MAP_CN.md) |
| 历史依据、决策、计划或证据 | [历史归档](archive/README_CN.md) · [ADR](adr/README_CN.md) · [设计索引](design/README_CN.md) |

## 常用细分页面

- [路由与 DNS 指南](guides/ROUTING_AND_DNS_CN.md) · [策略 CLI 指南](guides/POLICY_CLI_CN.md) · [English](guides/POLICY_CLI.md)
- [纯代理模式](guides/PROXY_MODE_CN.md) · [管理后端](guides/MANAGEMENT_BACKEND_CN.md)
- [部署说明](operations/DEPLOYMENT_CN.md) · [运维与故障排查](operations/OPERATIONS_CN.md)
- [运行时契约](reference/UI_RUNTIME_CONTRACT_CN.md) · [VMUX 验证](reference/VMUX_VALIDATION_CN.md)
- [原生启动与关闭顺序](architecture/STARTUP_AND_LIFECYCLE_CN.md)——启动链本身保留在[根 README](../README_CN.md#ppp-启动链)。
- [源码阅读、构建、测试和兼容性](development/README_CN.md)
- [本地策略实现证据与 Linux 实机结果](development/README_CN.md) · [Linux 实机报告](testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)

## 范围与状态规则

上方页面是原生 `ppp` 指导的当前规范入口。`go/`、`go/guardian/`、`android/`、`ios/` 和
`desktop/client/` 是由各自清单管理的配套或平台表面；它们的本地文档定义自己的构建和就绪
边界。不要推断核心 `ppp` 构建会生成或验证这些表面；参见[根 README 的边界说明](../README_CN.md#核心与配套表面)。

`archive/`、`adr/` 和 `design/` 保存历史依据、决策、计划和证据。除非当前页面明确说明，
它们不是当前配置或部署说明。依赖页面前请核对 Status、Type、Last verified、语言配对和
父级索引元数据。本页与[英文配对页](README.md)是持续维护的双语导航。
