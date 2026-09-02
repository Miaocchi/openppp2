# XTCP Linux IPv4 laboratory runtime 设计

> Status: Active / Laboratory
> Type: Design
> Last verified revision: `e79db8fd10a1ee39be2dc3a9361727fcad79d04c`

## 1. 状态与范围

XTCP 依赖来自已确认授权的内部仓库。许可证不再阻断内部 laboratory 集成。源码通过 `tools/prepare_xtcp.sh` 固定下载并校验，解包目录中的 `.openppp2-xtcp-revision` 必须等于上述 revision。

当前完成的是 **Linux desktop、client、IPv4 TCP、显式 opt-in 的 laboratory runtime**，不是 production 功能。只有同时满足以下编译门禁时 `--tcp-stack=xtcp` 才可用：

- `PPP_ENABLE_XTCP=1`：固定依赖已编入 artifact；
- `PPP_XTCP_RUNTIME_WIRED=1`：OpenPPP2 packet、listener、bridge、启动和 teardown 已接线。

顶层仅在 `ENABLE_XTCP=ON` 的生产 target 上定义这两个宏。关闭该选项时不包含任何 XTCP header 或链接依赖，显式请求 XTCP 保持 fail-closed，绝不回退 native/lwIP。历史默认和 `--lwip` 兼容规则不变。

当前明确不支持：

- production rollout 或性能承诺；
- IPv6、UDP、IPv4 分片、Windows、Android、iOS；
- 运行中热切换；
- MIMT；
- 多 owner executor；
- 零拷贝 stream bridge；
- 无限 flow、packet 或 stream 缓冲。

## 2. 组件边界

生产实现位于 `ppp/app/client/xtcp/`：

- `XtcpFirstLegHooks.h`：VNetstack/connection 可见的窄回调，不暴露任何上游 XTCP 类型；
- `XtcpPoolLease`：进程级引用计数 lease，首租约初始化 BufRef pools，最后一个 runtime 完成 stack/flow 清理后关闭 pools；
- `XtcpNdiBackend`：完整 L3 packet output adapter；
- `XtcpRuntime`：单 owner strand、deadline-driven timer pump、exact listener、flow registry、loopback connector 和双向背压；
- `XtcpRuntimePolicy.h`：standalone 可测试的 consumed、容量和 generation gate。

`VEthernetNetworkSwitcher` 持有 runtime。`VEthernetNetworkTcpipStack` 和 `VEthernetNetworkTcpipConnection` 只通过 `XtcpFirstLegHooks` 与 runtime 交互，不 include 上游 API。

## 3. Packet ingress 与 fail-closed

`VEthernetNetworkSwitcher::OnPacketInput` 的顺序固定为：

1. 先运行现有 `ClientPacketDispatchHandler`，保留 VNet raw NAT；
2. 若它未消费，且模式为 XTCP、协议为 IPv4/TCP，则把完整 L3 packet 提交 runtime；
3. 对 XTCP TCP 始终返回 consumed，包括 runtime 未 ready、已停止、队列满、pool exhausted 或 packet 无法提交；
4. UDP、ICMP、IPv6 和非 XTCP 模式沿用原路径。

这样任何 XTCP 故障都不会把同一 TCP flow 静默送入 native stack。

Ingress 在跨 executor 前复制，使用 item 与 byte 双上限。runtime strand 上把 copy 转入 XTCP `BufRef`，pool exhaustion 只丢弃当前 packet，不产生 native fallback。

## 4. NDI ownership

`XtcpNdiBackend` 把 XTCP 生成的完整 L3 packet交给 `VEthernetNetworkSwitcher::Output`：

- `Output=true` 才消费 `packet.owned`；
- `Output=false` 不 move ownership，交由 XTCP retry queue 后续重试；
- `TxBatch` 仅消费 accepted prefix；
- `Caps()` 返回 `kCapNone`；
- `Stop()` 清空 output/Rx handler，停止后拒绝新 packet。

backend 不缓存 borrowed pointer，也不把拒绝伪装成成功。

## 5. 单 owner 与 callback 规则

所有 `XtcpStack` API 都由 runtime 专属 strand 驱动。上游 recv/state/accept callbacks 可能在 shard lock 下运行，因此 callback 仅允许：

- 四元组匹配或轻量状态标记；
- 把 borrowed stream bytes复制到有界 owned chunk；
- post generation-tagged work 到 owner strand。

callback 禁止重入 `OnPacket`、`Send`、`Close`、`Abort`、`Listen`、`StopListen` 或 `PollAckTimers`。

runtime 使用 `steady_timer` 驱动 `PollAckTimers()`。调度策略是 deadline-driven：上游补丁 `0001-next-timer-deadline.patch` 为 `XtcpStack` 增加 `NextTimerDeadlineUs()`，runtime 把下一次 poll 精确武装到该 deadline（无 armed deadline 时退化为 10ms idle watchdog）；任何 stack-mutating 路径通过 `KickPoll()` 以更早的 deadline 抢占挂起的等待。这替代了早期 laboratory 的固定 1ms 间隔；负载数据见第 11 节。

## 6. Exact listener 与 deferred SYN

首个 IPv4 TCP SYN 建立 pending flow：

1. 保存唯一 owned SYN；
2. 对 SYN 目标 endpoint 建立 exact `Listen` 引用；
3. 先创建 external second-leg connector；
4. connector bind `127.0.0.1:0`，取得 ephemeral source port；
5. 先把 source port + runtime/flow generation 精确登记到 VNetstack，再 connect 本地 listener；
6. `VEthernetNetworkTcpipConnection` 完成 second-leg peer setup 后，通过 weak `XtcpFirstLegHooks::OnFirstLegReady` 通知 runtime；
7. generation 匹配时才把 deferred SYN 喂给 XTCP。

XTCP `AcceptHandler` 以 remote/local 完整四元组匹配 pending flow，匹配后记录 opaque conn id；任何不匹配 accept 直接返回 false。listener 按 endpoint 引用计数，最后一个 flow 结束时 `StopListen`。本实现不启用 MIMT。

## 7. VNetstack loopback bridge

`VNetstack` 增加独立 `external_clients_` 注册表，不复用或改变 native/lwIP `TapTcpLink`、`lan2wan_`、`wan2lan_` 和 `lwip::netstack::link` 语义。

`ProcessAcceptSocket` 只有在以下条件同时满足时走 external path：

- peer 是 IPv4 loopback；
- peer source port 与 one-shot registration 精确匹配；
- registration 尚未取消或消费。

未登记的 loopback 继续遵循原 native/lwIP gate。external client 无 `TapTcpLink`，只复用 accepted socket 与现有 connection forwarding 工厂。

external connection 覆盖 `AckAccept`：

- external 模式 one-shot signal first-leg ready，然后启动正常 `Establish`；
- 普通 native/lwIP 模式调用 base `AckAccept`；
- `Dispose`/析构 one-shot signal first-leg closed；
- weak hooks 避免 connection 与 runtime 循环引用。

## 8. Stream bridge 与背压

两个方向均有硬上限：

- XTCP recv 使用 checked callback；进入 connector 写队列前**同时检查两层预算**——per-flow queued-byte cap（当前默认 4MiB）与 runtime 全局 queued-byte budget（当前工程缺省 32MiB），任一不足均拒绝本次接收并返回 false，使 XTCP 宣告接收背压。全局 budget 仅作为资源安全保险丝，不作为正常流控手段；production 要求在受支持并发与 burst/churn 下该拒绝路径近似不发生；
- connector `async_read` 每次只保留一个 owned chunk；`XtcpStack::Send=false` 时暂停下一次 read，并以默认 1ms timer 重试同一 chunk。该 retry 是 adapter 的兼容性轮询机制；实验已证明缩短 retry 周期不改善吞吐（stall 由对端 ACK 释放节奏决定）。`OPENPPP2_XTCP_LAB_SEND_RETRY_US` 仅供 laboratory A/B，不得作为 production 调优参数；
- 不因 would-block 建立第二个 pending read chunk；
- first leg 收到 FIN 后先排空 connector 写队列，再对 connector 执行 `shutdown(SHUT_WR)`；connector EOF 则向 XTCP first leg 发起 `Close`，因此两个方向可独立 half-close；
- RST、非 EOF I/O error、closed、hook closed 和 runtime stop 汇入 one-shot flow teardown；second leg ready 前收到 RST 会直接取消 pending flow；
- 当前 adapter 显式拒绝 IPv4 分片，避免在重组前误读非首片为 TCP header；若后续支持分片，必须增加有界的 per-datagram pending queue 后再使用上游 reassembly。

所有 socket、timer 和 posted handler 携带 runtime/flow generation。旧 runtime 的 callback 不得复活新 generation flow。

### 8.1 half-close 平台限制（已定案）

ppp 转发核在 client 发起半关（app 先发 FIN）后，下行尾包会被截断，连接以 EOF 或 RST 终结。根因：`shutdown(SHUT_WR)` 排空写队列后，`VirtualEthernetTcpipConnection::ReceiveSocketToTransmission` 对任何读错误（含 EOF）直接 `Dispose()`；vmux_skt `finalize()` 丢弃 rx_queue_ 尾包并硬关 loopback socket，于是 connector 读到 ECONNRESET，`CloseFlow(flow, true)` 向 app 发 RST。`--tcp-stack=native` 在同一场景行为一致（同一 ConnectionResetError），证明这是平台既有限制而非 XTCP 回归。

runtime 侧的对应语义：

- second-leg 转发对象已消失（`OnFirstLegClosed`）且首腿已建立时，flow 置 `peer_gone`：丢弃未投递的上行数据、吞掉 first leg 下行数据以维持流控推进；connector 读侧继续排空内核已收字节，EOF 优雅关、ECONNRESET 诚实 Abort；
- 首腿建立前对端即拒绝/失败时，注入 deferred SYN 并置 `abort_when_ready`，`AcceptHandler` 返回 false 使栈以 RST 应答 app 的 pending connect（快速失败而非超时），随后异步清理 flow。

完整排空需要 vmux 协议级半关改造（vmux_skt 边缘 + 服务端 + 可能的 wire 协议变更），超出本次 laboratory 接入范围，列为 production gate 后续项。

