# OpenPPP2 吞吐优化计划

> Status: Draft / plan
> Type: Plan
> Last verified: 2026-10-17（证据来自 88e0a77b 与 PR #90 分支）
> Tracking: Miaocchi/openppp2#91

本文把已有的性能证据整理成一份可执行的优化计划。度量口径沿用
[XTCP 集成说明](XTCP_INTEGRATION_CN.md) §11.4 的三榜框架；Linux TUN 边缘
合并沿用 [GSO 合并设计](PPP-DATAPATH-GSO-CONTRACT_CN.md)；损伤验收沿用
[NetEm 验收设计](PPP-DATAPATH-NETEM-ACCEPTANCE_CN.md)。所有性能结论必须标明属于
哪一榜（A 严格单核产品 / B 实际产品 / C 逐层归因）以及哪个冻结 revision。

## 1. 目标

| 编号 | 目标 | 榜 | 现状 |
| --- | --- | --- | --- |
| G1 | Linux native D0 P1 单核吞吐翻倍以上 | A | 基线约 0.3 Gbps；GSO-on UL 实测 822 Mbps（default-off） |
| G2 | XTCP 在 A 榜全矩阵（P1/P4/P16 × UL/DL）稳定达到 native 的 1.2 倍 | A | P1/UL、P4/DL 已达 1.2×；P16/DL 单核约 1.05× |
| G3 | 多核场景按核数扩展，单连接不因跨线程唤醒退化 | B | shard 默认 1；跨线程逐段唤醒 |
| G4 | Windows、Android 有可复现的真机基线，并找到各自的主要瓶颈 | A/B | 只有回环组件基准；没有真机矩阵 |
| G5 | 每项优化都不降低正确性：xtcp 上游测试全过，Route3 qualification 全过 | — | 本地补丁栈让 13–15 个 xtcp 测试失败 |

非目标：本计划不改动加密协议、不改 carrier 线格式、不追求和内核回环比绝对数值
（见 §2.3）。

## 2. 现状与证据

### 2.1 产品数据面（Linux，A 榜）

- 单核约 0.3 Gbps 时的成本归因：TUN 27%、carrier 23.5%、加解密 13.4%，其余是
  系统调用、libc 等（GSO 合并设计 §0）。没有哪一域超过三成，单点优化收益有限。
- TUN 边缘 GSO 合并（default-off）：P1 UL 298.7 → 822.4 Mbps（2.75×），重传为 0。
- XTCP：P1 direct GRO 12KiB 约 975–1039 Mbps；13–15KiB 严重退化；
  DL 16KiB chunk 约 596 Mbps；NDI TSO 中位 527 Mbps，负收益，保持 default-off。
- 已知问题：`XTCP-GSO-STALL-001`（P16/DL GSO-on 全流归零）、P64 DL server wedge、
  direct 路径间歇退化（同配置三轮出现 30 Mbps 低值）。

### 2.2 组件基准（`xtcp_runtime_bridge_test --throughput`，C 榜补充）

单连接、32 KiB 窗口、合成对端：

| 平台 | 路径 | Mbps |
| --- | --- | --- |
| Windows | Winsock 回环参考 | 约 1000 |
| Windows | XTCP TCP 桥 / 直连桥 | 约 1130 / 约 1245 |
| Linux (WSL2) | 内核回环参考 | 7186–9561 |
| Linux (WSL2) | XTCP TCP 桥 / 直连桥（批量唤醒） | 1305–1389 / 1878–1987 |

剖析结论：runtime I/O 线程跑满，剩余成本是逐段（1360 B）的固定开销和每批次的跨线程
唤醒；`getenv` 热路径（占 I/O 线程约 36%）已修复。

### 2.3 口径说明

内核回环在组件基准里约 9 Gbps，是内核内存拷贝的速度，不是 OpenPPP2 能达到的上限，
不作为目标值。产品目标以 A/B 榜的 native 同机配对为准。

### 2.4 文档与代码不一致

XTCP 集成说明 §0 写"直连桥未设置 `OPENPPP2_XTCP_MEMORY_BRIDGE` 时默认尝试"，但
`VEthernetNetworkTcpipConnection.cpp` 的 `XtcpMemoryBridgeEnabled()` 只在值为 `1`
时开启。WS0 需要先确认哪一个是预期行为，再修正另一方。

