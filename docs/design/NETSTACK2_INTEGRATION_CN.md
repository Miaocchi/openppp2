# Netstack2 接入 OpenPPP2 设计

> Status: Active / Planned（尚未接入）
> Type: Design
> Last verified: OpenPPP2 d9c2bb4 / Netstack2 c012a9c（dirty worktree），2026-08-17
> Reviewed & revised: 2026-08-18（事实核对修订：调试输出清单、KCC/callback/CMake 行号、target 名确认、dirty 快照内容）
> Parent index: [Design Documents](README.md)

## 1. 文档状态与阅读约定

本文定义把 Netstack2 作为 OpenPPP2 第三种用户态 TCP/IP 栈接入的目标架构、
接口契约、实施工作包和验收门槛。本文是可执行设计，不是完成报告。

截至 `Last verified` 所列快照：

- **Netstack2 尚未接入 OpenPPP2**；OpenPPP2 运行时只有 native 与 lwIP 两条 TCP 路径。
- 本文中“当前事实”表示已从源码核对；“目标设计”表示尚待实现；“待验证”表示在启用前必须用测试或评审关闭的缺口。
- 所有“新增”“修改”“链接”“指标”均为未来时态，不能据此推断仓库已有对应实现。
- 本文不批准热切换、静默回退、UDP 生产启用或非 Linux 平台启用。
- 行号用于定位审计快照，后续代码移动时应按符号名复核，不应把行号当稳定 API。

状态图例：

| 标记 | 含义 | 合入要求 |
|---|---|---|
| 当前事实 | 审计快照中已存在 | 变更前重新核对 |
| 目标设计 | 本文规定的实现方向 | 对应工作包完成并通过验收 |
| 待验证 | 契约矛盾、成熟度或平台缺口 | 未关闭前不得扩大灰度 |
| 非目标 | 本轮明确不做 | 不得借接入之名顺带实现 |

## 2. 执行摘要

OpenPPP2 当前在 TUN/TAP 侧终结客户端 TCP，再通过 Direct、VMUX 或 VPN
transport 建立远端第二条连接。Netstack2 的角色是替换第一条 TCP 终结腿，
不是把客户端 TCP 端到端透传到目标服务器，也不是替换 OpenPPP2 的路由、
DNS、fake-IP、代理选择、VMUX、QUIC 或 VPN transport。

目标方案采用**同进程、静态链接、启动时三选一**：

```text
StackMode::Native | StackMode::Lwip | StackMode::Netstack2
```

首个可运行版本严格限定为：

- Linux；
- IPv4；
- TCP；
- 单 `IPacketQueue`、单 shard；
- 默认关闭；
- 进程启动时选择，运行中不热切换；
- UDP、ICMP 继续 OpenPPP2 现有路径；
- Netstack2 启动或运行失败时显式失败，不在同一进程中静默退回其他栈。

OpenPPP2 提供六个适配组件：

1. `OpenPppNetstack2Runtime`：生命周期、配置和依赖所有权；
2. `OpenPppPacketIo`：Netstack2 `IPacketIo` 实现；
3. `OpenPppPacketQueue`：TAP RX/TX 与 Netstack2 batch API 的有界桥接；
4. `OpenPppSessionFactory`：把 Netstack2 TCP flow 映射到 OpenPPP2 policy 与远端连接；
5. `OpenPppTransportSession`：把 OpenPPP2 Direct/VMUX/VPN 连接映射为 Netstack2 `ITransportSession`；
6. `OpenPppEventSink`：非阻塞指标与诊断事件出口。

这里列出六项是因为 packet I/O facade 与 queue 必须有独立职责；对外仍可把它们
视为一个 packet I/O 子系统。

接入的首要阻断项不是“把库编译进来”，而是先关闭这些契约缺口：

- TCP core 当前没有把 `resolved_destination`、`route_mark`、`dscp` 填入 `TcpOpenRequest`；
- `DatagramOpenResult` 的代码仍是 `void*`，与 ADR 的 typed API 描述漂移；
- UDP `WouldBlock` 没有可恢复的 writable 回调或重试队列；
- runtime 尚未消费 `Capabilities()`；
- metrics、keepalive、部分启动失败回滚尚不完整；
- CMake 无标准 `BUILD_TESTING` gate，Linux TAP 源和测试未正确平台隔离；
- 现有 OpenPPP adapter spike 与 integration smoke 都是 stub/synthetic，不是实际集成；
- 审计快照中的 UDP flow table 带未提交调试输出，UDP 启用前必须清理并评审；
- vendoring 后需完成 GPL-3.0 与 KCC attribution/NOTICE 合规核对。

因此实施按 P0–P6 分阶段，P0 先冻结接口与构建合同，P1 只完成 compile-only
scaffolding，P2 才建立 synthetic packet adapter，P3 才允许真实 OpenPPP TCP
transport 流量，后续再处理多队列、性能、UDP、IPv6 和移动/Windows 平台。

## 3. 目标、非目标与术语

### 3.1 目标

- 保持 native 与 lwIP 行为和默认值不变，增加可显式选择的 Netstack2 模式。
- 在不把 OpenPPP2 policy 下沉进 Netstack2 的前提下复用 fake-IP、DNS、路由和 transport。
- 明确定义 packet buffer、session callback、线程亲和性和停止重试的所有权。
- 为 backpressure、partial send、half-close、callback race、queue full 建立可测试合同。
- 使每个阶段都可独立合入、关闭、回滚，并有明确的验收证据。
- 为后续多 shard、UDP、IPv6 和跨平台留下接口，不提前承诺成熟度。

### 3.2 非目标

- 不把 Netstack2 做成 sidecar 进程。
- 不通过 `dlopen` 或其他运行时动态 ABI 装载。
- 不复制 Netstack2 的零散源文件到 OpenPPP2 目录来规避正式依赖管理。
- 不替换 OpenPPP2 的 DNS interceptor、fake-IP allocator、route selector、VMUX、QUIC 或 VPN transmission。
- 不实现客户端到目标服务器的单条端到端 TCP passthrough。
- 不在一个已运行的 VEthernet 实例内切换 TCP 栈。
- 不在首版启用 UDP、IPv6、多 queue、多 shard、AF_XDP、DPDK、netmap 或 Onload。
- 不因 Netstack2 接入清理无关 OpenPPP2 代码或改变 native/lwIP 语义。
- 不把 synthetic smoke test 描述为真实网络、互操作或生产验证。

### 3.3 术语

| 术语 | 本文含义 |
|---|---|
| TUN/TAP 侧 | 客户端操作系统与 OpenPPP2 用户态栈之间的 IP packet 边界 |
| 第一腿 | 客户端 TCP 与用户态 TCP 栈之间的 TCP 连接 |
| 第二腿 | OpenPPP2 从 policy 解析结果到 Direct/VMUX/VPN transport 的连接 |
| original destination | 客户端原始访问的 IP:port；fake-IP 情况下仍保留该值 |
| resolved destination | 经 DNS/fake-IP/route policy 后真正用于第二腿的 endpoint |
| owner shard | 唯一允许直接修改对应 Netstack2 flow/PCB 状态的 shard 线程 |
| success-prefix | batch API 返回成功数量 `n` 时，仅前 `n` 项所有权被接收方取得 |
| quiesce | 禁止新输入并保证已注册 callback 不再执行或已完成 |
| feature gate | 编译时和运行时都必须满足的显式启用条件 |

## 4. 仓库、版本与审计边界

### 4.1 OpenPPP2 基线

- 审计 commit：`d9c2bb4bac54edacd4ad06f8eab6e4504933ff6b`；
- 审计 describe：`v2.1.6-11-gd9c2bb4`；
- 顶层 `LICENSE` 是 GNU GPL version 3；
- 本文只新增设计文档，不代表代码或构建系统已变化。

### 4.2 Netstack2 基线

Netstack2 的源码 API 版本由顶层 CMake 与 README 标为 **v0.3.0**。审计 commit
是 `c012a9c63bab726fd05c9e7d97314fd9f9a0995c`；`git describe` 的
`v0.2.0-68-gc012a9c-dirty` 只表示最近 tag 与工作树状态，不能替代 API 版本。

审计时该仓库是 dirty worktree，至少包含 `.gitignore`、`CMakeLists.txt`、bench
文件和 `src/udp/flow_table.cpp` 的修改，以及未跟踪 CI/bench 文件。该未提交快照
还包含 `src/core/shard.cpp/.h` 的 UDP 分片路由修复（非分片报文不再进 reassembler、
`HandleReassembledUdp` 增加 `now_ms` 参数），并且已**删除** `src/udp/flow_table.cpp`
的 `[flowdbg]` 调试输出——该输出位于 commit `c012a9c`（flow_table.cpp:39），不在
未提交工作树中。本文的事实只绑定上述 commit 加当时工作树内容；实施前必须在干净、
已 pin 的 revision 上重新核对。

### 4.3 路径映射与依赖规则

本次外部审计源位于 `/home/Netstack2`。未来接入后，文中 Netstack2 路径统一映射为：

```text
审计源 include/tcpip2/...  -> common/libtcpip2/include/tcpip2/...
审计源 src/...             -> common/libtcpip2/src/...
审计源 tests/...           -> common/libtcpip2/tests/...
审计源 docs/...            -> common/libtcpip2/docs/...
```

除本节记录审计来源外，设计、构建和测试不得依赖机器绝对路径。生产构建不得通过
`-I`、`add_subdirectory` 或脚本引用外部工作目录；不得依赖开发者本地 checkout；
不得把 floating branch 当版本锁。

本文不执行也不授权任何 submodule 操作。依赖落地方式见第 15 节。

## 5. OpenPPP2 当前数据路径审计

### 5.1 当前总体路径

```text
客户端内核
   |
   | IPv4 packet
   v
ITap async_read_some
   |
   v
VEthernet TAP callback
   |
   +--> OnPacketInput / NAT / policy hook
   |
   v
VEthernet::PacketInput
   |
   +--> TCP + lwip_ ------> lwip::netstack::input
   |
   +--> TCP + !lwip_ -----> VNetstack::Input
   |
   +--> UDP/ICMP ----------> fragment/policy/current dispatch
                              |
                              v
VEthernetNetworkTcpipStack -> ResolveDestination
                              |
                              v
VEthernetNetworkTcpipConnection
   |
   +--> Direct/rinetd
   +--> VMUX
   +--> VPN transmission/transport

回程：远端 connection -> 用户态 TCP 栈 -> VEthernet::Output -> ITap::Output -> 客户端内核
```

这是一种双重 TCP 终结架构：用户态栈终结第一腿，OpenPPP2 connection/transport
建立第二腿。Netstack2 应只替换图中的 lwIP/native TCP 栈及其第一腿 session
适配，不改变第二腿 policy 和 transport。