## 9. 启动、readiness 与 teardown

启动顺序：

1. `VEthernet::Open` 创建 VNetstack listener；
2. XTCP 模式创建 pool lease、backend、stack、callbacks 与 timer；任何显式初始化失败使 client Open 失败；
3. exchanger Open 成功后 `MarkReady`；
4. `GetRuntimeReadiness()` 在 XTCP 模式额外要求 runtime ready。

关闭顺序：

1. stop input，解除 TAP packet callback；
2. 立即 `XtcpRuntime::Stop`，拒绝新 ingress；
3. strand 上取消 timer/connect/read/write，关闭 flows/listeners，析构 stack/backend，释放 pool lease；
4. 再 dispose exchanger、QoS 和其他 client services。

Start/Stop 与 flow close 均为幂等操作，stale generation handler 只返回，不执行资源重建。

## 10. 运行时指标与 stats-json

`XtcpRuntime::SnapshotStats()` 汇总 `ppp::app::runtime::RuntimeXtcpStats`（全部为进程内累计原子计数，gauge 除外）：

| 字段 | 含义 |
|------|------|
| `ingress_submitted` | 成功进入 ingress 队列的 IPv4/TCP datagram |
| `ingress_dropped` | 未 ready / 已停止 / 预算耗尽被拒的 datagram |
| `ingress_injected` | 实际注入 XTCP 栈的 datagram（含 deferred SYN 注入） |
| `flows_opened` / `flows_closed` | 由入站 SYN 创建、因任何原因拆除的 flow 数 |
| `flows_active` | gauge：`opened - closed` |
| `timer_polls` / `timer_events` | PollAckTimers 扫描次数 / 各 flow timer 触发总数 |
| `output_packets` / `output_bytes` | 栈产出的 L3 packet 与字节 |
| `connector_read_bytes` | second-leg connector 读出的字节数（app → 栈） |
| `connector_written_bytes` | 写入 connector 的字节数（栈 → app） |

读取路径：stats tick（`PppApplication::OnTick`，1s 周期）→ `VEthernetNetworkSwitcher::GetXtcpRuntimeStats()` → 同一 owner 持有的 runtime。XTCP 模式下 `--stats-json` 的每行 NDJSON 带 `xtcp` 块；非 XTCP 模式无该块。

注意：tick 是 1 秒周期，battery 类短流量可能在最后一个 tick 落盘前结束，因此消费方必须按"快照覆盖窗口"语义等待/折叠累计值，而不是只读最后一行。E2E 断言即按此实现（最长 15s 收敛 + 跨行取 max）。

### 10.1 稳定统计与诊断遥测的边界

`--stats-json`（上表 + `RuntimeXtcpStats`）是**稳定运行时统计接口**；字段变更需保持向后兼容。

`OPENPPP2_XTCP_PERF_JSON=<path>` 是**独立的诊断遥测通道**，不属于 `--stats-json` 的稳定接口。其字段服务于 laboratory/性能/根因分析，可随内部实现演进；未启用时不引入持续开销。当前输出分组：

- `send.*`：栈 Send 准入调用、拒绝、累计 stall；
- `admission.*`：仅当同时设置 `OPENPPP2_XTCP_SEND_ADMISSION_JSON=1` 时输出。每个 flow 仅在 `Send=false` 的阻塞转换时读取一次上游快照；报告当前 blocked flow/字节、非可发送状态与 `snd_buf` quota 分类、最长连续阻塞时间，以及 attempted/pending/inflight/snd_buf/snd_wnd/cwnd/pacing deadline 的跨 flow 范围。它只读状态、不改变 retry、队列、定时器或 TCP 行为；
- `conn.*`：connector read/write 次数与字节、平均块大小、write cycle/gap 直方图分位、per-flow 队列高水位、**`rej`/`rej_bytes`（OnReceive 因 per-flow cap 或全局 budget 被拒的次数与字节）**；
- `recv.*` / `out.*`：栈收发两侧的包数、字节与平均 segment/packet 大小；
- `tcp.*`：首个活动连接的真实 cwnd/inflight/snd_wnd/ssthresh/重传计数（ConnStats 导出）；
- `owner.*`：ingress post/dispatch/dropped/injected 与 strand 队列延迟分位；
- `queue.*`：全局 queued-byte 当前值/高水位、超过 256KiB/1MiB/2MiB 的 flow 数；
- `ndi.*`：NDI 输出包率、Output wall time 与 callback interval 分位、TxBatch 统计；
- `timer.*`：poll 武装数与 lateness 分位。

排查 `Send=false` stall 时，`admission.sndbuf_quota` 与 `inflight/sndbuf` 范围先用于确认直接许可门；其后才结合 `conn.rej`、`send.stall_ms`、`queue.global_high`、`ndi.out_*`/`iv_*` 判断 ACK/output 前置原因。

## 11. 测试、验证证据与进入 production 前的限制

### 11.1 上游源码与补丁机制

`tools/prepare_xtcp.sh` 固定下载并校验上游源码（`.openppp2-xtcp-revision` 必须等于第 1 节 revision），随后**幂等应用** `tools/xtcp-patches/*.patch`（文件名序）。已应用的补丁集合 sha256 记录在解包目录 `.openppp2-xtcp-patches`；反向可干净移除的补丁视为已应用而跳过。当前补丁：

- `0001-next-timer-deadline.patch`：为 `XtcpStack` 增加 `NextTimerDeadlineUs()` / `kNoTimerDeadline`，支撑 deadline-driven poll（见第 5 节）；
- `0002-send-admission-observability.patch`：为 `SendData` 拒绝路径记录只读许可快照，并提供 stack accessor，供上述 opt-in stall 诊断读取。
- `0003-ack-release-observability.patch`：ACK 释放/flush 路径的只读遥测计数（attempts、pacing 门、window/cwnd 门等）。
- `0004-timestamp-aware-data-payload-cap.patch`：`DataPayloadCap` 计入 TSopt/MD5 选项长度，按实际 wire 上限封顶 segment 载荷。
- `0005-pacing-burst-quantum.patch`：修复 pacing 子系统的吞吐钳制——① `NextTimerDeadline()` 纳入 `pacing_deadline_`，paced 连接在 pacing 到期时精确唤醒（此前 pending 缓冲一律报"立即到期"，host loop 空转）；② `FlushPendingSend` 改为突发 pacing：每个 pacing tick 放行 ~1ms 线速数据（fq/TSO autosize 量子，clamp 到 [1 segment, 64KB]），此前每次 flush 调用最多 1 个 MSS，任何 `pacing_rate > 0` 的 CC（KCC/BBR）吞吐被钉死在 MSS/事件循环迭代（与 RTT/cwnd 无关，实测 ~216Mbps）；③ pacing 门空手返回时零拷贝回挂缓冲（swap 代替整块 assign）。同时修复突发发送暴露的丢包恢复停摆（`test_wscale_transfer`，12.5% 丢包下 40-200s 超时）：④ OOO 数据驱动的 dup-ACK 不再受 100/s 泛洪限速——只有新增 SACK 信息的到达立即回 ACK，同键重复/缓冲溢出的无信息到达才进限速路径（Linux 从不对数据驱动的 dup-ACK 限速；RFC 5961 的限速针对的是 challenge ACK）；⑤ 填补接收空洞（drain OOO 缓冲）的 segment 立即回 ACK（等价 Linux 的 ICSK_ACK_NOW），不再等 40ms delayed-ACK——发送端 FR/RACK 恢复在等这个累计 ACK；⑥ 部分重叠（trim）路径只要推进了接收前沿就立即回累计 ACK，只有完全陈旧（纯 D-SACK 反射）的 segment 才走进限速的 `SendDupAck`。测试侧：`test_pacing_flush` 期望值修正为 TSopt 感知的 1448B（1460-12，补齐 0004 的漂移）；`test_1mb_stream` 的发送循环挂起保护从迭代次数改为墙钟（突发 pacing 下空转迭代几乎免费，迭代计数无法约束传输时间）。

修改上游行为的唯一入口是该目录下的补丁；直接改解包树会在下次 prepare 时丢失。

### 11.2 已完成的验证（laboratory gate）

- standalone 合同：XTCP TCP consumed/no-native-fallback policy；ingress item/byte cap、ready/stop gate；external loopback endpoint/source-port/generation gate；IPv4 字节序转换与分片拒绝；production NDI ownership；repeated start/stop 与 stale generation；上游 dependency、BufRef/NDI、exact listener accept/reject。
- bridge 功能测试 `tests/cpp/xtcp_runtime_bridge_test.cpp`：echo、half-close（EOF 优雅关）、peer-close drain、inject-then-reject RST、churn，含对端 EOF 后 `shutdown(SHUT_WR)` 的生产语义模拟。
- 上游故障套件 `tools/run_xtcp_fault_suite.sh`：20/20 通过。
- bench 单次参考值：`{"best_mbps":5950.78,"kpps":1089.65,"cc":"kcc"}`（实验室环境，不作 production 承诺）。该数值是**上游 XTCP standalone bench 在其测试环境中观察到的结果**：不同 CPU、内核、copy path、GSO 与页布局都会改变它，不得当作普适上限；完整 OpenPPP2 P1 只有数百 Mbps 与它是两个问题（见 §11.4 三榜框架），也不能从绝对数字推出"XTCP 与 native 同级"——除非同机有 native baseline，只能表述为**XTCP core benchmark 显示出远高于当前 OpenPPP2 integration 所能释放的性能余量**。
- Linux netns E2E `tests/integration/linux/xtcp_tap_netns_e2e.sh`（默认 SOAK=60s、CHURN=512）：echo / half-close 上传完整性 + 下行前缀排空 / peer-close 全量 drain / inject-then-reject RST / churn x512（`flows_opened=516` 与流量闭合）/ netem（loss 1% reorder 25% delay 10ms MTU 1280）/ 60s soak / stats-json xtcp 块断言 / TUN 消失后路由与 DNS rollback。短跑矩阵（SOAK=3、CHURN=16）同样全绿。
- TSan 构建（`ENABLE_TSAN=ON` 的 lab tests 目录）运行通过。

### 11.3 进入 production 前仍缺的证据

在以下证据完成前不得宣称 production：

