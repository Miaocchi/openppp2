# OpenPPP2 v2.1.10.2

> Status: Release preparation
> Type: Release notes
> Last verified: 2026-11-22
> Parent index: [运维](README_CN.md) · English: [Release notes](RELEASE_NOTES.md)

以下内容描述当前 main 修订版计划发布的 `v2.1.10.2`。精确的标签、必需
CI、打包制品、校验和及发布状态，以 GitHub release 工作流的结果为准。

## 小版本修复

- 修复合并后的 VMUX 接收语义回归测试闭合问题，确保发布测试目标可以编译。

- 加固多路径选路、可靠性 ACK/重传记账、整帧准入、pacing 状态和传输写入记账。
- 增加调度器、重传、接收语义、pacing、SIMD 尾部和集成遥测回归覆盖。
- 隔离三路径上下行实验达到约 89.4% 的合计业务利用率。多路径仍默认关闭，持续 95% 验收目标尚未完成。
## 重点变化

### 多路径仍为实验性功能

本版本合入了第一版多路径传输表面：

- 类型化的 `client.multipath` / `server.multipath` 配置与校验；
- 认证 Noise/AEAD 承载、会话创建/加入控制、路径管理、有界恢复接线，以及
  VMUX 调度和遥测钩子；
- 自适应承载实验，以及桌面端源码运行模式下的配置和路径视图。

多路径**默认关闭**。当前证据仅覆盖隔离的 VMUX TCP 实验场景，不能据此
承诺完整 VPN 流量、UDP relay、P2P、FRP、CDN 部署、故障连续性、全路径恢复、
跨平台行为或带宽聚合的生产支持。受控评估前请阅读[多路径 rollout 边界]
(MULTIPATH_ROLLOUT_CN.md)和[验收记录](../testing/MULTIPATH_ACCEPTANCE_CN.md)。

### 传输、加密与可靠性

- 加固 XTCP RACK/SACK 时间与记分板、SYN Cookie MAC，以及 CUBIC
  `delivered` 计数器回绕处理。
- 提供可选的 XTCP 私有执行域和 direct-upload batching，并增加 AES-CFB
  batching 与 SIMD 变换路径。这些是实现选项，不构成普遍性能保证。
- 增加 VMUX 帧、接收、承载 churn、估计器和协议 fuzz 覆盖；普通单路径行为
  仍是兼容性基线。

### 平台与构建维护

- Linux 打包按兼容、现代、TC、io_uring 和 SIMD 等能力拆分构建 profile。
- 刷新 Android 构建与生命周期代码；Android 包版本为 `2.1.10+10`。Android
  仍是实验性平台表面，仓库内置原生库不构成通用设备发布保证。
- 整理 Guardian 生命周期处理以及英中文档和导航集合。
- Tauri/Svelte 桌面端扩展连接、分流、运行时快照和多路径工作流覆盖，但仍
  没有受支持的安装程序或发布包。

## 兼容性与升级说明

- 现有单路径配置仍是受支持的兼容路径。
- 多路径必须在两端显式启用；同一逻辑会话的所有承载必须到达同一个服务端
  进程。它不提供跨进程会话共享，也不提供服务端重启后的状态恢复。
- `client.policy` 与 `ppp policy` 仍是 v2.1.7 引入的策略接口；不要把
  `client.policy` 与旧版分立分流字段混用。
- 替换运行中的二进制前请阅读[部署与回滚](DEPLOYMENT_CN.md)。保留旧二进制
  和配置，直到新版本通过代理模式与服务 smoke check。

## 验证边界

发布工作流必须先核对精确发布标签与所有必需 CI 工作流，再发布制品。本地
文档检查命令为：

```bash
python -m unittest tests.tooling.test_check_docs tests.tooling.test_docs_site -v
python tools/check_docs.py
python tools/docs_site.py build
```

不要从源码构建成功或实验室证据，推断生产 VPN、CDN、移动设备、桌面安装包
或多路径恢复已经得到支持。