### 5.2 TAP RX buffer 的关键所有权

`ppp/tap/ITap.h:40-48` 的 `PacketInputEventArgs` 只给出 `void* Packet` 与
`int PacketLength`。`ppp/tap/ITap.cpp:463-481` 把 `async_read_some` 直接读进
`_packet`，调用 callback 后立即重新 arm；`ppp/tap/ITap.cpp:489-495` 同步调用
handler。`ppp/tap/ITap.h:186-198` 还暴露可复用 read buffer，类内 fallback
位于 `ppp/tap/ITap.h:243-249`。

因此 callback 返回后不得保存 `Packet` 裸指针。目标 adapter 必须在 callback
作用域内从 Netstack2 pool 取得 `BufferLease`，复制 packet，然后才把 lease
发布到有界 RX queue。禁止把 ITap buffer 伪装成 zero-copy lease。

### 5.3 TAP callback 与 TCP 分叉

`ppp/ethernet/VEthernet.cpp:449-501` 安装的 callback 解析 IPv4；MTA/SSMT 模式
会复制进 pbuf 再 post，非 MTA 模式同步调用内部 `PacketInput`。
`ppp/ethernet/VEthernet.cpp:522-524` 安装 TAP 与 fragment callbacks。

TCP 的核心分叉在 `ppp/ethernet/VEthernet.cpp:606-630`：

- `lwip_ == true` 时调用 `lwip::netstack::input`；
- 否则解析 TCP 后调用 `VNetstack::Input`；
- `ppp/ethernet/VEthernet.cpp:636-649` 的 UDP/ICMP 仍走现有 fragment/policy 路径。

目标插入点必须位于 `OnPacketInput` 等现有 policy hook 之后，并仅截获 TCP。
可在上述分叉加入第三分支，但更可维护的实现是在 TAP callback/`PacketInput`
边界把经 policy 允许的 TCP packet 注入 `OpenPppPacketQueue`。无论采用哪种代码
形态，**都不能绕过现有入口 policy，也不能截获首版 UDP/ICMP**。

### 5.4 TX 路径

`ppp/ethernet/VEthernet.cpp:514-518` 的 lwIP 输出 callback 最终调用 `Output`；
`ppp/ethernet/VEthernet.cpp:997-1016` 的 raw packet 输出最终调用 `tap->Output`。
这也是 Netstack2 egress 回接点。

`ppp/tap/ITap.cpp:535-561` 把 TX 放入 `_write_queue` 后在 strand 异步写，
`ppp/tap/ITap.cpp:568-607` drain queue。`ppp/tap/ITap.cpp:616-632` 的 raw
`Output` 会先复制；`ppp/tap/ITap.cpp:641-644` 的 shared-buffer 路径可持有 owner。
目标 adapter 应优先使用能持有 owner 的路径，或在同步复制完成后立即确认
Netstack2 lease。若未来增加真正异步提交，必须把 lease 持有到 write completion。

### 5.5 policy 与第二腿

- `ppp/app/client/VEthernetNetworkSwitcher.cpp:467-472` 当前 `NewNetstack()` 总是创建 `VEthernetNetworkTcpipStack`；
- `ppp/app/client/VEthernetNetworkSwitcher.cpp:522-535` 安装 packet policy hooks；
- `ppp/app/client/VEthernetNetworkSwitcher.cpp:662-671` 暴露 `ResolveDestination`；
- `ppp/app/client/VEthernetNetworkTcpipStack.cpp:33-143` 在 TCP flow 建立后解析目标并创建 connection；
- `ppp/app/client/routing/ResolvedDestination.h:10-17` 同时保留 original、connect endpoint、hostname、action 及 fake/resolved flags；
- `ppp/app/client/dns/DnsInterceptor.cpp:797-835` 实现 fake-IP 与 human route 解析；
- `ppp/app/client/routing/TcpRoutingSelector.cpp:5-20` 定义 Direct、Proxy、LegacyAuto、Reject；
- `ppp/app/client/VEthernetNetworkTcpipConnection.cpp:174-321` 实现 Direct/rinetd、VMUX 和 VPN transmission 路径；
- `ppp/app/client/ClientPacketDispatchHandler.cpp:65-115` 的 NAT hook 先于 stack，`:190-200` 处理 UDP/ICMP，`:257-365` 承担 UDP DNS/fake-IP/route policy。

这些职责必须留在 OpenPPP2。`OpenPppSessionFactory` 应复用相同 resolver 与
connection 建立能力，而不是在 Netstack2 内复制一套 policy。

### 5.6 当前启动与停止顺序

`ppp/app/client/ClientConnectionOpener.cpp:69-73` 先打开 VEthernet，`:118-136`
后创建 exchanger。真实 Netstack2 factory 需要 exchanger ready，因此 P3 前必须
增加 readiness barrier：先让 exchanger 建立并可创建第二腿，再启用 TAP RX 和
Netstack2 ingress。否则启动窗口中的 SYN 会落入尚不可用的 factory。

`ppp/ethernet/VEthernet.cpp:90-120` 当前析构/释放 fragment、netstack、lwIP
callbacks、TAP callback、TAP、SSMT 和 timer。`ppp/app/client/ClientConnectionTeardown.cpp:52-94`
与 `ppp/app/runtime/RuntimeStopPipeline.h:29-35` 当前采用 stop input → DNS →
exchanger → route。Netstack2 接入必须把 session quiesce 与 stack drain 插入
停止流水线，不能在 callback 仍可能执行时先销毁 factory/exchanger。

## 6. Netstack2 当前能力与成熟度

### 6.1 已有核心能力

未来路径 `common/libtcpip2/include/tcpip2/netstack.h:36-87` 提供 `Netstack2`
facade、`Start(RuntimeDependencies)`、可重试的 `Stop()`、config 和 runtime 查询。

`common/libtcpip2/include/tcpip2/runtime_deps.h:23-50` 规定所有依赖指针均为
non-owning，且 `packet_io`、`session_factory` 必需；调用者必须让依赖活到
`Stop()` 完整成功。

核心已有：

- move-only `BufferLease`、pool owner 和跨线程 return queue；
- `IPacketIo` / `IPacketQueue` batch 收发、停止和 drain 合同；
- TCP session factory 与 transport session 抽象；
- shard owner thread、有界事件预算、control message 和 timer/egress lane；
- TCP flow、拥塞控制、remote callback 转 owner shard；
- partial remote data tail 与接收低水位恢复；
- FIN、close、abort 和 callback 清理路径；
- 保留 runtime 的 timeout/drain-failed 停止结果。

### 6.2 成熟度与缺口矩阵

| 领域 | 当前事实 | 缺口/风险 | 启用门槛 |
|---|---|---|---|
| TCP engine | flow、handshake、shard、CC 实现较完整 | 尚无真实 OpenPPP transport interop | P3 e2e 与故障测试 |
| Packet I/O | Null 与 Linux TAP backend | 无 OpenPPP backend；无 AF_XDP/DPDK/netmap | P2 adapter；其他后端不在首版 |
| Buffer | move-only lease 与 pool | outstanding lease 不可强制释放 | Stop retry 与泄漏测试 |
| Session metadata | API 有 original/resolved/mark/dscp | TCP core 未填 resolved/mark/dscp | P0 冻结并加 contract test |
| Backpressure | TCP 有 partial/WouldBlock 与 writable | OpenPPP 映射尚不存在 | P2/P3 双向压力测试 |
| Half-close | 有 FIN/ShutdownWrite/CloseWait | 与 OpenPPP transport 语义未验证 | P0 合同、P3 e2e |
| UDP | flow table 与 datagram session | opaque pointer、无 writable 恢复、dirty debug 输出 | P5 前全部关闭 |
| Capabilities | API 声明 link/poll/offload/async/zero-copy | runtime 未消费 `Capabilities()` | P0 明确协商/拒绝规则 |
| Metrics | 每秒 snapshot/event sink | 多个字节/包字段硬编码为 0 | P4 前补全并核验 |
| Keepalive | config 传播到 flow | 未见 probe 调度 | 不宣称支持；P4 或独立修复 |
| Shutdown | StopRx、DrainTx、OutstandingTx、retry | 部分 shard 启动失败 rollback 待验证 | P0 fault injection |
| Build | C++17、Threads、unit/integration tests | testing 与 Linux TAP 无平台 gate | vendoring 前 P0 修复 |
| Adapter tests | spike 与 synthetic smoke | stub queue/factory，不是真实 OpenPPP | 不计入生产验收 |
| 平台 | Linux TAP 实现 | 整库当前 Linux-oriented | 首版 Linux gate |
| License | GPL-3.0；KCC 有来源注释 | 无集中 NOTICE，需核对 attribution | 发布前合规签字 |

### 6.3 关键 API 事实

`common/libtcpip2/include/tcpip2/packet_io.h:15-26` 定义 success-prefix
所有权；`:49-117` 定义 `RecvBatch`、`SendBatch`、`QueueId`、`SetBufferPool`、
`StopRx`、`DrainTx`、`OutstandingTx`、`SetRecvHandler`；`:119-134` 定义
`QueueCount`、`OpenQueue`、`Capabilities`。

`common/libtcpip2/include/tcpip2/buffer.h:11-29,85-116,184-270` 的 lease
只能 move，释放回 owner pool；跨线程通过 return queue 回收。停止时不能为了
“清零”计数而强制释放仍被异步 I/O 持有的 lease。

`common/libtcpip2/include/tcpip2/session_factory.h:48-94` 的 TCP/UDP request
包含 flow、source、original/resolved destination、route mark、DSCP，但
`common/libtcpip2/src/tcp/handshake.cpp:825-838` 当前 TCP open 只填 flow、source、
original，resolved 留为默认值；UDP 在 `common/libtcpip2/src/udp/flow_table.cpp:101-110`
只把 resolved 设为 original。adapter 不能把这些字段误当已解析 policy 结果。

推荐 P0 合同是：**OpenPPP factory 以 original destination 为权威输入，在
OpenPPP owner/executor 上调用现有 resolver，生成 resolved destination 与 route
action；Netstack2 不解释 fake-IP。** 同时修正 Netstack2 core，让 request 明确
表达字段有效性，避免默认 `0.0.0.0` 被误用。若团队选择在 packet ingress 前完成
policy resolution，则必须增加独立 resolver hook 与异步结果合同；不得只补写
字段而没有数据来源。