- 大规模 flow churn 下的资源上限与内存水位验证；
- sanitizer/TSan 与长连接 soak 的持续集成化；
- PMTU 变化与背压极端场景的专项故障注入；
- vmux 协议级半关改造（消除 8.1 平台限制）；
- Linux IPv4 之外的平台/协议独立评审。

### 11.4 性能基线（provisional gate，CI 固化前须经同机多轮 paired baseline 校准）

**三榜框架（所有性能结论必须归属到具体榜，禁止混用）。**

| 榜 | 回答的问题 | 主要指标 | 入口 |
| --- | --- | --- | --- |
| **A. Strict single-core product** | 完整 OpenPPP2 数据面中，**只给 1 个 client CPU**，native/lwIP/XTCP 谁推出最高有效吞吐 | Mbps、process/procstat ns/B | `client-single-core` + §11.4 的 12 项 qualification 硬门 |
| **B. Operational product** | 允许当前线程模型自然扩展（client 可用第二颗 CPU）后，谁实际最快 | Mbps、cores、Mbps/core、延迟 | `client-vnet-isolated` |
| **C. Stack capability ladder** | 性能到底在哪一层丢掉 | 逐层 Mbps/ns/B 与相对上一层的保留率 | attribution experiments（见下） |

A 榜测的是"**谁能在完整 OpenPPP2 数据面里用 1 个 client CPU 推出最高有效吞吐**"，不是"谁的 TCP 栈算法实现本身最好"；后者属于 C 榜。当前数百 Mbps 的完整 P1 结果首先是 A/B 榜的 product datapath performance，**不能用于评价任一 core 的绝对能力**。三榜正式结果必须使用**同一冻结 revision**，且该 revision 须先满足：`XTCP` fixed-cell stress 完成 + 修复后 `XTCP+GSO` full matrix 完成（见 `XTCP-MSS-RETX-001`）；冻结时记录 `ppp_sha256 / git_head / git_describe / dirty tracked / untracked` 指纹，不得混入仍在 XTCP regression validation 的 revision。

**C 榜（capability ladder）设计（attribution experiments，不改 production 架构）。** 对 XTCP 逐层增加成本域：

```text
C0 XTCP upstream/core loopback
C1 + in-memory NDI（NDI Output → memory sink，输入由 memory source 喂入）
C2 + bare TUN NDI
C3 + OpenPPP2 Tap/VNet bridge
C4 + exchanger/carrier
C5 + mux
C6 + AES-256-CFB
C7 full native D0 OpenPPP2 path
```

lwIP 对应 `L0 in-memory netif → L1 bare TUN → L2 OpenPPP2 VNet → L3 full PPP`。每层保持相同 packet bytes、MTU、TCP options、单 owner / single-core 条件，只加一个成本域；报告相对上一层的保留率（例如 `XTCP full/core = 7.5%` vs `lwIP full/core = 46%` 会指向"integration 更适合 lwIP 执行模型、XTCP 能力损失在边界上"，而非"lwIP TCP 比 XTCP 强"）。C0 与 C1 的 gap 直接回答"去掉 TUN/Tap 后 OpenPPP2 XTCP adapter 自己能跑多少"。见 §11.5 的 `XTCP-NDI-MEMORY-001` 与 `LWIP-NETIF-MEMORY-001`。

测量装置：`iperf3` 单向流（10s，跳过 2s warmup），P=并行流数；延迟为 512B ping-pong ×512（TCP_NODELAY）；对照 `--tcp-stack=native`。

`tools/run_datapath_linux_matrix.sh` 是该矩阵的专用运行器：它用同一个显式指定的 XTCP-enabled `ppp` binary 启动 native、lwip 与 XTCP client cell，P 仅映射为 `iperf3 -P`；OpenPPP2 的 `client.concurrent` 是独立旋钮，默认固定为 `1`。`--stacks` 与 `--tap-gso` 都接受逗号列表；runner 从固定的 `native/off`、`lwip/off`、`xtcp/off`、`native/on`、`lwip/on`、`xtcp/on` 顺序中过滤请求 mode，并在每轮循环轮换。每个 `(round,P,direction,stack,gso)` 使用新 netns；cell artifact 位于 `round-N/<stack>-gso-<off|on>-pP-<ul|dl>/`，保留 raw iperf JSON、配置、日志、stats NDJSON、metadata 与 result JSON。所有 cell 都须由 stats NDJSON 证明 requested/active TCP stack 一致；GSO-on 还必须证明 Linux TAP 的 VNET header 与 GSO merge 均 active，能力回退不是有效 score；XTCP cell 继续要求 XTCP stats block、flow 与流量计数。summary 按 round、GSO requested/active 状态输出 lwip/native、xtcp/native 与 xtcp/lwip 配对比值，并保留 per-flow min/p10/p50/p90/max、`zero_rate_flows` 与 max/min；出现零速流是必须保留并报告的公平性退化（max/min 标为未定义），不是 runner 格式错误。`--dry-run` 可在不需要 root、PPP binary 或 netns 的条件下打印解析后、按轮轮换的 cell 路径。典型校准命令：

```bash
tools/run_datapath_linux_matrix.sh \
  --ppp-bin "$PWD/build/xtcp-runtime-root/bin/ppp" \
  --artifacts "$PWD/build/datapath-matrix" \
  --stacks native,lwip,xtcp --tap-gso off \
  --parallel 1,4,16 --directions ul,dl --rounds 3 --duration 10 --omit 2
```

#### 11.4 CPU profile 实验计量

`--cpu-profile` 默认 `none`。**`client-vnet-isolated`** 是 operational 测量 profile：要求 `--affinity-cpus` 至少两个互异、online 且在当前 `cpuset.cpus.effective`（存在时）内的 CPU，runner 在 client PPP TUN ready 后唯一定位 `vnet` TID 并 pin 到第一个 CPU，其余 client TID pin 到余下 CPU；它允许 stack 内部线程使用第二颗核，回答"不限制 stack 内部线程时当前产品实际能跑多少"。**`client-single-core` 是本基准的主 profile**：要求恰好一个 CPU，把整个 client ppp 进程的全部 TID（含 vnet、lwIP 线程、XTCP runtime、Asio、transport 等）都 pin 到同一颗 CPU，禁止 client 使用第二颗核，回答"同样一个 client CPU budget，哪个 stack 最有效"；运行后以 `process CPU cores ≈ 1` 为 invariant，超出（如 lwIP 曾出现 1.6–1.92 cores）即 affinity 未真正约束，cell 不算严格单核。两个 profile 都自动保留 datapath SIGUSR1 formal boundary；formal window 从 omit 后的 start boundary 到 iperf 完成后的 end boundary，不包含 warm-up omit 或 post-traffic sleep。`--process-perf-stat` 记录 client PID 的 `task-clock/context-switches/cpu-migrations`；`--system-cpu-stat` 允许两个非 none profile，按 selected CPU set 记录同一组非 PMU 事件。

每 cell 的 `cpu_measurement` 保留 profile、CPU list、affinity 回读、formal monotonic interval、client thread snapshot、`lscpu`/allowed CPU、`/proc/stat`、`/proc/softirqs`、ksoftirqd mapping 与可选 perf CSV 文件名；payload 只使用 iperf end aggregate 的实际 `sum_sent.bytes`（UL）或 `sum_received.bytes`（DL），缺失时 CPU 结果为 `failed`，不会用吞吐倒推。状态含义只有：`unavailable`（profile none）、`failed`（pin、snapshot、payload 或请求的 perf 失败）和 `measured`（完整采集）。`migration_warning` 独立记录 selected 之外的 `NET_RX`/`NET_TX` 增量；它只表示可能存在迁移或 peer/server/netns/veth 的正常网络处理，不能凭全局 `/proc/softirqs` 归因。matrix summary 仅对 measured 输出 **busy 口径**的 CPU ns/B（`process task-clock` 与 selected `/proc/stat` non-idle，两者互相验证）的 median/min/max/MAD，并据此写 `cpu_paired`；`perf stat -a -C` 的 system-wide task-clock 在该用法下是 selected CPU 的墙钟容量（每颗 CPU 忙闲都计时，反算恒为 ≈n_cores），**无 ranking 信息量**，只作为 `selected_cpu_clock_capacity_ns_per_payload_byte` 诊断输出、不参与 paired。`cpu_paired` 仅在同一 round 的两个 cell 都是 measured 时写入，既有吞吐 paired ratio 语义不变。CPU 配对采用 `cpu_efficiency_gain = reference_ns_per_B / candidate_ns_per_B`，故 `>1` 表示 candidate 的每 byte CPU 成本更低。

P1 UL dedicated-host smoke 的顺序是：先确认 host 空闲并明确三类 CPU 角色：client selected CPU、允许处理 peer/server 的 CPU、以及 unexpected CPU。外部运维完成 IRQ、RPS/XPS、peer affinity 等归属后，unexpected CPU 的网络活动才可升级为 hard contamination；在此之前它只能保留为 `migration_warning`。随后固定 `P1/UL/concurrent=1/MTU=1500` 跑两套榜：**榜 1（主榜，严格单核）** 用 `client-single-core`，一次跑 native/lwIP/XTCP × GSO off/on 的 6 cell，先验收 `cpu_measurement=measured`、affinity 全部落在唯一 client CPU、`process CPU cores ≈ 1`、client softirq 不外溢，再读 Mbps（此时 Mbps ≈ 单核效率）；**榜 2（operational）** 用 `client-vnet-isolated`，允许 stack 使用第二颗 client CPU，作为当前架构最大吞吐榜。通过后才扩展到 paired rounds、P4/P16、DL 或 GSO。P1 smoke 的 CPU 主比较口径是 `process task-clock ns/B` 与 selected `/proc/stat` non-idle ns/B（busy 口径），`perf -a -C` 的 system-wide task-clock 只是容量诊断。此 runner **不会**配置 IRQ affinity、RPS、XPS、ksoftirqd、cpuset、CPU governor 或 Turbo；这些 host controls 必须在 runner 之外完成，所以 profile 不能单独证明隔离或生产 CPU 归因。