## 3. 工作流

工作量标记：S（数天）、M（1–2 周）、L（数周以上）。

### WS0 度量与门禁（先做，其他工作流依赖它）

| 项 | 内容 | 量 |
| --- | --- | --- |
| 0.1 | 冻结 revision 指纹流程落地（`ppp_sha256 / git_head / dirty` 记录），所有 A/B 榜结果绑定指纹 | S |
| 0.2 | C 榜阶梯 C0–C7 从设计变成可运行脚本，先做 C0/C1/C2/C7 四档，回答"损失在哪一层" | M |
| 0.3 | 组件基准增强：合成对端协商窗口缩放；新增超大段模式（16–64 KiB GSO 语义段）；分别报告每段固定成本和每字节成本 | M |
| 0.4 | Linux CI 冒烟：组件基准 5 秒短跑，相对基线下降超过 10% 判失败；同时在 CI 跑 xtcp 上游测试 | S |
| 0.5 | 核实 §2.4 的直连桥默认值 | S |

### WS1 正确性前置（阻塞 WS2）

| 项 | 内容 | 量 |
| --- | --- | --- |
| 1.1 | 修本地补丁栈 FIN 顺序 bug：0005+0013 后一个带 FIN 的段若同时清理了过期乱序节点，会跳过 CLOSE-WAIT。改为先处理 FIN（上游 PR 版本已按此顺序） | S |
| 1.2 | 本地补丁栈让 xtcp 上游测试 13–15 个失败：0004 按 Linux 语义把带时间戳的载荷改成 1448 B，需要逐个核实并更新测试预期；0005 的立即 ACK 带 TSecr=0，需修正回显 | M |
| 1.3 | `XTCP-GSO-STALL-001` 根因与修复 | M |
| 1.4 | P64 DL server wedge；direct 路径间歇低值（resume_effective=0 的那一轮） | M |
| 1.5 | 上游合并进展：liulilittle/xtcp#1 合入后，把补丁栈 rebase 到上游新版本，删除已合入的补丁 | S |

### WS2 把处理单元做大（主要收益来源）

| 项 | 内容 | 依赖 | 量 |
| --- | --- | --- | --- |
| 2.1 | Linux TUN 边缘 GSO 合并晋升默认开启：门槛是 1.3 修复、full matrix 通过、NetEm campaign 通过 | 1.3, 0.1 | M |
| 2.2 | 查清 direct GRO 13–15 KiB 退化原因（猜测与 32 KiB 池块、ACK 节奏或 window 更新有关），再决定能否突破 12 KiB | 0.3 | M |
| 2.3 | xtcp 缓冲池增加 64 KiB 大块层（有上限，与 upload budget 联动），超大帧整段进协议栈，不再切分 | 2.2 | L |
| 2.4 | NDI TSO 负收益根因：single-flight recovery gate 是否让 TSO 段频繁退化成 MSS；修好后再评估默认开启 | 2.3 | M |
| 2.5 | 直连桥下行：16 KiB、单在途改为更大块、多在途。之前 four-credit 实验因 receiver window 互锁被否决，需要先让 credit 释放和 rwnd 解耦 | 1.4 | M |
| 2.6 | Windows/Android 在 TAP 层做用户态 GRO/GSO：Wintun 一次唤醒读一批包，按同流、连续、仅 ACK/PSH 的规则合并；发送端软件分段后批量写入 | WS5/WS6 基线 | L |

### WS3 线程与唤醒

| 项 | 内容 | 量 |
| --- | --- | --- |
| 3.1 | 生产 second leg（`VirtualEthernetTcpipConnection`）实现 `BatchesPeerSend()/FlushPeerSend()`：一个入站批次只唤醒一次 writer。组件基准上该改动带来约 30% | S |
| 3.2 | 轮询定时器：只在新 deadline 早于已设 deadline 时才重设，减少 `timerfd_settime`（组件剖析中占 6–9%） | S |
| 3.3 | shard 默认数量按核数设置，并验证 B 榜多流扩展性和公平性 | M |
| 3.4 | 抽出通用的"SPSC 环形队列 + 仅在消费方等待时唤醒"组件，替换 TAP→栈、栈→second leg 两处交接 | M |
| 3.5 | Linux 多队列 TUN 的 per-queue affinity（按 MULTIQUEUE_TUN_MODEL 建议方向） | M |
| 3.6 | 单连接流水线：TAP I/O、TCP 状态机、carrier 加密分属三个线程。仅在 WS2 之后、有 C 榜数据证明单线程到顶时再做 | L |