`common/libtcpip2/include/tcpip2/transport_session.h:36-131` 定义
`SessionError`、send/receive status、临时 `BufferView`、`TrySend`、
`ResumeReceive`、`ShutdownWrite`、`Abort` 和 Writable/Data/Closed callbacks。
`BufferView` 只在 `TrySend` 调用期间有效；OpenPPP 若异步发送，必须复制或接管
独立 owner。callback 设为 `nullptr` 必须同步 quiesce，这是安全销毁的前提合同。

### 6.4 当前实现不得过度承诺之处

- `common/libtcpip2/src/CMakeLists.txt:6-45` 只编译 Null 和 Linux TAP backend；
- `common/libtcpip2/src/io/tap_io.cpp` 使用 Linux TUN/TAP 与 poll；
- README 中 AF_XDP、DPDK、netmap 等应视为路线图，不是已有能力；
- `Capabilities()` 当前未被 runtime 消费，不能声称已完成 offload 协商；
- `common/libtcpip2/src/core/shard.cpp:496-513` 的若干 metrics 仍为零；
- `common/libtcpip2/src/tcp/handshake.h:103-108` 只传播 keepalive 配置，未形成 probe scheduler；
- `common/libtcpip2/tests/unit/openppp_adapter_spike_test.cpp:1-157` 明确使用 no-op/stub；
- `common/libtcpip2/tests/integration/openppp2_smoke_test.cpp:1-17` 使用 Null I/O 与 FakeSession；
- 这些测试能验证 core contract，但不能证明与 OpenPPP2 实际代码兼容。

## 7. 架构决策

### 7.1 ADR-N2-01：同进程静态链接

**决定**：把 pin 后的 Netstack2 源作为 OpenPPP2 构建树内静态目标，链接到
`openppp2_lib`。

理由：

- packet、lease、callback 与 shard 之间是低延迟、强所有权接口；
- sidecar 会引入 IPC framing、额外复制、独立故障域与更复杂的 backpressure；
- `dlopen` 要稳定 C ABI，而当前 API 是 C++ v0.3.0，无法提供合理 ABI 保证；
- 复制若干源码会丢失上游提交边界、测试和许可证归属。

### 7.2 ADR-N2-02：保留三种栈，启动时选择

**决定**：用 `StackMode` enum 替代跨层传播的 `bool Lwip`，但 native/lwIP
实现和默认行为保持不变。单个进程实例只能选择一种 TCP 栈。

不支持运行时热切换，因为已有 flow、TCP sequence/window、buffer lease、
session callback 和 exchanger 都绑定原栈。回滚方式是停止进程并以旧模式重启。

### 7.3 ADR-N2-03：policy 属于 OpenPPP2

**决定**：Netstack2 只管理第一腿 TCP 状态；`OpenPppSessionFactory` 调用
OpenPPP2 resolver 和 connection factory。Direct/Proxy/Reject、fake-IP、hostname、
route mark、VMUX、QUIC/VPN 选择均由 OpenPPP2 决定。

### 7.4 ADR-N2-04：首版单 queue/shard

**决定**：P2/P3 固定 `QueueCount()==1`、shard count `1`。先证明契约，再扩展
RSS/flow affinity。若配置请求大于 1，首版启动失败并给出明确错误，不静默降级。

### 7.5 ADR-N2-05：失败显式化

- 非 Linux 请求 Netstack2：启动失败；
- IPv6 packet 进入首版 Netstack2：按明确 counter 记录并走既定 reject/drop，不能误送 native；
- Netstack2 Start 失败：VEthernet open 回滚并返回错误；
- 运行中 fatal：停止接收新输入，触发受控 teardown；
- 不在同一进程中自动切 native/lwIP，因为这会让已有 flow 状态不可解释。

## 8. 目标架构与时序

### 8.1 组件图

```text
                       OpenPPP2 control plane
        config/parser ---- StackMode ---- startup/readiness barrier
             |                                 |
             v                                 v
+-------------------------- VEthernet --------------------------------+
| OnPacketInput/NAT/policy                                          |
|     | TCP + Netstack2                                               |
|     v                                                               |
| OpenPppNetstack2Runtime                                             |
|     |                                                               |
|     +-- OpenPppPacketIo -- OpenPppPacketQueue -- ITap RX/TX          |
|     |                                                               |
|     +-- Netstack2 facade/runtime                                    |
|     |      +-- owner shard / TCP PCB / timers / buffer pool          |
|     |                                                               |
|     +-- OpenPppSessionFactory -- ResolveDestination                  |
|                  |                                                   |
|                  +-- OpenPppTransportSession                         |
|                         +-- Direct                                   |
|                         +-- VMUX                                     |
|                         +-- VPN transport                            |
|     +-- OpenPppEventSink -- bounded telemetry queue                  |
+----------------------------------------------------------------------+
```

### 8.2 上行时序：客户端 packet 到远端

```text
ITap callback
  -> validate IPv4/TCP length
  -> OnPacketInput/NAT/current ingress policy
  -> BufferPool::Allocate
  -> memcpy(ITap reusable buffer -> BufferLease)
  -> bounded OpenPppPacketQueue RX enqueue
  -> signal/wake Netstack2 queue handler
  -> owner shard parses TCP and advances flow
  -> on first open, OpenPppSessionFactory::OpenTcp
  -> post OpenPPP executor if resolver/connection API requires it
  -> ResolveDestination(original)
  -> Reject OR create Direct/VMUX/VPN second leg
  -> callback open result to Netstack2 owner shard
  -> Netstack2 calls OpenPppTransportSession::TrySend(payload)
  -> adapter copies payload if OpenPPP async API cannot retain caller view
  -> OpenPPP remote write queue applies backpressure
```

### 8.3 下行时序：远端数据到客户端

```text
Direct/VMUX/VPN callback
  -> OpenPppTransportSession DataCallback
  -> enqueue immutable/copy-owned message to Netstack2 control lane
  -> owner shard validates flow generation and state
  -> TCP segmentation/ACK/window logic
  -> BufferLease egress batch
  -> OpenPppPacketQueue::SendBatch
  -> VEthernet::Output / ITap::Output(shared owner)
  -> async write completion releases owner
  -> lease returns to Netstack2 pool
```

远端 callback 绝不能直接调用 Netstack2 内部 flow 或 PCB。当前 Netstack2 已把
remote callback 封为 shard message（未来路径
`common/libtcpip2/src/tcp/handshake.cpp:1013-1060`）；adapter 必须保持这条边界，
并携带 flow id/generation，丢弃晚到 callback。

### 8.4 线程与阻塞规则

| 上下文 | 可以做 | 禁止做 |
|---|---|---|
| ITap callback | 有界校验、pool allocate、一次复制、enqueue、wake | DNS、连接、阻塞锁、无限重试 |
| Netstack2 owner shard | flow/PCB/timer 状态变更、短小 batch | 阻塞网络 I/O、等待 OpenPPP executor |
| OpenPPP executor/strand | resolver、connection 建立、OpenPPP queue 操作 | 直接修改 Netstack2 flow |
| transport callback | 构造有所有权的消息并 post | 持有临时 view、同步重入 flow |
| EventSink callback | 原子计数、有界采样 enqueue | 格式化大日志、磁盘/网络输出、阻塞锁 |

## 9. Adapter 目录与类设计

### 9.1 预计新增文件

```text
ppp/ethernet/netstack2/
  OpenPppNetstack2Runtime.h
  OpenPppNetstack2Runtime.cpp
  OpenPppPacketIo.h
  OpenPppPacketIo.cpp
  OpenPppSessionFactory.h
  OpenPppSessionFactory.cpp
  OpenPppTransportSession.h
  OpenPppTransportSession.cpp
  OpenPppEventSink.h
  OpenPppEventSink.cpp
```

`OpenPppPacketQueue` 可作为 `OpenPppPacketIo` 文件中的私有/内部类型，避免首版
扩大 public surface；若测试需要独立注入，再拆文件，不提前抽象。

### 9.2 `OpenPppNetstack2Runtime`

职责：

- 持有 `Netstack2` facade；
- 持有且按声明逆序销毁 packet I/O、session factory、event sink；
- 把 OpenPPP config 转成 Netstack2 config；
- 实现 `Constructed -> Starting -> Running -> Stopping -> Stopped` 状态机；
- 管理 exchanger readiness barrier；
- 统一 Start rollback、Stop retry、错误文本与 runtime status；
- 只在 `Running` 接受 TAP RX。

建议接口伪代码：

```cpp
class OpenPppNetstack2Runtime final {
public:
    bool Start(const Netstack2Options& options,
               const std::shared_ptr<ITap>& tap,
               const std::shared_ptr<VEthernetNetworkSwitcher>& switcher,
               std::string& error);
    tcpip2::StopResult Stop(const tcpip2::StopOptions& options) noexcept;
    bool OnTapPacket(const void* packet, int packet_length) noexcept;
    RuntimeState State() const noexcept;
};
```

实际签名应匹配项目现有 pointer/allocator 习惯，但不得把 non-owning
`RuntimeDependencies` 指向栈上临时对象。

### 9.3 `OpenPppPacketIo` / `OpenPppPacketQueue`

职责：

- 首版报告一个 queue；
- `OpenQueue(0)` 返回稳定对象；其他 id 返回明确错误；
- 接受 Netstack2 注入的 buffer pool；
- 把 TAP callback 的 packet 复制成 lease 并排入 RX；
- 实现有界 `RecvBatch`，不在 shard 上阻塞；
- 将 `SendBatch` 的成功前缀提交给 VEthernet/ITap；
- 正确维护 outstanding TX；
- `StopRx` 同步阻止新 enqueue，并 quiesce handler；
- `DrainTx` 等待已提交 TX 或按 deadline 返回失败。

### 9.4 `OpenPppSessionFactory`

职责：

- 以 request 的 `original_destination` 为权威；
- 映射 source endpoint、flow id、DSCP/route metadata；
- 调用 `ResolveDestination`，保留 original 与 resolved；
- 对 Reject 返回稳定、可观测的 open error；
- 复用 `VEthernetNetworkTcpipConnection` 或抽取其最小 factory，不复制 transport policy；
- 在 exchanger ready 前拒绝/延迟 open；最终选择必须由 P0 合同固定；
- open completion 总是 post 回 Netstack2 owner shard；
- Stop 时拒绝新 open、取消 pending open、等待 callback quiesce。

### 9.5 `OpenPppTransportSession`

职责：

- 包装一条 OpenPPP 第二腿 connection；
- 把 OpenPPP write 结果映射为 `SendStatus` 与 consumed bytes；
- `WouldBlock` 后只在真实可写时触发 WritableCallback；
- 把远端 data 作为有所有权消息投递，不暴露临时 buffer；
- 映射 EOF、reset、timeout、policy reject 和 local abort；
- 实现 `ShutdownWrite`，验证 Direct、VMUX、VPN 三类 transport 的半关闭差异；
- callback 设空时同步保证旧 callback 不再执行；
- 析构不隐式阻塞无限时间。