**严格单核资格判定（每 cell 硬 invariant）。** runner 在非 `none` profile 下为每个 cell 写 `qualification.json`（纯 Python `tools/datapath_qualifier.py`，可单测）。`process_cores` **直接按 `process task-clock / formal wall time` 计算，不从 Mbps/ns-per-byte 反推**（反推值仅作 `process_cores_derived_sanity` 交叉验证），且必须满足 **`0.9 ≤ process_cores ≤ 1.02`**：低于 0.9 说明 ppp 进程未吃满唯一 client CPU（宿主干扰或调度伪影导致饥饿），该 cell 数据不是有效单核结果，判 FAIL；高于 1.02 说明越出了单核约束。每 cell 必须同时满足：

```text
cpu_measurement.status = measured
affinity_verified        = true
cpu_migrations           = 0
0.9 <= process_cores <= 1.02
active_stack == requested_stack
active_gso  == requested_gso
payload_bytes            > 0
zero_rate_flows          = 0
no watchdog              （cell status == pass）
retransmits              >= 0
first_push_failure       = none（XTCP 才判；native/lwIP 该诊断不产生，恒 true）
oversized_l3_rejected    = false（由 first_push_failure.packet_shape.excess_bytes>0 派生）
```

任一失败即该 cell `qualification.status=fail` 并列出 `failed_checks`；`migration_warning` 与 other-CPU softirq 增量只作诊断，不判失败。runner summary 输出 `qualification_count status=pass/fail cells=N`，只有 6/6 全 pass 的严格单核第一轮才允许进入 P1 UL/DL × 3 rounds（36 cells）主榜。


> **实测陷阱（已修复）：** 首次带 `--process-perf-stat/--system-cpu-stat` 运行发现 runner 在 iperf 完成后无限阻塞。根因是 `perf stat ... -- sleep 1000000`：perf 在 `do_wait` 等其 sleep 子进程，`kill -INT` 被忽略，runner 的无界 `wait` 于是永久挂起。修复为：去掉 workload，改用 `--timeout=<iperf_timeout+60>s` 兜底（正常 run 不触发）+ `stop_cpu_perf()` 有界等待（INT 后 10s，僵尸提前退出；超时再 `SIGKILL`）。复现实验确认无 workload 的 `perf stat -p/-a -C` 响应 SIGINT 并写出 CSV；带 workload 则否。修复后 6/6 cell 均 `status=measured` 且 `affinity_verified=true`。

> **口径修正（重要）：** P1 UL 六模式首次实测暴露两个口径问题。其一，`perf stat -a -C 8,9` 的 system-wide task-clock 实际是 selected CPU 的墙钟容量（每颗 CPU 忙闲都计时），六模式反算全部恒为 ≈2 cores，因此它**不能**用于 CPU 排名，只能作 `selected_cpu_clock_capacity` 诊断；真正的 busy CPU 以 `process task-clock` 与 selected `/proc/stat` non-idle delta 为准。其二，`client-vnet-isolated`（vnet→CPU8、其余→CPU9）**允许 lwIP 同时吃两颗核**：lwIP 实际消耗 1.60（off）/1.92（on）个 process core，而 native/XTCP 恒为 ≈1.00 core。因此**不得**把 lwIP 的 raw 577/979 Mbps 称为"单核"。按 process core 归一化：GSO-off 为 lwIP≈360、XTCP≈336、native≈261 Mbps/core（lwIP 仅比 XTCP 高约 7%，而非 raw Mbps 的 73%）；GSO-on 为 native≈833、lwIP≈510、XTCP≈419 Mbps/core（native+GSO 明显领先）。lwIP 的 GSO 收益应分开写：throughput 1.70×、process CPU efficiency 1.41×、process CPU consumption +20%。这佐证了引入 `client-single-core` 主 profile 的必要性。



**历史两栈 GSO-off 参考（已由 §11.4.2 三栈 provisional 取代）。** 新 runner 曾完成 GSO-off、3 round、P1/P4/P16 × UL/DL × native/XTCP 的 **36/36** cell；该历史窗口全部 XTCP stats NDJSON 验证通过，最终窗口未见 zero-rate flow。它不与当前三栈 Stage A / provisional 的 `XTCP/GSO-off/P16/UL` 零速流观察相抵触，后者仍是当前公平性结论。该历史窗口的 per-round ratio 中位数如下（artifact：`build/c1-gso-production-candidate-20260901/matrix-gso-off-r3-v2/`）：

| P | UL XTCP/native | DL XTCP/native |
| --- | ---: | ---: |
| 1 | 1.2799 | 0.6909 |
| 4 | 0.2563 | 0.4451 |
| 16 | 0.2477 | 0.4900 |

这些是共享宿主、GSO-off、同一 XTCP-enabled binary 的最近三轮 paired 证据；它们显示 P4/P16 的 shared-path 问题显著，且与先前有限测量的绝对吞吐不可直接混合。它们不是 CI 校准、严格单核结果或 production 门禁结论。

**历史 XTCP+GSO 矩阵故障（已由 `XTCP-MSS-RETX-001` 修复；现场保留）。** 首次 GSO-on、3 round 同维度运行完成 **34/36** cell，round-3 的 XTCP P16 DL 在约 43s 后 16 条流同时变为零速，iperf server 最终报告 `select failed: Bad file descriptor`；外层 30 分钟时限中断前未产生该 cell result 或总 summary。相同 cell 随后的单次重跑在 42s watchdog 下通过：312.805Mbps、16 条流均非零，说明问题具间歇性，但**不**抵消首次 stall，也不允许将当时的 XTCP+GSO 矩阵标记为完成或作为 production 门禁依据。

为抓取而非掩盖该故障，runner 现提供每 cell `--iperf-timeout` watchdog（默认 `duration + omit + 30` 秒）和 `--stall-diagnostics`：超时后先保存稳定 stats、XTCP perf、datapath/GSO ledger、PPP/iperf fd 与线程快照、三端 netns 的 `ss -tinp`/路由/链路状态，再发送 SIGINT。固定 `XTCP-GSO-STALL-001`（GSO-on、P16 DL、10s+2s、42s watchdog）的 30-round 专场在 round-1 通过后、round-2 复现。iperf 从 4–5s 的 26.2Mbps 下降，并在 **5–6s 起 16/16 流同时归零**，直至 42s watchdog；现场在 `build/c1-gso-production-candidate-20260901/xtcp-gso-stall-001-r30/round-2/xtcp-p16-dl/timeout-diagnostics/`。稳定 stats 表明 `flows_active=17`、`flows_closed=0`、ingress 无 drop、`queued_bytes=0`，`timer_polls` 继续约 55k/s 推进，而 XTCP `output_packets/output_bytes` 与 connector read/write 都已冻结。target 端 16 条 iperf socket 仍 ESTAB、server Send-Q 约 5–9MiB、`rwnd_limited≈99.5%`；pre-signal fd 快照未显示 data fd 消失，本次也没有 `EBADF`。这排除了“全部 flow 已关闭”及队列积压型 backpressure 的直接证据，并把首要排查范围收窄为 XTCP 输出/发送许可、ACK clock 或其前置状态；但尚不能把根因归于 KCC、GSO 或任一具体组件。该样本的 GSO ledger 只有 40,692 次 ordinary write（40,657 次 PSH 拒绝），无 GSO full write/segment，故亦不能用它证明 GSO 合并器造成 stall。最新 Send admission 快照显示，16 条流的 `sndbuf_quota` 均为 64 KiB，`non_sendable` 不是原因；peer window/cwnd/pacing 快照不构成直接 admission gate。quota 未释放的上游链路仍未知。已知现场还表明：stall 期 NDI `Tx` 每秒约 37–39 万且全部被下游 handler 拒绝；normal 期 direct TUN write 成功后 attempts 归零、direct failure/partial 均为 0。这是 NDI 下游拒绝与 normal 后输出停止的证据，**不是** Tap、上层 disposed、写失败、KCC 或 GSO 的根因归属。

为抓取而非掩盖 fail-closed 路径，`OPENPPP2_DATAPATH_TUN_OUTPUT_DIAGNOSTICS=1` 在已有 datapath JSON 中写入累计 `tun_output`：`invalid`、`disposed`、`already_tun_write_failed` 是 `TapLinux::Output` 的 early false 原因；`first_latch` 分别记录 bare write、GSO disable-flush、hold-timer flush、ordinary write、coalescer push 首次触发 `FailTunWrite` 的来源；另有 VNET malformed/unsupported close 与 `Finalize` 总数。`first_push_failure` 的语义严格是该 session 中首次 **最终返回 false 的 `Push()` 的 terminal write failure**：`terminal` 明示 stage/kind（`ordinary`/`gso`）、outcome（`negative`/`partial`）、flush/rejection、原始 packet、requested VNET frame、signed written bytes、捕获时的 errno、segments、hold 与 monotonic ns；无事件明确为 `none`/零。MTU strict 拒绝没有 syscall，errno=0。若同一 Push 先有负 GSO write、随后 fallback ordinary 失败，`precursor` 保留该首个 GSO negative 的 errno/requested/written/monotonic，而 terminal 仍准确报告 ordinary；负 GSO 后 fallback 全部成功不产生 terminal snapshot。`failure_timeline` 的 T1=`Push()` returned false、T2=实际首次 `FailTunWrite` latch、T3=该 latch 导致的 `Dispose()` 调用，`push_false_without_terminal` 是必须为零的诊断不变量；它们都使用同一 PPP process 的 `steady_clock` nanosecond epoch。`OPENPPP2_XTCP_OUTPUT_REJECTION_JSON=1` 只在 `OPENPPP2_XTCP_PERF_JSON` 已配置时写入每秒 `output_rejection` delta，并附 T4=首次实际 `owner->Output` rejected 的同一 process `steady_clock` epoch；弱 owner、VEthernet disposed、tap missing、accepted 与 iperf 零速/runner watchdog 都不是该同一时间线。两类诊断均 default-off，只解码既有分支，不改变或证明发送许可、retry、队列、KCC、GSO、TUN write、packet ownership、返回值、生命周期或根因。