### WS4 carrier 与加解密（合计约 37%）

| 项 | 内容 | 量 |
| --- | --- | --- |
| 4.1 | 测量 C4/C5/C6 三档，量化 exchanger/carrier、mux、加密各自的保留率 | S |
| 4.2 | 上行 gather 晋升：60 KiB gather 在单轮筛选中 P1/P4/P16 达到 1.21–1.26×，但三轮复测 P1 不稳；查清不稳原因 | M |
| 4.3 | 批量加密：一次处理多个帧，复用已做的 AES-NI 4 路交织 | M |
| 4.4 | 发送零拷贝：帧头和载荷用 writev 拼接，去掉一次复制 | M |

### WS5 Windows

| 项 | 内容 | 量 |
| --- | --- | --- |
| 5.1 | 建立隔离的 Windows 真机或 VM 测试环境（需授权，不能影响现有连接），跑 native 与 XTCP 配对 | M |
| 5.2 | Wintun 批量收发，减少每包一次的环形缓冲操作 | M |
| 5.3 | KCC 在 Windows 的定时器问题：评估高精度可等待定时器；在此之前保持默认 cubic | S |
| 5.4 | 评估 Windows AF_UNIX 桥（只评估，结论已记录在 XTCP 集成说明） | S |

### WS6 Android

| 项 | 内容 | 量 |
| --- | --- | --- |
| 6.1 | 补齐 Android Boost/OpenSSL 依赖，打开 XTCP 构建 | S |
| 6.2 | 真机基线：吞吐、CPU、耗电，至少两款 SoC | M |
| 6.3 | VpnService fd 批量读与用户态 GRO（同 2.6） | L |

## 4. 里程碑

| 里程碑 | 包含 | 验收 |
| --- | --- | --- |
| M0 度量就绪 | 0.1–0.5、1.1、3.1、3.2 | C0/C1/C2/C7 可跑；CI 冒烟上线；xtcp 上游测试在 CI 里跑；FIN bug 修复有测试 |
| M1 正确性清零 | 1.2–1.5 | 本地补丁栈下 xtcp 上游测试全过；GSO-on full matrix 无 stall、无 wedge |
| M2 Linux 大单元 | 2.1、2.2、4.1、4.2 | A 榜 native D0 P1 ≥ 2×；GSO 合并默认开启；XTCP 全矩阵 ≥ 1.2× native |
| M3 栈内大段与多核 | 2.3–2.5、3.3–3.5 | B 榜随核数扩展；单连接 DL 吞吐提升可复现 |
| M4 Windows/Android | WS5、WS6、2.6 | 两个平台都有真机配对基线，主要瓶颈有剖析数据支撑 |
| M5 流水线（视数据决定） | 3.6、4.3、4.4 | 只在 C 榜显示单线程到顶时启动 |

M0 和 M1 可以并行；M2 依赖 M0 的度量和 M1 的 stall 修复；M4 中的测试环境搭建可以与
M2 并行进行。

## 5. 风险

- **补丁栈膨胀**：2.3、2.4 会继续修改 xtcp。每项通用修复都同时向上游提交，降低升级成本。
- **内存**：64 KiB 块乘以连接数会放大内存，大块层必须有上限并接入 upload budget。
- **延迟与公平性**：大段带来突发，需要和 pacing quantum 配合；交互型小包延迟单独测。
- **测量噪声**：共享宿主上的结果不能当严格结论，A 榜必须在隔离环境、同机配对、多轮交错下测。
- **真机授权**：任何启动 PPP、改路由/DNS/网卡的验证都需要事先授权，并在隔离环境进行。

## 6. 待决策

1. §2.4 直连桥的预期默认值。
2. M2 的目标数值是否采用 §1 的 G1/G2。
3. Windows/Android 真机测试环境由谁提供、何时可用。
4. 3.6 单连接流水线是否列入本季度范围，还是等 M2 数据后再定。