### 9.6 `OpenPppEventSink`

职责：

- 把 Netstack2 event 转为 OpenPPP counters/structured diagnostics；
- shard callback 只做原子操作或有界 enqueue；
- 对高频 packet/flow 事件采样或聚合；
- 不记录 payload、DNS 明文以外的敏感流内容或完整用户数据；
- drop telemetry 时增加 `netstack2.telemetry.dropped`，不得反压数据平面。

## 10. Packet I/O 合同

### 10.1 RX：必须复制 ITap reusable buffer

伪代码：

```cpp
bool OpenPppPacketQueue::EnqueueTapPacket(const void* data, std::size_t size) {
    if (!rx_accepting_ || size == 0 || size > max_packet_size_) {
        CountRxReject(size);
        return false;
    }
    BufferLease lease = pool_->Allocate(size);
    if (!lease) {
        CountRxPoolEmpty();
        return false;
    }
    std::memcpy(lease.data(), data, size);
    lease.set_size(size);
    if (!rx_ring_.try_push(std::move(lease))) {
        CountRxQueueFull();
        return false; // local lease 自动回 pool
    }
    NotifyRecvHandlerOnce();
    return true;
}
```

要求：

- queue 必须有固定容量和可配置上限；
- pool empty 与 queue full 分开计数；
- wake 应合并，避免每 packet 唤醒；
- callback 返回前完成复制；
- enqueue 失败时释放 lease，不保存 TAP pointer；
- oversized、non-IPv4、non-TCP 和 malformed packet 有不同 counter；
- 首版非 TCP packet 不进入此 queue，而是继续现有 OpenPPP 路径。

### 10.2 `RecvBatch`

- 只从 RX ring move 最多 `max_count` 个 lease；
- 空队列立即返回 0；
- 不等待 TAP、condition variable 或网络 I/O；
- handler 只表示“可能有数据”，消费者必须容忍虚假/合并 wake；
- handler replacement/null 必须与 `StopRx` 建立同步关系；
- queue 停止后不得重新 arm callback。

### 10.3 `SendBatch` 与 success-prefix

```text
输入 leases: [0, 1, 2, 3]
返回 2:       [0, 1] 已由 adapter 接管；[2, 3] 仍归调用者
```

实现不得在第 2 项提交失败后继续接管第 3 项。每项只有以下两种安全路径：

1. `VEthernet::Output` 在调用内复制：调用成功后可立即消费 lease；
2. shared owner 异步写：构造持有 lease/owner 的 shared buffer，write completion 后释放。

如果 ITap queue 满或返回 WouldBlock：

- `SendBatch` 返回已经成功提交的前缀长度；
- 未提交 lease 仍由 Netstack2 保有，稍后重试；
- adapter 增加 `netstack2.tx.would_block`；
- 可写通知不得丢失唤醒；
- 禁止 busy loop，也禁止为了“成功”而无界扩张 OpenPPP write queue。

### 10.4 Outstanding 与 shutdown

`OutstandingTx()` 只统计 adapter 已接管但尚未完成/释放的 TX。停止序列：

1. `StopRx()`；
2. 清除 TAP input callback 并等待 callback quiesce；
3. Netstack2 停止生成新 egress；
4. `DrainTx(deadline)`；
5. 校验 `OutstandingTx()==0`；
6. 成功后才能销毁 pool/queue/tap owner。

超时不得伪造 outstanding 为 0，不得强制回收 lease，不得释放仍被 kernel/strand
引用的 shared owner。返回 TimedOut/DrainFailed，并保留 runtime 和依赖供重试。

### 10.5 Capabilities

P0 必须定义 runtime 真正消费能力的规则。首版 OpenPPP backend 只声明已实测能力：

- link type：IPv4 L3 packet；
- polling：callback/wake bridge，而非 busy polling；
- checksum/GSO/TSO：默认不声明 offload；
- async TX：仅当 completion/owner 合同完整时声明；
- zero-copy RX：明确 false；
- zero-copy TX：首版不承诺。

未知 capability 必须保守关闭。配置要求不支持能力时 Start 失败并输出具体字段。

## 11. Session 与 policy 合同

### 11.1 `OpenTcp` 字段映射

| Netstack2 字段 | OpenPPP2 来源 | 合同 |
|---|---|---|
| `flow_id` | Netstack2 flow | 全生命周期唯一并带 generation 防 ABA |
| `source` | 客户端 SYN source | 保留客户端 endpoint，不与代理 endpoint 混淆 |
| `original_destination` | packet destination | fake-IP 解析前的原始值 |
| `resolved_destination` | OpenPPP resolver | resolver 成功后才有效 |
| hostname | `ResolvedDestination` | 若 fake-IP/human route 可解析则保留 |
| route action/mark | TcpRoutingSelector/policy | Direct/Proxy/Reject 显式映射 |
| DSCP | ingress packet/config | 未实现时标记 absent，不伪造 0 的语义 |

由于当前 Netstack2 core 没有填齐后四类 metadata，P0 必须选择并测试一种明确模型。
推荐 asynchronous factory-resolution 模型：request 只要求 original/source/flow 有效，
factory 解析后在其 OpenPPP connection context 中保存 resolved/policy metadata；若
Netstack2 需要这些字段做调度，则通过 typed open result 返回，而不是依赖默认值。

### 11.2 fake-IP、DNS 与路由

factory 流程：

```text
original endpoint
  -> DnsInterceptor/fake-IP lookup
  -> hostname + connect endpoint
  -> TcpRoutingSelector
      -> Reject: open error, no remote session
      -> Direct: direct/rinetd connection
      -> Proxy/LegacyAuto: VMUX or configured VPN transmission
  -> OpenPppTransportSession
```

必须复用当前 `ResolveDestination` 结果，确保：

- fake-IP 未解析时与 native 路径同样 reject；
- `original_endpoint` 不被 connect endpoint 覆盖；
- route action 与 DNS policy 顺序一致；
- Direct 与 Proxy 使用相同配置语义；
- VMUX/QUIC/VPN 的选择仍由 OpenPPP2 控制；
- Netstack2 不读取 OpenPPP config JSON，不自行查 DNS。

### 11.3 `TrySend` 与 backpressure

`BufferView` 在 `TrySend` 返回后失效。adapter 可：

- 在 OpenPPP write API 同步复制时直接传入；或
- 复制到 OpenPPP-owned shared buffer 再异步提交。

返回规则：

| OpenPPP 结果 | Netstack2 映射 |
|---|---|
| 全部同步接收 | `Ok`, consumed = input size |
| 只接收前缀 | `Ok/Partial`（按 API 枚举），consumed = prefix |
| queue 满且消费 0 | `WouldBlock`, consumed = 0 |
| 已关闭 | `Closed`/对应 error，consumed = 0 |
| 永久错误 | `Error`，随后一次 ClosedCallback |

partial tail 仍归 Netstack2，不能重复复制已消费前缀。`WouldBlock` 后 Netstack2
停止该 session 的 remote send，直到 adapter 确认 OpenPPP queue 可写并触发
WritableCallback。callback 必须 edge-safe：注册与变为可写并发时不能丢 wake。

### 11.4 远端 Data 与 `ResumeReceive`

当 Netstack2 返回只消费部分 remote data 时，adapter 保留 tail；当窗口/队列不足，
暂停 OpenPPP connection read。只有收到 `ResumeReceive()` 才恢复。不得继续读取并
无界缓存，也不得静默丢 remote TCP bytes。

Netstack2 当前实现会把 remote data 转 shard message，并在低水位调用
`ResumeReceive`（未来路径 `common/libtcpip2/src/tcp/handshake.cpp:1228-1271`）。
P3 必须验证 OpenPPP Direct、VMUX、VPN read loop 都能暂停和恢复。

### 11.5 close、error 与 half-close

客户端 FIN 当前会让 Netstack2 调 `ShutdownWrite` 并进入 close-wait 相关状态；
未来路径 `common/libtcpip2/src/tcp/handshake.cpp:527-565,594-600` 显示其后对 peer
data 有状态限制。接入前必须用真实 transport 验证：

- 客户端 `shutdown(SHUT_WR)` 后仍能读远端响应；
- 远端 EOF 只关闭下行方向，已排队数据仍发送到客户端；
- simultaneous close；
- RST、connect timeout、DNS reject、transport reset；
- VMUX logical stream close 与底层 exchanger close 的区别；
- `ShutdownWrite` 不应错误关闭共享 VMUX/QUIC 物理连接。

任何语义不匹配都应在 typed adapter state machine 中修复，不能用 `Abort()` 替代
所有 half-close。

### 11.6 callback quiesce

销毁前按顺序：

1. factory 拒绝新 open；
2. 每个 session 停止从 OpenPPP 接收新事件；
3. Writable/Data/Closed callback 设为 `nullptr`；
4. 设空操作同步等待正在执行的旧 callback 完成；
5. owner shard 处理已入队且 generation 有效的消息；
6. flow teardown；
7. session/factory/exchanger 才可释放。

未来路径 `common/libtcpip2/src/tcp/handshake.cpp:673-693` 已在删除前清三类 callback，
`:1199-1213` 会 deactivate gate 后清 PCB；adapter 必须提供与此假设一致的同步保证。

## 12. 生命周期、启动屏障与回滚

### 12.1 状态机

```text
Constructed
    |
    | Start
    v
Starting --failure--> Stopping --rollback complete--> Stopped
    |
    | dependencies ready + queues open + shards running + exchanger ready
    v
Running
    |
    | Stop/fatal
    v
Stopping --timeout/drain failed--> Stopping (保留对象，可重试 Stop)
    |
    | callbacks quiesced + TX drained + outstanding=0
    v
Stopped
```

非法转换必须返回错误；Start 不能与 Stop 并发；Stop 必须幂等且可重试。

### 12.2 Start 顺序

推荐 P3 稳定顺序：

1. parse/validate `StackMode` 与 Netstack2 options；
2. 构造 TAP，但不启用 Netstack2 RX callback；
3. 构造 event sink、session factory、packet I/O；
4. 构造 Netstack2 facade 和 non-owning dependencies；
5. `Netstack2::Start`，open queue/pool/shard；
6. 建立 exchanger/transport readiness；
7. 安装 TAP callback；
8. 原子切到 `Running`；
9. 开始接收 SYN。

如果现有整体启动框架不能交换第 5/6 步，则 factory 可以先构造但 gate 为 not ready，
同时 TAP RX 必须等 readiness 后启用。不能靠“收到 SYN 后很快 ready”消除竞态。

### 12.3 Start rollback