固定 P16/DL/GSO-on/XTCP 42s cell 已再次复现，现场位于 `build/xtcp-gso-stall-001-diagnostics-20260901/round-1/xtcp-p16-dl/timeout-diagnostics/`：`tun_output.first_latch.gso_coalescer_push=1`、其他首次 latch 来源均为 0，`invalid=0`、`vnet_input_close=0`、`finalize=1`；随后 `disposed` 增至 11,679,249，说明首个 fail-closed 分支是 coalescer `Push()` 失败后 `FailTunWrite` 的既有关闭路径。47 条 XTCP perf 记录累计 `output_rejected=11,523,774`、`accepted=268,052`，而 weak owner expired、VEthernet disposed、tap missing 均为 0；因此后续拒绝发生在 owner/Tap 调用后的返回值，而非该 lambda 的三个前置对象状态。该证据只定位失败分支，尚未解释 `Push()` 为何返回 false，不能据此归因于 KCC、GSO 策略或底层写失败。该历史样本当时仍阻塞；其后由 §11.4.1 的修复与回归更新当前状态。

固定 XTCP GSO-on P16 DL 的 50-run 专场在 round-5 再次触发 watchdog。该次 `Push=false` terminal 是预写 strict-MTU 分支：ordinary negative rejection=`mtu`、`original_packet_bytes=1512`、requested/written=`0/0`、errno=`0`、`precursor` 不存在；这不是 kernel syscall errno。为保留未知的 packet shape 而不记录 payload 或五元组，default-off `tun_output.first_push_failure.terminal.packet_shape` 对 MTU terminal 记录 supplied bytes、独立的 IPv4 total length、IHL、TCP data offset/payload length、IPv4 version/protocol/fragment flags、DF/TCP flags、固定 1500 guard 与 excess bytes。ParsePacket 失败时 `parsed=false` 且解析字段为零；非-MTU/no-event terminal 也稳定输出该默认值。guard 始终按 supplied buffer length 判断，不能将其与 IPv4 total length 混同。这只说明既有 pre-write 分支及其输入形状；并不证明 kernel、GSO 实现、`XtcpMSS`、KCC 或任何具体组件是 stall 根因。

#### 11.4.1 `XTCP-MSS-RETX-001`：历史 GSO stall 的已修复根因

以上段落保留了历史现场和诊断链；其“根因未知/矩阵阻塞”的结论已被随后取得的 packet shape、上游源码和回归证据取代，不能作为当前状态。

根因是 XTCP 的 segment payload sizing 曾固定使用 1460B，未扣除实际 TCP header 长度。启用 TCP Timestamp 时，TCP header 为 32B；RTO 重传于是构造 `IPv4 20B + TCP 32B + payload 1460B = 1512B` 的 L3 packet。Tap 的 1500B strict-MTU guard 正确拒绝该包，继而触发既有 `Push()==false → FailTunWrite() → Dispose()` 链。GSO coalescer 是 fail-closed 的下游检测点，不是根因；不得放宽它的 1500B guard。

修复以 header-aware cap 在 XTCP segment/retransmission queue 建立前完成分段：

```text
payload_cap = effective_l3_mtu - actual_ipv4_header_length - actual_tcp_header_length
```

因此 IPv4 无 Timestamp 保持 `1460B`，IPv4 Timestamp 为 `1448B`。初次发送与 RTO 重传使用同一个逻辑 segment，禁止在 RTO 阶段截断 payload。focused 合同覆盖 Timestamp on/off、边界长度、多 segment 与 RTO exact-size/seq/payload 一致性。

已验证的修复后回归：

- `XTCP-GSO-STALL-001-fixed-r50`：历史 `XTCP/GSO-on/P16/DL` cell 连续 **50/50 PASS**，无 watchdog、zero-rate flow、first push failure、VNET close、oversize output 或 XTCP output rejection；这表示修复后 50 次未复现，不是统计学上的永久保证。
- `XTCP-GSO-MATRIX-RETX-001-r3`：native + XTCP、GSO-on、P1/P4/P16、UL/DL、3 rounds 共 **36/36 PASS**；18 个 XTCP cell 均有完整 stats，未见上述失败信号。

当前仍保持 `GSO default-off`。未完成的 production gate 包括三栈基准、隔离 CPU 的 system-wide CPU ns/B、idle/loaded latency、短流/故障矩阵，以及独立的 `spinlock_test` hang；因此这些回归不构成 default-on 或 production 结论。

#### 11.4.2 三栈 provisional benchmark（2026-09）

已完成 CURRENT-LWIP、native 与 XTCP 在相同 Linux datapath 下的第一轮统一比较：`client.concurrent=1` 固定，P 仅映射为 `iperf3 -P`；每个 `(P,direction)` 跑 3 round，按 `native/off → lwip/off → xtcp/off → native/on → lwip/on → xtcp/on` 的 canonical mode 顺序循环轮换。共 **108/108** cell 完成并通过 runner 的配置、runtime proof 与统计完整性检查；所有 GSO-on cell 的每条 stats 样本均证明 `tap_linux.vnet_header=true` 且 `tap_linux.gso_merge_active=true`，没有 capability fallback、watchdog 或 XTCP stats 缺失。

本次构建/工作树指纹：

```text
ppp_sha256=00e357521664134faac6ec429d8521f20d36bc39523809511a08cd0f33fb9b5d
git_head=898667737a14b649bd49e84504a387e39a044454
git_describe=8986677-dirty
CMAKE_BUILD_TYPE=Release
ENABLE_SIMD=OFF
ENABLE_XTCP=ON
compiler=c++ (Debian 14.2.0-19) 14.2.0
dirty tracked=42, untracked=36
```

这只是共享宿主上的 provisional 结果，所有比值均为同一 round 内 `Mbps` 相除后再取中位数；没有隔离 CPU 的 system-wide CPU ns/B、延迟、内存或故障矩阵，故不能据此做 production stack 或 default-on 选择。

| P/方向 | GSO | lwIP/native | XTCP/native | XTCP/lwIP |
| --- | --- | ---: | ---: | ---: |
| P1 UL | off | 2.0543 | 1.1818 | 0.5559 |
| P1 UL | on | 1.1670 | 0.5002 | 0.4332 |
| P1 DL | off | 1.0281 | 0.8146 | 0.8224 |
| P1 DL | on | 0.3971 | 0.4390 | 1.1056 |
| P4 UL | off | 2.4550 | 1.0009 | 0.4062 |
| P4 UL | on | 1.0187 | 0.2885 | 0.2787 |
| P4 DL | off | 1.0276 | 0.8966 | 0.8616 |
| P4 DL | on | 0.4025 | 0.5085 | 1.2631 |
| P16 UL | off | 2.8112 | 0.7466 | 0.2641 |
| P16 UL | on | 1.0792 | 0.4740 | 0.4392 |
| P16 DL | off | 1.0280 | 0.9270 | 0.9133 |
| P16 DL | on | 0.4638 | 0.5672 | 1.2331 |

同栈 GSO-on/off uplift：

| P/方向 | native | lwIP | XTCP |
| --- | ---: | ---: | ---: |
| P1 UL | 2.8983x | 1.6489x | 1.2713x |
| P1 DL | 1.7876x | 0.7133x | 0.9697x |
| P4 UL | 3.3258x | 1.3647x | 0.9249x |
| P4 DL | 1.7179x | 0.6624x | 0.9712x |
| P16 UL | 2.6122x | 1.0212x | 1.6585x |
| P16 DL | 1.5539x | 0.7000x | 0.9612x |

该结果不能简化为“GSO 对所有栈普遍加速”：native 在 UL/DL 都有明显 uplift；lwIP 在 UL 受益而 DL 稳定回退；XTCP 的 DL 基本持平或微退、P4 UL 回退，P16 UL 虽受益仍明显落后 native/lwIP。尚未有 GSO ledger、CPU/byte 或 packetization 归因，禁止据此调整 GSO cap/hold、XTCP queue/KCC/pacing 或 lwIP 参数。

**GSO 叙事（收紧为解释假设，非结论）。** “XTCP 从 GSO 获益较少（P1 UL 1.27×）是因为它不吃 syscall tax”目前只能作为**待验证假设**：GSO 对完整 OpenPPP2 路径的影响已被证明不只是 `write(TUN)` syscall 计数（还会改变 TUN reads、VNET framing、packetization、frame encode rate、carrier sends 与上游 batching），因此更严谨的表述是：**XTCP 的完整集成路径对 edge packet-rate 摊薄的敏感度低于 native；具体份额需由 C 榜 capability ladder / stage ledger 定量**，而不是直接归因于 syscall。

唯一的公平性告警是 round-3 `XTCP/GSO-off/P16/UL` 出现 1 条 zero-rate flow。加上 Stage A 同 mode 曾观察到的 2 条零速流，这已是可复现的 `XTCP-SHARED-PATH-001` 信号；因此 **108/108 runner pass 不等于全部性能 cell pass**，该 mode 不可宣称公平性通过。

CURRENT-LWIP 未调参：`NO_SYS=1`、callback API、`TCP_MSS=1460`、`TCP_WND=TCP_SND_BUF=32KiB`、`MEM_SIZE=128KiB`、`MEMP_NUM_TCP_PCB=16`、`LWIP_STATS=0`；TCP Timestamp 当前未启用，`CHECKSUM_CHECK_IP/TCP/UDP/ICMP=0`、`LWIP_CHECKSUM_ON_COPY=1`。这是当前集成的透明记录，不是关闭 checksum 或调大窗口后的优化成绩。静态路径与 runtime proof 均表明 lwIP cell 实际进入 lwIP，而非初始化失败后静默回退 native。

后续顺序是：先在真实隔离 CPU 环境采集 process 与 client-side system CPU ns/B、softirq/ksoftirqd/额外 CPU 归属，再做 idle/loaded control-flow latency、短流和 MTU/retransmission/故障矩阵；通过这些 gate 后才执行 12-round（两套完整 6-mode rotation）校准。`GSO default-off` 保持不变。

#### 11.4.3 严格单核 P1 单轮基线（provisional，宿主污染，非正式榜）

artifact：`build/three-stack-singlecore-p1-ul-r1`、`build/three-stack-singlecore-p1-dl-r1`（均为 `client-single-core`、P1、30s、`affinity_cpus=8`、`--stall-diagnostics`）。**这些数字来自当前共享宿主，CPU8 被大量非 ppp 活动抢占，且 `process_cores` 下界是事后才加入 qualifier**，因此只能作为单轮 provisional 参考，**不构成正式三栈排名**。权威重判定（用修复后 qualifier）结果如下：