每一步只回滚已完成资源，逆序执行：

- 禁止新 TAP RX；
- 清 handler 并 quiesce；
- StopRx；
- stop 已启动 shard；
- drain 已提交 TX；
- 校验 outstanding；
- 取消/等待 pending session open；
- 释放 factory、packet queue、pool；
- 最后释放 TAP/exchanger owner。

Netstack2 当前 runtime 的部分 shard 启动失败路径需要 fault injection 核对是否显式
执行 `StopRx/DrainTx`。这是 P0 upstream 阻断项；未验证前不能假定 rollback 完整。

### 12.4 Stop 顺序

结合 OpenPPP 当前 stop pipeline，目标顺序是：

```text
stop new input / clear TAP callback
  -> wait in-flight TAP callback
  -> factory reject new sessions
  -> quiesce transport callbacks and pending opens
  -> Netstack2 StopRx + stop shards + DrainTx
  -> verify OutstandingTx and leases
  -> stop/close session factory connections
  -> DNS/exchanger/route teardown
  -> TAP and adapter dependency destruction
```

实际 pipeline 可把 DNS 与 factory teardown 的细节按 owner 关系细分，但必须满足：
resolver 在 pending open 完成前仍存活，exchanger 在 transport callback quiesce 前仍
存活，TAP 在 Netstack2 TX drain 前仍存活。

### 12.5 Stop timeout

`StopOptions` 默认 deadline 不能被 adapter 改成无限等待。返回 TimedOut 或
DrainFailed 时：

- runtime 保持 `Stopping`；
- dependencies 和 pool 保持存活；
- 输出 outstanding queues/sessions/leases 的计数；
- 上层可用更长 deadline 重试；
- 不调用析构强制清状态；
- 进程最终强制退出属于最外层运维策略，不是 library 成功停止。

## 13. 配置与兼容契约

### 13.1 新配置

推荐新增：

```text
--stack=native|lwip|netstack2
```

内部：

```cpp
enum class StackMode {
    Native,
    Lwip,
    Netstack2,
};
```

替代 `ppp/app/PppApplicationInternal.h:101-123` 中 `NetworkInterface::Lwip`
作为跨层唯一真值。可在迁移期保留 legacy bool parser，但进入 bootstrap 后必须
归一为 enum，不能同时维护两个会分叉的状态。

### 13.2 与 `--lwip` 的兼容规则

优先级与冲突规则：

1. 显式 `--stack` 优先；
2. 同时出现 `--stack=lwip --lwip=yes` 或 `--stack=native --lwip=no`：接受并 warning legacy 参数；
3. 任何语义冲突，如 `--stack=netstack2 --lwip=yes`：启动失败，不静默覆盖；
4. 只有 `--lwip=yes`：映射为 lwIP；
5. 只有 `--lwip=no`：映射为 native；
6. 两者都没有：完全保留当前平台默认。

当前默认由 help/config 路径表达：Windows 使用普通 TAP 且非 Wintun 时 lwIP 默认
开启；Wintun/native 及非 Windows 保持当前 native 默认。P0 parser tests 必须锁定
这些组合，避免 enum 重构改变历史行为。

### 13.3 Netstack2 options

初始 options 建议只暴露经 Netstack2 `Config::Validate()` 且有测试的字段：

```text
netstack2.enabled（由 --stack 推导，不单独产生第二真值）
netstack2.shards=1
netstack2.rx_queue_capacity
netstack2.buffer_pool_size
netstack2.tcp_mss
netstack2.tcp_receive_window
netstack2.tcp_send_window
netstack2.congestion_control
netstack2.stop_timeout_ms
```

keepalive、offload、zero-copy、多 queue 在实现与 capability 协商完成前不应暴露为
“有效”开关。未知字段按 OpenPPP 现有配置策略报错或 warning，但不能悄悄启用愿景能力。

### 13.4 平台和协议拒绝

- `--stack=netstack2` + 非 Linux：明确启动失败；
- 首版 shards/queues 不等于 1：明确启动失败；
- 首版 TAP link type 不符合 IPv4 L3：明确启动失败；
- IPv6/UDP 不通过 Netstack2；TCP 以外协议保持现有路径；
- 编译时未带 `ENABLE_NETSTACK2` 但请求该模式：输出“binary lacks Netstack2 support”；
- 不得 silent fallback，否则运维无法判断实际数据平面。

### 13.5 runtime status

`ppp/app/ApplicationMainLoop.cpp:360-367` 当前只报告 lwip/tc/ctcp 等有限状态。
目标至少增加：

```text
stack_mode: native|lwip|netstack2
netstack2_compiled: bool
netstack2_state: disabled|starting|running|stopping|stopped|failed
netstack2_api_version: 0.3.x/pinned revision
netstack2_queues, netstack2_shards
netstack2_capabilities
netstack2_last_error
```

不要把 `stack_mode=netstack2` 等同于 `state=running`。

## 14. 构建与平台集成

### 14.1 Netstack2 vendoring 前置

当前 Netstack2 顶层无条件 `enable_testing()` 并加入 tests；unit CMake 无平台条件
加入 `tap_io_test`，TAP 源/测试直接包含 Linux headers。vendoring 前 upstream P0
应完成：

```cmake
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

并把 Linux TAP backend 及其测试放入 `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`
或独立 backend target。核心 library 不能因未启用 TAP 而在 Windows/Android/iOS
编译阶段包含 Linux header。

### 14.2 OpenPPP2 CMake 目标

目标形态：

```cmake
option(ENABLE_NETSTACK2 "Build Netstack2 integration" OFF)