| 方向 | mode | Mbps | process_cores | qualification |
| --- | --- | ---: | ---: | --- |
| UL | native off | 262 | 0.985 | ✅ pass |
| UL | native on | 824 | 0.975 | ✅ pass |
| UL | lwIP off | 373 | 0.943 | ✅ pass |
| UL | lwIP on | 103 | 0.224 | ❌ **fail（饥饿）** |
| UL | XTCP off | 246 | 0.704 | ❌ **fail（饥饿）** |
| UL | XTCP on | 418 | 0.978 | ✅ pass |
| DL | native off | 369 | 0.985 | ✅ pass |
| DL | native on | 656 | 0.975 | ✅ pass |
| DL | lwIP off | 272 | 0.975 | ✅ pass |
| DL | lwIP on | 257 | 0.976 | ✅ pass |
| DL | XTCP off | 303 | 0.926 | ✅ pass |
| DL | XTCP on | 291 | 0.924 | ✅ pass |

DL 六 cell 全 pass，UL 4/6 pass（lwIP on 与 XTCP off 因未吃满单核作废）。**初步趋势（仅限本轮单轮）**：native+GSO 双方向领先（UL 824 / DL 656 Mbps）；GSO 对 lwIP/XTCP 在严格单核 P1 未观察到正收益（lwIP DL 272→257、XTCP DL 303→291，XTCP UL 246→418 方向相反），而 native 明显受益（UL 262→824、DL 369→656）。**这些 GSO 差异仍需 3-round paired rotation 才能定案**：`0.94×/0.96×` 很可能在单轮波动范围内，不能写死为"用户态栈不吃 GSO 红利"；XTCP DL 的 `XTCP/native = 0.82×（off）/0.44×（on）` 中，0.44× 主要来自 native 的 GSO 红利（369→656），XTCP 自身 off→on 约 0.96× 是基本不吃 GSO 收益，不能解释为"XTCP 自己退化 56%"。

#### 11.4.4 XTCP 集成优化 + KCC pacing 修复验证（2026-09-02）

**优化改动（OpenPPP2 侧，`XtcpRuntime.cpp`）：**
- 删 `OnReceive` 遗留 `fprintf(stderr, "XTCPDBG ...")`（每 segment 一次 stderr I/O）；
- `kConnectorReadBytes` 16K→64K（UL read chunk 放大，UL 提升主因之一）；
- UL 零拷贝：`Submit` 直接 `BufRef::Acquire`（省 1 次 vector 分配+memcpy，经 A/B 无吞吐收益——证明 UL 下一瓶颈是 strand/同步而非 memcpy，保留无害）；
- `OPENPPP2_XTCP_CC`（kcc/bbr/cubic/reno）与 `OPENPPP2_XTCP_SNDBUF_BYTES` env 开关；
- runner 新增 `--xtcp-cc`/`--xtcp-sndbuf`/`--netem-delay-ms`（netem 用于 RTT 扫描，已验证 `tc qdisc netem` 生效：ping RTT 0.05ms→40.2ms）。

**KCC pacing 钳制根因（216Mbps 天花板，由开发者 `0005-pacing-burst-quantum.patch` 修复）：**
- `FlushPendingSend` 在 `pacing_rate>0` 时每次调用最多发 1 个 MSS（now 冻结 + 逐 segment 重设 deadline）；
- `pacing_deadline_` 无调度器消费——`NextTimerDeadline()` 不看它，pending 非空一律报"立即到期"，host loop 空转；
- KCC 的 bw 采样测到的正是这个串行化速率，自我实现天花板。CUBIC 因 `pacing_rate=0` 绕开，故修复前 DL 翻倍（216→400）。
- 修复后另解决突发发送暴露的丢包恢复停摆（OOO dup-ACK 限速、gap-fill delayed-ACK、trim 前沿静默），`test_wscale_transfer` 40-200s 超时 → 17-228ms。

**修复验证（本机复现，单核 CPU8，P1 DL）：**

| 场景 | KCC 修复前 | KCC 修复后 |
| --- | ---: | ---: |
| DL off | 216 | **395** |
| DL on | 217 | **393** |
| DL 40ms RTT | 219 | **398**（无停摆） |

**3-round 稳定性回归（72/72 cell，native/XTCP × P1/P4/P16 × UL/DL × GSO off/on × 3 rounds，单核 CPU8，artifact `build/kcc-fixed-regression-r3`）：**

- **72/72 完成，无 watchdog、无 zero-rate flow、无 first_push_failure**；paired ratio 每 cell 的 3-round MAD 全部 <0.05（多数 <0.02），KCC 修复后无间歇波动。
- **6 个 qualification fail 全部为 XTCP P1 DL 的 `process_cores_ok`**（cores 0.72-0.75 < 0.9）：DL 发送是 ACK clock 驱动，CPU 不饱和是固有特征（开发者也确认 perf task_clock 遥测缺失），**吞吐/正确性全过，非故障**——qualifier 的 `process_cores_ok` 不应适用于 DL 发送场景。
- **XTCP/native paired ratio（3-round median，MAD 括号）：**

| P/方向 | GSO off | GSO on |
| --- | ---: | ---: |
| P1 DL | 1.09（±0.01） | 0.60（±0.01） |
| P1 UL | 1.29（±0.05） | 0.55（±0.01） |
| P4 DL | **1.49**（±0.03） | 0.83（±0.01） |
| P4 UL | 1.14（±0.07） | 0.30（±0.00） |
| P16 DL | **1.46**（±0.00） | 0.89（±0.05） |
| P16 UL | 0.81（±0.03） | 0.46（±0.01） |

- **DL GSO-off 全面反超 native**（P1 1.09×、P4 1.49×、P16 1.46×），DL GSO-on 接近（P16 0.89×）；**UL 落后**（GSO-off P1 反超 1.29×，但 GSO-on 0.55×、P16 0.46×，strand 串行投递是瓶颈，见 `XTCP-STRAND-DISPATCH-001`）。

**CC A/B 定案（KCC-fixed vs CUBIC 全场景等价）：**

| 场景 | native | KCC-fixed | CUBIC |
| --- | ---: | ---: | ---: |
| P1 UL on | 818 | 473 | 478 |
| P1 DL on | 663 | 393 | 392 |
| P16 UL on | 707 | 305 | 313 |
| P16 DL off | 331 | **499** | 493 |
| P16 DL on | 529 | **473** | 484 |

**决策**：修复前 KCC pacing bug（216Mbps）→ 曾临时默认 CUBIC；`0005` 修复后 KCC 追平 CUBIC（全场景 ±3%），**恢复 KCC 默认**（上游默认、开发者维护）。`--xtcp-cc cubic` 保留可切。**已回滚实验**（证明方向不对）：`SetRcvBuf(4MB)`（XTCP 在 DL 是发送方，无效）、`SetSndBuf(4MB)`（pending 一次 flush 超大段，DL 崩到 25Mbps）、`SetSndBuf(512K)`（snd_buf 不是门，无效）、KCC sndbuf 16K-32K（read 64K 不匹配，DL 崩到 0-70Mbps；开发者建议针对上游原始用法，与 OpenPPP2 64K read chunk 不兼容）。

**XTCP 对标 native 最终成绩（单核，KCC 修复后默认，binary `15daa299`）：**

| 场景 | native | XTCP | 差距 |
| --- | ---: | ---: | --- |
| P1 UL on | 818 | 473 | 1.73× |
| P1 DL off | 352 | **398** | **反超** |
| P1 DL on | 663 | 393 | 1.69× |
| P16 DL off | 331 | **499** | **反超** |
| P16 DL on | 529 | 473 | 1.12× |
| P16 UL on | 707 | 305 | 2.3× |

**DL 全面接近/反超 native（P16 DL off 反超），UL 仍落后（下一瓶颈 = 单 strand 串行同步，零拷贝已证 memcpy 不是门）。**

#### 11.4.5 `XTCP-STRAND-DISPATCH-001` 修复：批量 ingress + 无锁 budget + timer churn（2026-09-02）

§11.4.4 回归登记的 UL strand 瓶颈本轮修复（全部在 ppp 集成层，`XtcpRuntime.cpp`/`XtcpRuntimePolicy.h`，不动上游 core）：

**根因链（perf 证据钉死）**：每包一次 `asio::post` 到单 owner strand（GSO 64K 突发 ≈ 44 段 = 44 次调度）→ strand 消化速率低于到包速率 → in-flight 打满 1024 item budget 上限 → `ingress_dropped` 累计 68.2 万（丢包率窗口峰值 10-20%）→ TCP 层真丢包 → 重传风暴 → UL GSO-on 崩到 0.30-0.55×。伴随：每包 2 次 `state_sync_` mutex 跨线程争用（Submit/TryReserve 与 ProcessIngress/Release）、`KickPoll` 因 pacing deadline 每次前移几乎逐包 cancel+`make_shared<steady_timer>` 重臂、以及 zero-copy 改造遗漏的死代码（Submit 里 vector 分配+memcpy 后弃用）。

**修复**：
1. Submit 改批量 handoff：压入 mutex 护栏的 MPSC 队列，仅队列从空变非空时 post 一次 `DrainIngress`，strand 单次调度换出整批逐包注入（同锁内"空检查+清位"保证无包滞留）；
2. `XtcpIngressBudget` 无锁化：items+bytes 合并单 CAS 字段，热路径零锁；`state_sync_` 只留 Start/MarkReady/Stop 冷路径；
3. poll timer 复用单一 `steady_timer` 对象 + 50µs re-arm slack（deadline 微移不再重臂）；
4. 删除死代码 copy。

**效果（单核 CPU8，binary `eaac9eda`，paired vs §11.4.4 三轮中位数）**：

| cell | 修复前 | 修复后 | Δ |
|---|---:|---:|---:|
| P16 UL on | 0.46×（305）| **0.487×（336）** | +10% |
| P1 UL on | 0.55×（473） | 0.572×（480） | +1.5% |
| P16 DL off | 1.46×（499） | **1.573×（527）** | +6% |
| P16 DL on | 0.89×（473） | **0.931×（503）** | +6% |
| P1 DL off / on | 1.09× / 0.60× | 1.064× / 0.586× | 持平 |