if(ENABLE_NETSTACK2 AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(common/libtcpip2)
    target_link_libraries(openppp2_lib PRIVATE tcpip2::tcpip2)
    target_compile_definitions(openppp2_lib PRIVATE PPP_ENABLE_NETSTACK2=1)
endif()
```

实际 target 名已确认：vendored Netstack2 导出 `add_library(tcpip2 STATIC ...)`
与别名 `tcpip2::tcpip2`（`src/CMakeLists.txt`）。不得把其源纳入
OpenPPP2 现有全局 glob；应使用 Netstack2 自身 target，传播 include 和 Threads
依赖。顶层 `CMakeLists.txt:350-357,403-418,448-455,484-606` 是预计接入位置。

`ENABLE_NETSTACK2` 默认 OFF。Linux 以外可选择“不构建该 target”而不是编译半个
backend；parser 仍应对请求不可用模式给出清楚错误。

### 14.3 平台入口

| 平台 | 当前构建入口证据 | 首版状态 | 后续要求 |
|---|---|---|---|
| Linux desktop/server | 顶层 CMake | 唯一可启用平台 | P0–P4 |
| Android | `android/CMakeLists.txt` + Gradle | 禁用 | core 平台隔离、NDK、TUN adapter、生命周期测试 |
| iOS | `ios/CMakeLists.txt` | 禁用 | Network Extension/packet tunnel adapter、Apple toolchain |
| Windows | `sln/*/*.vcxproj` 与顶层平台链接 | 禁用 | TAP/Wintun adapter、MSVC、IOCP/strand 语义 |
| macOS | Darwin/OpenPPP 平台代码 | 禁用 | utun adapter、CMake gate、签名/entitlement |

非 Linux 阶段不能复用 Linux `tap_io.cpp`。更合理的是 OpenPPP backend 直接面向
现有 `ITap` 抽象，Netstack2 自带 TAP backend 只用于其独立测试。

## 15. 依赖获取与许可证

### 15.1 推荐：vendor snapshot/subtree，而非运行时外部 checkout

推荐先采用受控 vendor snapshot（治理上可由 `git subtree` 产生，但提交内容必须
直接存在仓库），目标目录 `common/libtcpip2`。理由：

- OpenPPP2 发布构建自包含；
- CI、源码包和离线构建不依赖 submodule 初始化；
- pin revision 与补丁在普通 diff 中可审计；
- 现有构建/发布流程无需增加 nested repository 状态机。

代价是同步上游提交需要维护脚本/流程，vendor diff 较大。若项目治理最终选择
submodule，也必须 pin 精确 commit、CI 拒绝未初始化/floating 状态、源码发行物
包含依赖，并单独批准相关仓库操作；本文不执行 submodule 操作。

无论选择哪种方式，都禁止：

- 依赖开发机绝对路径；
- 下载未校验的 latest archive；
- 只复制 adapter 所需的若干实现文件；
- 在 vendor snapshot 中混入无来源的本地 dirty 修改；
- 丢失上游 commit、版本、LICENSE 和版权头。

### 15.2 许可证与 KCC notice 风险

OpenPPP2 与 Netstack2 顶层均使用 GPL-3.0，因此许可证家族表面兼容；这不替代
正式合规审查。vendor/发布前必须：

- 保留 Netstack2 `LICENSE` 与源码版权头；
- 记录 pin commit 和本地补丁；
- 核对源码发行物包含相应 source；
- 检查交互式/发行物现有 GPL notice 流程；
- 核对 `common/libtcpip2/src/tcp/congestion.h:654` 所述 `liulilittle/kcc`
  commit `227a20b` 来源及 BSD/GPL dual-license 文本（`.cpp` 无对应归属注释）；
- 按 `common/libtcpip2/docs/adr/009-kcc-v2-port.md:87-88` 的项目决定保留归属；
- 补齐或核对集中 NOTICE/第三方版权文本。

审计快照没有 `NOTICE` 文件。这是**归属材料缺口风险**，不是本文对侵权的法律
结论。必须由维护者/法务确认需要的文本及分发位置，关闭后才能发布。

## 16. 文件级改动矩阵

以下全部是“预计”，当前尚未实施。

### 16.1 OpenPPP2

| 文件 | 预计改动 | 阶段 |
|---|---|---|
| `CMakeLists.txt` | feature option、vendor target、Linux gate、链接 | P1 |
| `ppp/app/PppApplicationInternal.h` | `StackMode` 与 options | P0/P1 |
| `ppp/app/ApplicationConfig.cpp` | `--stack` parser、legacy 冲突规则 | P0/P1 |
| `ppp/app/ApplicationHelp.cpp` | 新参数、默认与平台限制 | P0/P1 |
| `ppp/app/ApplicationClientBootstrap.cpp` | enum 传播、runtime 构造与 rollback | P1/P3 |
| `ppp/app/ApplicationMainLoop.cpp` | stack/runtime status | P1/P3 |
| `ppp/ethernet/VEthernet.h` | runtime owner、TCP ingress hook | P1/P2 |
| `ppp/ethernet/VEthernet.cpp` | 第三分支、TX 回接、停止顺序 | P2/P3 |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp` | Netstack2 factory 接口、policy 复用 | P3 |
| `ppp/app/client/VEthernetNetworkTcpipStack.cpp` | 抽取/复用 resolver 与 connection factory | P3 |
| `ppp/app/client/VEthernetNetworkTcpipConnection.cpp` | transport session adapter 接点 | P3 |
| `ppp/app/client/ClientConnectionOpener.cpp` | exchanger/readiness barrier | P3 |
| `ppp/app/client/ClientConnectionTeardown.cpp` | quiesce/Stop retry 顺序 | P3 |
| `ppp/app/runtime/RuntimeStopPipeline.h` | 显式 Netstack2 stop step/contract | P3 |
| `ppp/ethernet/netstack2/*` | 新 adapter 组件 | P1–P3 |
| `tests/cpp/CMakeLists.txt` | adapter/config/lifecycle tests | P0–P3 |

### 16.2 Netstack2 upstream/vendor

| 文件/领域 | 预计改动 | 阻断级别 |
|---|---|---|
| `common/libtcpip2/CMakeLists.txt` | `BUILD_TESTING` gate | P0 |
| `common/libtcpip2/src/CMakeLists.txt` | Linux TAP 条件或独立 target | P0 |
| `common/libtcpip2/tests/unit/CMakeLists.txt` | Linux-only `tap_io_test` | P0 |
| `common/libtcpip2/include/tcpip2/session_factory.h` | metadata 有效性/typed result 合同 | P0 |
| `common/libtcpip2/src/tcp/handshake.cpp` | 填充或重定义 resolved/mark/dscp 流程 | P0 |
| `common/libtcpip2/src/core/runtime.cpp` | capability 消费、失败 rollback fault tests | P0/P2 |
| `common/libtcpip2/include/tcpip2/datagram_session.h` | typed session pointer | P5 |
| `common/libtcpip2/src/udp/flow_table.*` | writable/backpressure、清 debug 输出 | P5 |
| `common/libtcpip2/src/core/shard.cpp` | 完整 packet/byte metrics | P4 |
| `common/libtcpip2/src/tcp/handshake.*` | keepalive probe 或移除未实现承诺 | P4 |
| `common/libtcpip2/NOTICE` 或项目归属文件 | KCC/第三方 notice | 发布前 P0 |

### 16.3 测试与文档

| 预计新增/修改 | 内容 |
|---|---|
| OpenPPP config unit tests | enum、legacy、平台默认、冲突 |
| packet queue unit tests | ownership、success-prefix、queue full、StopRx |
| session adapter unit tests | partial、WouldBlock、callback race、half-close |
| lifecycle fault tests | Start 每阶段失败、Stop timeout/retry、late callback |
| Linux namespace e2e | TUN/TAP、真实 Direct/VMUX/VPN TCP |
| differential suite | 同一流量对 native/lwIP/Netstack2 |
| performance harness | 吞吐、CPU、p99、copy、queue depth |
| operations/reference docs | feature flag、status、rollback、已知限制 |

## 17. P0–P6 工作包

### 17.1 P0：合同冻结与上游可嵌入性

**范围**：不接真实 TAP，不改变 OpenPPP 默认数据流。

**前置**：选定干净 Netstack2 revision；确认 GPL/KCC review owner。

**交付物**：

- `StackMode`/legacy 参数行为规范与 parser tests；
- TCP open metadata 有效性与 policy-resolution 决策；
- `ITransportSession` callback quiesce、partial、WouldBlock、half-close contract tests；
- `BUILD_TESTING` 与 Linux TAP platform gates；
- runtime partial-start rollback fault tests；
- capability 协商最小规则；
- vendor 方式和 pin policy 决策记录；
- 许可证/NOTICE checklist 与责任人。

**验收**：Netstack2 clean build、CTest、ASan/UBSan、TSan 通过；非 Linux core
compile job 不包含 Linux headers；metadata tests 不依赖默认 `0.0.0.0`；每个
Start failure point 无 thread/lease/handler 遗留。

**回滚**：只回滚合同/构建补丁，不影响 OpenPPP 数据面。

### 17.2 P1：compile-only scaffolding

**范围**：vendor pin、CMake target、feature gate、adapter 类型骨架、runtime status；
不安装 TAP callback，不创建真实 session。

**前置**：P0 完成。

**交付物**：

- `common/libtcpip2` pin snapshot；
- `ENABLE_NETSTACK2=OFF` 默认；
- Linux ON 与所有平台 OFF 编译矩阵；
- adapter 构造/销毁 smoke；
- parser/help/runtime status；
- native/lwIP 回归。

**验收**：默认 binary 行为与大小变化有记录；OFF 时不链接 Netstack2；ON 时
compile/link；请求未编译模式明确失败；现有 native/lwIP tests 全绿。

**回滚**：关闭 option 或回退 P1 commit，默认配置无数据面影响。

### 17.3 P2：Linux synthetic packet adapter

**范围**：单 queue/shard、IPv4 TCP packet RX/TX，使用 fake session，不接真实
OpenPPP remote transport。

**前置**：P1、buffer/packet contracts 完成。

**交付物**：

- callback 内 copy 与 bounded RX queue；
- `RecvBatch/SendBatch` success-prefix；
- shared-owner TX completion；
- StopRx/DrainTx/OutstandingTx；
- synthetic SYN/data/FIN/RST；
- queue full、pool empty、partial TX、late callback tests。

**验收**：ASan/UBSan/TSan 无问题；所有 lease 回池；Stop timeout 可重试；pcap
中的 seq/ack/checksum 合法；UDP/ICMP 仍走旧路径。

**回滚**：feature flag OFF；adapter 不安装 callback。

### 17.4 P3：真实 OpenPPP TCP transport

**范围**：factory/resolver、Direct、VMUX、VPN transport，真实 Linux e2e。

**前置**：P2；exchanger readiness 与 callback quiesce 合同可实现。

**交付物**：

- `OpenPppSessionFactory` 与 `OpenPppTransportSession`；
- fake-IP/DNS/Direct/Proxy/Reject 一致性；
- exchanger-ready 启动屏障；
- partial/WouldBlock/ResumeReceive；
- EOF、RST、half-close、timeout；
- shutdown pipeline 集成；
- runtime fatal 与 status。

**验收**：真实 HTTP/TLS/长流/双向流经 Direct、VMUX、VPN；网络抖动与重连条件
符合预期；native/lwIP differential 无 policy 回归；Stop/retry 无 callback UAF。

**回滚**：停止新灰度，重启使用 native/lwIP；不对运行中 flow 热切换。

### 17.5 P4：多队列、性能与生产可观测性

**范围**：多 queue/shard、flow affinity、metrics 完整性、基准和容量门禁。

**前置**：P3 功能稳定，单 shard baseline 已记录。

**交付物**：

- queue-to-shard mapping 与稳定 RSS/hash；
- 跨 shard return queue 压力验证；
- 完整 packets/bytes/session/stop counters；
- keepalive 实现或明确禁用；
- baseline/per-commit benchmark artifacts；
- 长稳、资源上限、故障注入。

**验收**：团队批准的 regression budget 全部满足；指标守恒；无 unbounded queue；
TSan 与 24h soak 无 race/leak。未达到则保留单 shard 或停止发布。

**回滚**：配置锁回 1 queue/shard，或关闭 Netstack2。

### 17.6 P5：UDP 与 IPv6

**范围**：在独立设计/评审后启用 UDP、再评估 IPv6。

**前置**：typed `IDatagramSession`、writable callback/重试队列、debug 输出清理、
UDP policy 与 DNS/fake-IP 对齐、IPv6 parser/checksum/route 合同。

**交付物**：

- 去除 opaque `void*`；
- UDP WouldBlock 无丢包式“成功”；
- 有界 datagram retry/backpressure；
- flow eviction 与 close callback race tests；
- UDP Direct/Proxy/DNS e2e；
- IPv6 extension header/fragment/PMTU 设计与测试。

**验收**：UDP 压力下无无限缓存、无静默 ownership 错误；IPv6 与平台 route
策略完整。未完成前 UI/help/status 明确显示 unsupported。

**回滚**：协议级 feature gate 关闭，TCP Netstack2 可独立保留。

### 17.7 P6：Android、Windows、iOS/macOS

**范围**：逐平台 backend、构建、生命周期与发布。

**前置**：core 与 Linux backend 解耦；P3/P4 稳定；各平台 owner 批准。

**交付物**：

- Android NDK/TUN queue 与 app pause/resume；
- Windows TAP/Wintun、MSVC 与 completion ownership；
- iOS Packet Tunnel/Network Extension 生命周期；
- macOS utun 与 entitlement；
- 平台 CI、崩溃恢复、后台/前台、网络切换测试。

**验收**：逐平台单独灰度；不能用 Linux benchmark 代替平台数据。

**回滚**：每个平台独立 compile/runtime gate，默认保持旧栈。

## 18. 测试策略与命令

### 18.1 当前已存在的 OpenPPP2 命令

```bash
bash tools/check_include_boundaries.sh
bash tools/check_vcxproj_sources.sh
scripts/run-cpp-tests.sh
scripts/run-cpp-tsan-tests.sh
```

`run-cpp-tests.sh` 实际配置 `tests/cpp` 到 `build/test` 并运行 CTest；TSan 脚本使用
独立 `build/test-tsan` 和 suppression。接入 PR 必须运行与改动相关命令，但本文
只新增文档，因此未运行构建测试。

### 18.2 当前已存在的 Netstack2 命令

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
bash scripts/build-asan.sh
bash scripts/build-tsan.sh
```

ASan 脚本启用 `ENABLE_SANITIZERS=ON`；TSan 脚本启用 `ENABLE_TSAN=ON`，两者互斥。
当前 bench orchestrator 的接口是：

```bash
cmake -S . -B build-bench -G Ninja -DNETSTACK2_BUILD_BENCHMARKS=ON
cmake --build build-bench
BUILD_DIR=build-bench ./bench/run_p0.sh
```

bench 会创建时间戳结果目录并校验记录结构。它测的是 Null RX/TX、pool、timer、
shard 等 P0 场景，不是 OpenPPP e2e 性能。

### 18.3 接入后目标命令（当前尚不存在）

以下命令是 CI/实现目标，option、label 和测试 binary 需在对应阶段创建：

```bash
cmake -S . -B build-netstack2 -G Ninja \
  -DENABLE_NETSTACK2=ON -DBUILD_TESTING=ON
cmake --build build-netstack2
ctest --test-dir build-netstack2 --output-on-failure -L netstack2
ctest --test-dir build-netstack2 --output-on-failure -L netstack2-adapter
ctest --test-dir build-netstack2 --output-on-failure -L netstack2-e2e
```

不得在它们实现前把命令写进发布 checklist 并声称可执行。

### 18.4 单元/合同测试矩阵

| 测试域 | 必测场景 |
|---|---|
| Config | 三 enum、legacy only、一致双参数、冲突、平台默认、binary unsupported |
| RX ownership | callback 返回后覆写 ITap buffer，lease 内容仍稳定 |
| RX pressure | pool empty、queue full、wake 合并、StopRx 并发 |
| TX ownership | success-prefix 0/N/partial、同步复制、异步 completion、late completion |
| Session send | full/partial/WouldBlock/error、无丢 wake、无重复 bytes |
| Session receive | partial tail、pause、ResumeReceive、remote EOF |
| Policy | fake-IP hit/miss、Direct/Proxy/LegacyAuto/Reject、original/resolved 保留 |
| Lifecycle | 每个 Start step fail、Stop timeout、retry、double Stop、callback quiesce |
| TCP close | client FIN、remote FIN、simultaneous close、RST、connect timeout |
| Generation | flow id reuse 后旧 callback 被丢弃 |
| Capability | unsupported offload/queue 配置明确拒绝 |
| Metrics | packet/byte 守恒，drop 原因互斥且可解释 |

### 18.5 Linux e2e

使用 network namespace、veth/TUN/TAP 和本地 origin/proxy，覆盖：

- TCP handshake、短 HTTP、TLS、大文件、双向持续传输；
- 1-byte writes、MSS 边界、窗口收缩/恢复；
- 客户端/远端 half-close；
- Direct、VMUX、VPN transport；
- fake-IP 与 hostname route；
- connection refused、DNS miss、transport reset；
- packet loss、duplicate、reorder、delay、MTU/fragment 条件；
- TAP write queue full 与远端 write queue full；
- teardown 期间持续 RX、late callback、outstanding TX；
- process restart 回滚到 native/lwIP。

`tc netem` 和 namespace setup 可能需要 root 或 `CAP_NET_ADMIN`，应放在隔离的专用
CI runner，不在普通单元测试中隐式提权。

### 18.6 differential 与回归

对同一 pcap/请求集合分别运行 native、lwIP、Netstack2，比较：

- policy decision 与 resolved endpoint；
- 用户可见连接成功/错误分类；
- payload hash；
- close/timeout 行为；
- DNS/fake-IP 映射；
- UDP/ICMP 在 Netstack2 TCP-only 模式下与 native baseline 一致；
- route/exchanger teardown 无回归。

差分不是要求 TCP packet 字节级完全一致；ISN、option、ACK timing 可不同，但必须
在明确的协议 invariant 和用户可见合同内。

## 19. 可观测性与性能预算

### 19.1 最小 counters

```text
netstack2.rx.packets
netstack2.rx.bytes
netstack2.rx.copy_bytes
netstack2.rx.pool_empty
netstack2.rx.queue_full
netstack2.rx.malformed
netstack2.rx.unsupported_protocol
netstack2.tx.packets
netstack2.tx.bytes
netstack2.tx.partial
netstack2.tx.would_block
netstack2.tx.outstanding
netstack2.session.open.started
netstack2.session.open.succeeded
netstack2.session.open.rejected
netstack2.session.open.failed
netstack2.session.active
netstack2.session.send.partial
netstack2.session.send.would_block
netstack2.session.late_callback
netstack2.stop.started
netstack2.stop.succeeded
netstack2.stop.timed_out
netstack2.stop.drain_failed
netstack2.telemetry.dropped
```

维度必须有界。不得把原始 hostname、IP、flow id 当高基数 metrics label；需要排障
时通过采样 structured event，并遵循现有隐私/日志策略。

### 19.2 日志

状态转换、Start validation failure、capability mismatch、Stop timeout 可记录结构化
日志。packet drop 使用聚合 counter 和 rate-limited summary，不能每 packet 打印。

审计快照（commit `c012a9c`）中的 `common/libtcpip2/src/udp/flow_table.cpp:39`
含 1 处 `std::fprintf(stderr, "[flowdbg] OnClientDatagram ...")` 调试输出；
审计时 dirty worktree 已删除该输出（工作树 diff 为 2 行删除）。P5 前必须确认
pin 的干净 revision 不携带任何调试输出，或替换为受控、限速 telemetry。

### 19.3 性能门禁模板

本文不虚构“提升 X%”。P3/P4 应在固定硬件、编译器、MTU、cipher/transport、
flow 数和 payload 下同时记录 native/lwIP/Netstack2：

- throughput；
- packets/s；
- CPU cycles 或 process CPU%；
- RSS/heap/pool high-water；
- p50/p95/p99 latency；
- RX copy bytes/packet；
- queue depth/would-block/drop；
- Stop latency。

门禁：

1. 功能、数据完整性、leak、ASan/TSan 为零容忍；
2. 无 unbounded memory/queue growth；
3. 性能回归阈值由团队基于首轮可信 baseline 评审批准，并写入 CI 配置；
4. 未批准阈值前只收集数据，不宣称性能胜出；
5. 超阈值自动阻止扩大灰度，不能用平均值掩盖 p99 或内存恶化。

## 20. 安全、可靠性与风险登记

### 20.1 安全边界

- TAP 输入不可信：验证 IPv4 header length、total length、TCP offset、checksum 策略和最大包长；
- 所有 size arithmetic 防溢出，禁止负 `PacketLength` 转无符号大值；
- 不记录 payload；
- fake-IP/route policy 必须与旧栈一致，避免绕过 Reject；
- session callback 使用 generation 防 UAF/ABA；
- pool/queue 有硬上限，抵御 packet flood；
- Start config 与 capability 不匹配 fail closed；
- vendor revision、许可证与第三方来源可追溯。

### 20.2 风险登记

| 风险 | 概率/影响 | 早期信号 | 缓解 | Owner/阶段 |
|---|---|---|---|---|
| ITap buffer 被异步复用 | 高/严重 | 随机 packet corruption | callback 内 lease copy + 覆写测试 | Adapter/P2 |
| policy metadata 缺失 | 高/严重 | resolved 为 0.0.0.0、绕过 route | P0 合同与 resolver 复用 | Core+Client/P0 |
| exchanger 未 ready 收到 SYN | 中/高 | 启动初期偶发 reset | readiness barrier | Client/P3 |
| callback 晚到 UAF | 中/严重 | teardown crash/TSan | 同步 quiesce + generation | Adapter/P2/P3 |
| TX lease 提前释放 | 中/严重 | corrupt packet/UAF | shared completion owner | Adapter/P2 |
| WouldBlock 丢 wake | 中/高 | 流永久停住 | edge-safe callback contract | Adapter/P2/P3 |
| Stop timeout 后误析构 | 中/严重 | outstanding/UAF | 保留 runtime、可重试 Stop | Runtime/P2 |
| half-close 语义错误 | 中/高 | HTTP response 截断 | transport 分类 e2e | Client/P3 |
| native/lwIP 默认改变 | 中/高 | 大范围回归 | parser matrix、default OFF | Config/P0/P1 |
| 多 shard flow 漂移 | 中/高 | reorder/状态分裂 | P4 前固定单 shard | Core/P4 |
| UDP opaque pointer/backpressure | 高/高 | drop/UAF | P5 前禁用 UDP | Core/P5 |
| 能力声明不实 | 中/高 | checksum/offload corruption | 保守 capability + reject | Core/P0 |
| metrics 不完整 | 高/中 | 无法灰度判断 | counter 守恒 tests | Core/P4 |
| Linux 源污染跨平台构建 | 高/中 | MSVC/NDK include failure | CMake platform gate | Build/P0 |
| vendor 来源/NOTICE 不完整 | 中/高 | 发布合规阻断 | pin、LICENSE、KCC review | Release/P0 |
| `[flowdbg]` 调试输出进入发布 | 中/中 | stderr 泄漏/性能下降 | clean pin、日志扫描（审计快照仅 1 处，工作树已删除） | UDP/P5 |

### 20.3 可靠性不变量

- 一个 TCP flow 只由一个 owner shard 修改；
- 一个 lease 任意时刻只有一个 owner；
- success-prefix 之外的 lease 所有权不转移；
- callback 设空返回后旧 callback 不再执行；
- Stop 成功意味着 RX stopped、TX drained、outstanding zero、threads joined；
- Stop 失败意味着对象仍有效且可重试，不意味着“差不多停止”；
- policy reject 不创建第二腿；
- runtime 状态与实际启用栈一致；
- 首版 UDP/IPv6 不能误入 Netstack2。

## 21. 发布、灰度与回滚

### 21.1 Feature flags

三层 gate：

1. build：`ENABLE_NETSTACK2=OFF` 默认；
2. runtime：`--stack=netstack2` 显式选择；
3. deployment：配置下发 allowlist/canary cohort。

可选增加 server-side rollout policy，但本地 binary 必须仍能报告真实 stack mode。
不得让远端配置在不重启进程的情况下迁移已有 flow。

### 21.2 灰度阶段

```text
开发/CI synthetic
 -> 内部 Linux 单机、Direct only
 -> 内部 VMUX/VPN
 -> 小比例无关键业务 canary
 -> 按平台/版本扩大
 -> 默认值评审（不属于初始接入）
```

每一阶段观察 open success/error、queue full、WouldBlock、late callback、Stop latency、
crash、CPU/RSS/p99。任何数据完整性、UAF、死锁、policy 绕过立即停止灰度。

### 21.3 回滚

- 配置回滚只影响**新进程**；
- 先停止接收新连接，再按正常 Stop drain；
- 重启选择 native 或 lwIP；
- 不把已有 Netstack2 flow 搬到其他栈；
- 若 vendor regression，回退到记录的上一 pin commit 并重新跑全套合同/e2e；
- 发布物必须能确认 compiled revision 与 runtime stack mode。

## 22. Definition of Done

Netstack2 TCP 集成只有在以下条件全部满足后才可称“已接入并可灰度”：

- [ ] `StackMode`、legacy 兼容和平台默认有测试，Netstack2 默认关闭；
- [ ] 依赖 pin、构建可复现，无机器绝对路径；
- [ ] GPL/KCC attribution/NOTICE review 关闭；
- [ ] P0 build/platform/metadata/callback contract 缺口关闭；
- [ ] Linux IPv4 TCP single queue/shard adapter 实现；
- [ ] ITap RX callback 内复制，覆写测试通过；
- [ ] SendBatch success-prefix 与 async owner 测试通过；
- [ ] Direct、VMUX、VPN transport 的 partial/WouldBlock/ResumeReceive 正确；
- [ ] fake-IP、DNS、route、Reject 与旧栈 differential 通过；
- [ ] exchanger readiness barrier 消除启动 SYN race；
- [ ] half-close、RST、timeout、late callback 测试通过；
- [ ] Stop timeout/retry、outstanding lease 和 callback quiesce 通过 fault tests；
- [ ] native/lwIP 全量回归通过；
- [ ] ASan/UBSan/TSan 与 soak 无错误；
- [ ] runtime status、counters、structured errors 可用于灰度；
- [ ] 性能 baseline 与团队批准门禁已记录；
- [ ] 运维文档写明 Linux/TCP-only、重启回滚和 unsupported 能力；
- [ ] feature flag、canary 与停止条件已演练。

UDP、IPv6、多 queue 和跨平台各自有独立 DoD，不因 TCP DoD 完成自动获得支持。

## 23. 首个 PR 的严格范围

首个 PR 只做 **P0 合同 + OpenPPP compile-only 配置 scaffolding**，建议上限：

- 新增 `StackMode` enum；
- 解析 `--stack`，实现与 `--lwip` 的冲突/兼容规则；
- 更新 help 和 parser unit tests；
- 保持 bootstrap 最终仍只允许 native/lwIP，选择 Netstack2 时报告“not compiled”；
- 在 Netstack2 upstream/候选 vendor 分支修 `BUILD_TESTING` 和 Linux TAP gates；
- 增加 TCP open metadata、callback quiesce、partial-start rollback 的合同测试；
- 固定将要 vendor 的 clean revision 和许可证 checklist。

首个 PR **不应**：

- 安装真实 TAP callback；
- 改 `VEthernet::PacketInput` 数据路径；
- vendor 带 dirty 修改的整棵源码；
- 链接真实 OpenPPP transport；
- 启用 UDP/IPv6/多 shard；
- 改变默认栈；
- 宣称性能或生产 readiness。

若“上游 Netstack2 修复”和“OpenPPP parser”不能在同一仓库 PR 表达，应拆成两个
有依赖关系的首批 PR：先 upstream P0，再 OpenPPP compile-only。不要为了单 PR
方便把未 pin 的外部源码直接复制进来。

## 24. 后续待决策清单

1. TCP open metadata 采用 factory-resolution 还是新增 async policy resolver hook？
2. `VEthernetNetworkTcpipConnection` 是直接复用、抽取 factory，还是增加最小 adapter facade？
3. OpenPPP 现有 write API 哪条路径能安全持有 lease owner 到 completion？
4. Direct、VMUX、VPN 各自的 pause-read 与 writable 通知合同是什么？
5. callback 设空的同步 quiesce 是否能由所有 transport 实现保证？
6. exchanger readiness 应重排全局启动顺序还是只 gate TAP RX？
7. fatal runtime error 由谁触发进程级 teardown，如何避免重复 Stop？
8. vendor snapshot/subtree 的同步 owner、周期和 patch policy 是什么？
9. KCC 需要的集中 NOTICE/上游版权文本由谁批准？
10. 第一轮 baseline 的硬件、编译器、transport 和门禁阈值是什么？
11. 多 queue 时使用 5-tuple hash、TAP queue id 还是 OpenPPP dispatch affinity？
12. keepalive 是在 P4 实现，还是从首版公开配置中明确移除？
13. UDP typed API 与 writable contract 的最终形态是什么？
14. IPv6、fragment、PMTU 由 Netstack2 还是 OpenPPP ingress 负责？
15. Android/iOS app 生命周期中 Stop timeout 的最外层策略是什么？

未决项必须在对应阶段开工前由代码 owner 形成可测试决定；不能在 adapter 中以
临时默认值悄悄定案。

## 25. 关键源码索引

### 25.1 OpenPPP2

| 路径 | 审计要点 |
|---|---|
| `ppp/tap/ITap.h:40-48,186-198,243-249` | callback 裸 pointer、复用 buffer、fallback packet |
| `ppp/tap/ITap.cpp:463-495` | RX 读入复用 buffer、同步 callback、重新 arm |
| `ppp/tap/ITap.cpp:535-644` | TX queue、drain、raw copy、shared owner |
| `ppp/ethernet/VEthernet.cpp:90-120` | 当前释放顺序 |
| `ppp/ethernet/VEthernet.cpp:334-447` | TAP/lwIP/VNetstack 初始化；process-wide lwIP loopback |
| `ppp/ethernet/VEthernet.cpp:449-525` | TAP callback、MTA/SSMT copy/post、callback 安装 |
| `ppp/ethernet/VEthernet.cpp:606-649` | TCP lwIP/native 分叉与 UDP/ICMP 路径 |
| `ppp/ethernet/VEthernet.cpp:997-1016` | raw output 到 TAP |
| `ppp/ethernet/VEthernet.h:34-40,106,136-150,234,243-282` | VEthernet API、flags、owners |
| `ppp/app/client/VEthernetNetworkSwitcher.cpp:467-535,662-671` | stack 创建、policy hook、resolver |
| `ppp/app/client/VEthernetNetworkTcpipStack.cpp:33-143` | TCP open、resolve、connection 创建 |
| `ppp/app/client/VEthernetNetworkTcpipConnection.cpp:130-321` | relay、Direct、VMUX、VPN transmission |
| `ppp/app/client/routing/ResolvedDestination.h:10-17` | original/resolved/hostname/action |
| `ppp/app/client/dns/DnsInterceptor.cpp:797-835` | fake-IP/human route resolution |
| `ppp/app/client/routing/TcpRoutingSelector.cpp:5-20` | route actions |
| `ppp/app/client/ClientPacketDispatchHandler.cpp:65-365` | NAT、UDP/ICMP、DNS/route policy |
| `ppp/app/ApplicationConfig.cpp:427-434` | legacy `--lwip` parser |
| `ppp/app/PppApplicationInternal.h:101-123` | `NetworkInterface::Lwip` bool |
| `ppp/app/ApplicationClientBootstrap.cpp:70-76,248-280` | TAP/open 配置与失败 rollback |
| `ppp/app/ApplicationHelp.cpp:95-105` | lwIP help/default |
| `ppp/app/ApplicationMainLoop.cpp:360-367` | 当前 runtime status |
| `ppp/app/client/ClientConnectionOpener.cpp:69-136` | VEthernet 先于 exchanger |
| `ppp/app/client/ClientConnectionTeardown.cpp:52-94` | 当前 teardown |
| `ppp/app/runtime/RuntimeStopPipeline.h:29-35` | stop step 顺序 |
| `CMakeLists.txt:350-357,403-455,484-606` | include/glob/library/platform link |

注意 `VEthernet.cpp:334-447` 显示即使 native 路径，当前仍可能启用 process-wide lwIP
loopback；实现第三模式时不能仅凭 `StackMode != Lwip` 就无条件清除该现有机制，
必须先确认它是否被其他功能依赖。

### 25.2 Netstack2 未来 vendor 路径

本表路径按第 4.3 节映射；行号来自 API v0.3.0 审计快照。

| 路径 | 审计要点 |
|---|---|
| `common/libtcpip2/include/tcpip2/netstack.h:36-87` | facade、Start/Stop/retry/status |
| `common/libtcpip2/include/tcpip2/runtime_deps.h:23-50` | non-owning mandatory dependencies |
| `common/libtcpip2/include/tcpip2/packet_io.h:15-134` | success-prefix、queue/io API |
| `common/libtcpip2/include/tcpip2/buffer.h:11-29,85-116,184-270` | move-only lease、pool、return queue |
| `common/libtcpip2/include/tcpip2/session_factory.h:48-94` | TCP/UDP open request/result |
| `common/libtcpip2/include/tcpip2/transport_session.h:36-131` | send/receive、callbacks、close |
| `common/libtcpip2/include/tcpip2/datagram_session.h:8-18` | opaque datagram result 缺口 |
| `common/libtcpip2/include/tcpip2/events.h:15-84` | shard 非阻塞 callback、metrics |
| `common/libtcpip2/include/tcpip2/shutdown.h:17-45` | stop status/options/result |
| `common/libtcpip2/include/tcpip2/config.h:26-112` | config 与 validation |
| `common/libtcpip2/include/tcpip2/capabilities.h:21-51` | backend capability 声明 |
| `common/libtcpip2/src/core/runtime.cpp:173-540` | start、open、stop、drain、销毁 |
| `common/libtcpip2/src/core/shard.cpp:316-523` | owner loop、lanes、budget、metrics |
| `common/libtcpip2/src/core/shard.h:152-240` | budgets 与 shard-owned data |
| `common/libtcpip2/src/tcp/handshake.cpp:389-416` | client payload 到 TrySend |
| `common/libtcpip2/src/tcp/handshake.cpp:527-600` | FIN/CloseWait/half-close |
| `common/libtcpip2/src/tcp/handshake.cpp:673-693` | 删除前 callback 清理 |
| `common/libtcpip2/src/tcp/handshake.cpp:825-838` | TCP request metadata 缺失 |
| `common/libtcpip2/src/tcp/handshake.cpp:1013-1143` | remote callback 转 shard message |
| `common/libtcpip2/src/tcp/handshake.cpp:1199-1271` | deactivate、partial tail、resume |
| `common/libtcpip2/src/tcp/handshake.cpp:1364-1397` | close/abort |
| `common/libtcpip2/src/udp/flow_table.cpp:39-203` | metadata、WouldBlock、dirty debug 输出 |
| `common/libtcpip2/src/CMakeLists.txt:6-45` | 仅 Null/TAP backend、Threads |
| `common/libtcpip2/tests/unit/openppp_adapter_spike_test.cpp:1-157` | stub adapter spike |
| `common/libtcpip2/tests/integration/openppp2_smoke_test.cpp:1-17` | synthetic smoke，不是真集成 |
| `common/libtcpip2/docs/adr/008-api-correctness-reset.md:29-30` | typed UDP 声明与代码漂移 |
| `common/libtcpip2/docs/adr/009-kcc-v2-port.md:87-88` | KCC 派生与归属决定 |

## 26. 结论

Netstack2 可以作为 OpenPPP2 的第三种 TUN 侧 TCP 终结栈，但可行性依赖严格的
adapter 边界，而不是简单地在 CMake 中链接一个库。最关键的工程工作是：

- 在 ITap reusable buffer 与 Netstack2 lease 之间建立明确复制/所有权；
- 让所有远端事件回到 owner shard；
- 复用 OpenPPP2 policy 与第二腿 transport；
- 对 partial、WouldBlock、half-close 和 callback quiesce 建立可执行合同；
- 让 Stop timeout 保留依赖并可安全重试；
- 在 clean、pin、合规可追溯的 dependency 上逐阶段启用。

在 P0–P3 验收完成前，准确的项目状态仍是：**Active / Planned，尚未接入，默认关闭，
仅设计 Linux IPv4 TCP 路径。**