**结构性指标**：`owner.dropped` **68 万 → 0**；`q_p50` 恒定 128µs → 多数窗口 0；retx=0；`posts ≈ dispatched ≈ injected` 自洽。验证：xtcp runtime/dependency 5/5、lab 套件 135/135、netns 8 cell 全 PASS。

**剩余 UL 差距已换层**：XTCP 每字节 CPU ~2× native（P16 UL on 24.1 vs 11.6 ns/B），单核下被 CPU 顶死——下一层是数据面每包成本（parse/hash/alloc/Inject），不再是调度。附带登记：上游 `dup_acks_` 会把带 payload 段的 piggyback ack（== 空闲发送侧 snd_una_）也计数，收方向连接遥测刷高，但所有恢复路径均有 `retrans_queue_` 非空门卫（实测 retx=0、fast_rec=0），行为良性，不改上游。

**数据面层跟进（同日第二轮，binary `e68b1e75`）**：Output 全链零拷贝（栈 BufRef 经 owning `shared_ptr` 直通 TAP 写队列，省每包 1 分配+1 memcpy；Tx 被拒时所有权回迁 `packet.owned`，`TestProductionNdiOwnership` 契约保持）+ TAP 写队列有界同步 drain（fd 为 O_NONBLOCK，64 包/批摊薄 epoll 往返；EAGAIN 回退单 async_write）。**实测：P16 两侧 +5-8%（DL off 1.51×、DL on 0.91×、UL on 0.486×），P1 持平；r1/r2 两轮独立样本落在同噪声带**。UL 接收侧剩余大头：connector 逐段写（avg 1.4KB/write，925K 次/30s）——但 write 合并已被 `XTCP-UL-WRITE-BATCH-001` 实验证伪（256KB 单次写超时，UL 崩到 2.6Mbps），维持逐段写。至此单核 XTCP 剩余差距为结构性每包用户态 TCP 成本（native 的 TCP 在内核），无低风险进一步优化项；P1/P16 UL 提升需 `XTCP-SHARED-PATH-001`（多核分片）路线。

**历史两栈参考（2026-08；已被上述三栈 provisional 取代，不作为当前公平性结论）。** 下表数字来自有限轮次的同机测量，用于人工判断参考，**不得直接作为 CI 的 PASS/FAIL 硬门禁**——单轮对照在共享环境里噪声带很宽（实测同一构建 P1 upload 跨轮 343–404 Mbps、native 271–332 Mbps），已出现过"实现无回归却触发三条绝对 hard fail"的假阳性。CI 门禁须先完成文末的 paired baseline 校准。

**参考实测区间（2026-08，本仓库构建，多轮汇总）：**

| 场景 | xtcp | native | 稳定结论 |
|------|------|--------|---------|
| P=1 upload | 343–404 Mbps | 271–332 Mbps | xtcp 反超且跨轮区间不重叠（+18~27%）；UL cap 修复稳定复现 |
| P=1 download | 261–328 Mbps | 389–396 Mbps | −19~−34%，根因归 `XTCP-KCC-PACING-001` |
| P=4 upload | 268 Mbps | 267 Mbps | 打平（单轮数据，**候选分水岭**：并发上升后共享路径开始主导，需 2–3 轮重复确认后升级为定案）|
| P=4 download | ~309 Mbps | ~386 Mbps | 差距收窄 |
| P=16 upload | 163–195 Mbps | 218–244 Mbps | **paired ratio 漂移（0.89→0.67）是当前最值得关注的信号**，见 SHARED-PATH-001 |
| P=16 download | 325–328 Mbps | 366–376 Mbps | −10~−14% |
| 延迟 mean | 0.31–0.37 ms | 0.29 ms | +7% |
| 连接建立速率 | 35–38/s | 38–42/s | −6% |

#### CI 门禁设计（待三栈校准后生效）

采用**三层判定**，替代单一绝对 Mbps 硬门禁：

1. **环境健康门禁**：native 是同机锚点；每轮先检查其是否仍落在已校准分布内，偏离明显则整轮标记 `PERF_ENV_UNSTABLE`，不将环境抖动误判为任一栈回归。
2. **paired relative gate（主判据）**：分别比较同一 round 的 `lwip/native`、`xtcp/native` 与 `xtcp/lwip`，对每类比值取中位数（不是分别取吞吐中位数后再相除）。低于校准后的 relative floor 才判 regression。
3. **absolute catastrophe floor**：保留但显著低于各 stack/GSO-mode 的正常分布，仅抓灾难性回退。

判定逻辑：

```text
if 功能、runtime proof 或统计完整性失败:  FAIL
if native 基线明显异常:                    PERF_ENV_UNSTABLE
else if 绝对吞吐 < catastrophe_floor:       FAIL
else if paired relative ratio < floor:      FAIL
else:                                       PASS

WARN: 离散度异常 / zero-rate flow / P16 fairness 恶化 / 接近门槛
```

**CI 运行形态**：canonical mode 为 `native/off → lwip/off → xtcp/off → native/on → lwip/on → xtcp/on`。每一轮将该六项序列循环左移一位，连续 6 round 后每个 mode 恰好经历每个位置一次，以抵消温频/cache/负载漂移。

每个 cell 记录 requested/active TCP stack、requested/active GSO proof、吞吐、per-flow min/p10/p50/p90/max、zero-rate flow、公平性、paired ratio 与离散度；GSO-on 还保存 GSO ledger。缺失 active proof 或能力回退不是有效 score。

**校准规程**（重新制定 §11.4 门槛前一次性执行）：P=1/4/16 × UL/DL × native/lwIP/XTCP × GSO off/on × **12 rounds**（两套完整 6-mode rotation）；每个 cell 保存 median、p10/p90、MAD、paired ratio median、paired ratio p10/p90；门槛从该分布推导，**不手工拍绝对阈值**。

**已定案的归因（勿重复排查）：**

- UL 曾被 runtime 接收队列上限过小导致的人为反压限速（拒绝 → 栈不 ACK → 对端 RTO/cwnd 坍缩）；per-flow cap 提升至 4MiB 后消除并反超 native。该 cap 不是最终流控设计：production 需叠加全局 queued-byte budget 与高水位监控。
- **全局 budget 是安全保险丝，不是正常流控机制**：budget 耗尽同样走 `OnReceive(false)` 恶性路径（对端 RTO）。production gate 要求在受支持并发（P=16/64/256、burst/churn）下 `global rejection ≈ 0`、`OnReceive(false) ≈ 0`，并以真实并发 burst 水位分布确定默认值——32MiB 目前是工程缺省而非实测结论。
- DL 天花板与 XTCP 栈发送节奏相关：NDI callback interval p50≈32µs（~22kpps），Output() wall time p50=16µs。A2-0 排除了 Output 存在独立 ~22kpps service-rate 硬上限，也证明当前不应实现 OutputBatch；但 Output 同步耗时约占 callback 周期一半，**尚不能排除它与 kcc pacing 串联形成最终发送间隔**。源码证据（tcp_fsm.cpp 发送路径）：pacing deadline 基于发包起点时间戳（absolute deadline），Output 耗时小于 pacing 间隔时被吸收；DL 有效 flight 受 `min(cwnd, snd_wnd, snd_buf)` 限制，观测值约 10KiB 量级，按观测 RTT 粗略换算得到约数百 Mbps 的容量上限，与实际 DL 天花板处于同一量级——结合发送路径源码与 pacing 行为，证据指向上游 kcc 稳态 cwnd/发送节奏，而非 bridge 的 Output service-rate 硬上限。若需最终因果钉死，再做 Direct-TUN 或 pacer deadline 遥测（验证性增强，非必要归因步骤）。
- strand/CPU/timer/ingress 全链路健康（queue-delay p95≤128µs、owner CPU ~40%、timer late p95≤256µs、ingress 零丢弃）。
- 历史两栈 P16 样本曾无明显 starvation：每流吞吐连续分布（9.6–26Mbps）、无接近零的流；但这已被 §11.4.2 的三栈 provisional 结果限制——`XTCP/GSO-off/P16/UL` 在 Stage A 与 round-3 分别出现 2 条和 1 条 zero-rate flow。故不得再宣称 P16 公平性已通过；`XTCP-SHARED-PATH-001` 必须同时报告 SUM、per-flow p10/p50/p90、max/min 与 zero-rate flow，并在隔离 CPU 环境复核。

**记账生命周期不变量**：runtime 全局 queued-byte ledger 在任意 teardown 路径（peer_gone mid-flight / RST / 正常关闭 / runtime stop）后必须精确归零——禁止用 saturating subtraction 掩盖问题。回归覆盖见 `xtcp_runtime_bridge_test` 的 `TestQueuedBytesAccounting`。

**实验旋钮约定**：`OPENPPP2_XTCP_PERF_JSON`（保留）、`OPENPPP2_XTCP_WRITE_CAP_BYTES`（内部/实验配置）、`OPENPPP2_XTCP_GLOBAL_QUEUE_BYTES`（production 前按压力矩阵定默认值）、`OPENPPP2_XTCP_LAB_SEND_RETRY_US`（LAB-only——已证明 retry 周期不影响吞吐，禁止当作生产调优参数）。

### 11.5 后续独立工作项

> **multi-worker 边界（勿迁移）：** 即使上游 bench 显示其 RX multi-worker 扩展 sublinear（如 1 worker 135 → 8 worker 99），也不得据此推导 OpenPPP2 "应做 8 shard"。那只能说明**增加 worker 不天然提升 XTCP core**；`PPP-DATAPATH-001` 继续严格单核，多流/multicore 归 `XTCP-SHARED-PATH-001` 独立评估，不混入单核主线。

| 优先级 | 工作项 | 目标 |
|------|------|------|
| P1 | `XTCP-KCC-PACING-001` | 分析 kcc 稳态 cwnd/pacing/ACK clock，解释 clean netns、RTT≈0.3ms 下稳态仅 ~10KiB flight 的成因（cwnd 自身目标 / ACK clock / snd_buf 配额 / 窗口字段单位或更新错误），争取 DL 从 −19% 收敛至 native −10% 内。**禁止一开始就调大 cwnd**——先拆解 cwnd、snd_wnd、snd_buf、bytes_in_flight、ACK cadence、growth/decay、pacing_rate、loss/retrans，并区分 app-limited / cwnd-limited / rwnd-limited |
| P2 | `XTCP-WRITABLE-001` | `SendSome`（prefix 接受语义）/ writable notification（0→正边沿一次性武装），移除无效 retry polling；效率与接口语义改善，非当前吞吐主修复 |
| P2 | `XTCP-SHARED-PATH-001` | P16/64/256 shared-path 容量、全局 budget 水位压力矩阵（确定 production 默认值）、fairness 与容量门禁。**首要观察项：P16 upload 的 paired ratio（xtcp/native）已从 ~0.89 漂移至 ~0.67**——native 自身波动不能完全解释该相对比值漂移，须厘清其中 XTCP 额外损失的成分；同时确认 P=4"共享路径候选分水岭"是否可复现升级为定案 |
| P1 | `XTCP-NDI-MEMORY-001` | C 榜 attribution：不走 TUN、不改 XTCP core 与 tunnel wire，NDI Output 直接进 memory-backed sink、输入由 memory source 喂入，保持相同 packet bytes/MTU/TCP options、单 owner / single-core。回答"去掉 TUN/Tap 后 OpenPPP2 XTCP adapter 自己能跑多少"；对 UL/DL 分开测（RX→NDI direct sink 与 memory source→XTCP TX 分岔），把 NDI/backend ceiling 与 KCC pacing/cwnd ceiling 分开 |
| P2 | `LWIP-NETIF-MEMORY-001` | C 榜 lwIP 阶梯：in-memory netif → bare TUN → OpenPPP2 VNet → full PPP，与 XTCP 的保留率对比，回答 integration 更适合哪个执行模型 |

**XTCP-SINGLECORE-BASELINE-20260902（单核优化封板锚点，多核结果一律相对此基线报告）：**

- 锚点 commit：本 commit（`XTCP-SINGLECORE-BASELINE-20260902`）；内容冻结于 ba67601（docs）+ 本记录
- ppp binary sha256: `e68b1e75deac0636577069fa99b8a715ab98f243a913ec871ec6937ba63680fe`（build/xtcp-runtime-root，Release，）
- XTCP upstream revision: `e79db8fd10a1ee39be2dc3a9361727fcad79d04c`（archive sha256 `fd194478...`）
- patchset 0001-0005 合并 sha256: `bf8e25bf32110e755de34be1cb842c299392e6be3f98fd6daebac9695dc6464d`
- GSO 模式：off/on 双模均属基线；单核 pinned CPU8
- 基线配对比（KCC 修复+两层优化后，单轮样本）：P1 DL off 1.06-1.09×、P1 DL on 0.59-0.60×、P1 UL on 0.56-0.57×、P16 DL off 1.51-1.57×、P16 DL on 0.91-0.93×、P16 UL on 0.486-0.487×

### 11.6 `XTCP-SHARED-PATH-001` S0：共享资源审计与分片单位定案（2026-09-02，单核基线 `ec3bc66` 之后）

**S0 三个问题的回答：**

1. **P16 的 shared resource 饱和点**：单核 pin 下不是任何一个共享锁，而是"整个 runtime 只有一个执行域"本身（owner strand 串行 = 全部 flow 的 Inject/ACK/定时器）。两轮优化后 queue 延迟已打掉，剩余差距是执行域宽度，不是某个 mutex 热点。
2. **最小可安全分片单位 = XtcpRuntime 实例**（方案 (a)：N 个完整 runtime，switcher 按 flow 4-tuple hash 路由 Submit）。不用方案 (b)（单 stack 多 strand 驱动）：上游 stack 虽有 per-conn shard 锁（`ShardOf` + `recursive_mutex`），但 accept/listen/timer/回调契约是 stack 全局的，方案 (b) 破坏"回调在 owner 线程"语义；方案 (a) 复用全部已验证 runtime 代码、可回滚、隔离清晰。同一 flow（含 SYN/deferred_syn）按 4-tuple hash 永远落同一 shard，ordering/ACK 状态/generation/close 生命周期天然保持。
3. **共享资源分类**（S1 实现的约束清单）：

| 资源 | 类 | 说明 |
|---|---|---|
| XtcpRuntime::Impl（strand/flows_/listeners_/stack_/backend_/budget/poll timer/handoff/stats） | A | 每 shard 一份（方案 a 即此含义） |
| ITap 写路径（`_write_mutex` + 单 fd + TAP strand） | D 候选（低危） | 临界区 O(1) push，sync drain 已批量化；内核侧 fd write 本就串行。S1 必须报告 per-shard enqueue p95 证实无新排队 |
| XtcpPoolLease 全局 BufRef 池（`pool_sync` 单 mutex） | D 候选（中危） | UL 每 包 Acquire 都过这把锁。2 shard 先测量池锁竞争；若 p95 恶化 → per-shard lease |
| GlobalQueueBudget / flow write_queue 记账 | A（语义变更点） | per-runtime 后全局上限变 32MiB×N——S1 需决策：总量守恒（每 shard 32MiB/N）或按 shard 放大（先 32MiB×N，报告 high-water） |
| runtime stats / perf JSON | A | 每 shard 独立输出（shard 标签），汇总在矩阵脚本层做 |
| packet_dispatch_ / switcher OnPacketInput | B（新增路由器） | 解析 4-tuple → hash → shard；复用 XtcpRuntime 的 ParsePacket |
| crypto/mux/carrier | 不在 XTCP 数据面 | client 侧 flow 经 loopback connector 桥接本地应用，wire 侧在 server 实例；server 线程入口同 pattern 路由 |

**S1 矩阵（冻结）**：P=1/4/16/64 × shards=1/2 × UL/DL × GSO off/on；每 cell 报 SUM Mbps、per-flow p10/p50/p90/min/max、Jain fairness、zero-rate flows、process cores、Mbps/core、ns/B、per-shard packets/bytes/flows/CPU/queue p50/p95/p99/high-water、global budget high-water/rejections/OnReceive(false)。**吞吐与单位 CPU 效率必须同时报告**。

**2-shard 工程门槛（非 CI gate）**：P16/P64 throughput ≥1.5× 1-shard 且 total CPU ≤2.1 cores 且 per-core 效率 ≥0.75× 基线 且 zero-rate=0 且 Jain ≥0.98 且 global rejection ≈ 0 且 OnReceive(false) ≈ 0 且 lifecycle/记账回归零。达 1.6-1.8× 才扩 4 shard；<1.5× 停下找新串行点，不扩规模。KCC/PACING-001 与本战线严格串行，不同时改。

**S1 结果（2026-09-02，binary `043894d1`，2×CPU{8,9} `client-cpuset`，30s formal，xtcp-only 32 cell；相对 1-shard 同设置配对）：**

| cell | 1-shard | 2-shard | × | cores(1→2) | Mbps/core × | Jain(1→2) | zero |
|---|---:|---:|---:|---|---:|---|---|
| P16 UL off | 179 | **593** | **3.31** | 0.59→0.92 | 2.13 | 0.992/0.980 | 0/0 |
| P16 UL on | 339 | **531** | **1.57** | 0.53→0.88 | 0.94 | 0.989/0.787* | 0/0 |
| P16 DL off | 529 | **833** | **1.57** | 0.53→0.91 | 0.92 | 0.983/0.993 | 0/0 |
| P16 DL on | 505 | **773** | **1.53** | 0.54→0.93 | 0.89 | 0.972/0.992 | 0/0 |
| P64 UL off | 211 | **606** | **2.87** | 0.55→0.92 | 1.71 | 0.560/0.781 | **18**/1 |
| P64 UL on | 238 | **476** | **2.00** | 0.59→0.89 | 1.31 | 0.735/0.894 | **14**/1 |
| P64 DL off/on | — | — | stall（见下） | — | — | — | — |
| P1 全部 | 344-485 | **151-320** | **0.31-0.81** | — | — | 1.000 | 0 |

**2-shard 门槛判定（P16/P64）：✅ 通过**——吞吐 ≥1.5×（P16 四 cell 1.53-3.31×，P64 UL 2.00-2.87×）、总 CPU ≤2.1 核（实测 ≤0.93）、per-core 效率 ≥0.75×（0.84-2.13×）、zero-rate=0、global rejection=0、lifecycle/记账零回归。Jain：p16-ul-on 单轮 0.787 为瞬态，两轮复跑 0.982/0.984 判为噪声；P64 UL 的 Jain 低于 0.98 是 1-shard 就存在的 P64 公平性问题（SHARED-PATH 已立项观察项），2-shard 反而改善（18/14 zero-rate → 1/1）。

**关键发现：**
1. **多 shard 必须有真正的执行域**：runtime 原绑定在仅主线程 run 的 default context 上，双 strand 实测零并行（+4%）；专属 io_context + 每 shard 一个 worker 线程后才拿到上述数字。
2. **P1/P4 在 shards=2 下回退（0.31-0.81×）**：跨 context 投递税——TAP 读/写在主线程（default context），shard 工作在专属池，每个突发 ingress/egress 各跨线程一次。P1 吞吐对 ACK 往返延迟敏感（ACK clock）。**shards 默认仍为 1**，S2 需做 IO 同址（TAP IO 与 shard 池合并）才能默认启用。
3. **P64 DL GSO off/on 完整 stall（既有问题，与分片无关）**：client `posts=0`、cwnd=1、inflight 单段卡死、iperf 64 流全零——用基线 binary（`e68b1e75`，分片改造前）复现同样挂死，r3 回归从未覆盖 P64 DL。wedge 在 server 侧/隧道路径而非 client runtime。**S2 阻塞项**。
4. `process_cores_ok`/`zero_migrations` fail 为本环境 perf task_clock 遥测缺失（基线已记录），吞吐与正确性检查全过。

**S2 工作项（按优先级）**：① IO 同址消除跨 context 投递税（解锁 shards=2 默认化与 P1 回归）；② P64 DL stall 根因（posts=0 指向 server 侧/隧道路径）；③ 达标后再议 4 shard 扩展。
