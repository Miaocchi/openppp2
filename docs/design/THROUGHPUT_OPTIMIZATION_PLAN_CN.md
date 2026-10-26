# OpenPPP2 吞吐优化计划

> Status: Draft / plan
> Type: Plan
> Last verified: 2026-10-26（§2.4 历史矩阵 d4fcdb6f；新基线与诊断 add77894）
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
| G2 | XTCP 在 A 榜全矩阵（P1/P4/P16 × UL/DL）稳定达到 native 的 1.2 倍 | A | 2026-10-26 当前 main 三轮复测未达成：六组均未每轮达到 1.2×；UL P4/P16 为 0.945–0.991× / 0.913–0.923×（§2.4.2） |
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

### 2.4 M2 矩阵与瓶颈（A 榜定向）

以下先保留 2026-10-25 的历史矩阵；当前 main 的复测与上行诊断见 §2.4.2。

冻结二进制：`d4fcdb6f`（clean，SHA-256 `7767443b…`），远端共享 VM（Xeon E5-2680 v4），
隔离 netns，客户端单核绑定。参数：gather 65536 / wait 1 ms、memory bridge on、
`--tap-gso-segments 32`、kcc、aes-256-cfb、shards 1。3 轮交错（GSO on/off 顺序轮换），
每 cell 45 s；72 cell 全部 pass，无零速流，netns 清理与二进制冻结校验均通过。测量在
同机 M3 编译结束、负载降至 4 以下后才开始。

| GSO | 方向 | P | native Mbps | XTCP Mbps | XTCP/native（3 轮） |
| --- | --- | --- | --- | --- | --- |
| on | UL | 1 | 1119 | 1301 | 1.17 / 1.17 / 1.12 |
| on | UL | 4 | 1123 | 1177 | 1.04 / 1.05 / 1.05 |
| on | UL | 16 | 1107 | 1070 | 0.94 / 0.96 / 0.97 |
| on | DL | 1 | 1842 | **414** | **0.23 / 0.22 / 0.22** |
| on | DL | 4 | 1745 | 1269 | 0.72 / 0.72 / 0.73 |
| on | DL | 16 | 1735 | 1528 | 0.92 / 0.88 / 0.87 |
| off | UL | 1 | 244 | 547 | 2.25 / 2.23 / 2.18 |
| off | UL | 4 | 240 | 514 | 2.14 / 2.21 / 2.09 |
| off | UL | 16 | 222 | 485 | 2.18 / 2.18 / 2.13 |
| off | DL | 1 | 623 | 364 | 0.57 / 0.58 / 0.59 |
| off | DL | 4 | 618 | 771 | 1.26 / 1.23 / 1.24 |
| off | DL | 16 | 608 | 813 | 1.36 / 1.34 / 1.33 |

（native/XTCP 列为 3 轮中位数。）

结论：

- G2（全矩阵每轮 ≥ 1.2×）**未达成**。GSO-on 下 12 个 cell 中没有一个满足；GSO-off 下
  只有 UL 全部与 DL P4/P16 满足，但这是因为 native 在 GSO-off 时本身很低。
- 最大缺口是**单连接下行**：GSO-on 时 XTCP 固定在约 414 Mbps（native 1842），三轮
  几乎无方差，说明是结构性上限而不是间歇抖动；随并发数升高差距缩小（P4 0.72×、
  P16 0.9×），符合"每连接存在固定上限"的特征，与 2.5（直连桥下行 16 KiB、单在途）
  的描述一致。列为新工作项 1.6。
- 此前只测 UL 的定向结果（P1 UL 1.12–1.20×）在本矩阵中得到复现。
- 这仍是共享 VM 上的定向结论；严格 A 榜验收需隔离单核环境。

#### 2.4.1 单连接下行上限的根因：XTCP snd_buf（2026-10-25/26）

- 诊断（GSO-on DL P1，perf + 计数器）：客户端进程只用 0.33 核，不是 CPU 瓶颈；
  直连桥下行块 16→32 KiB 只从 413 提到 424 Mbps，也不是块大小。生产默认不设
  `OPENPPP2_XTCP_SNDBUF_BYTES`，xtcp 每连接 snd_buf 取上游默认 64 KiB，在途数据被
  限制，约 410 Mbps 正好对应 64 KiB / ~1.2 ms。
- snd_buf A/B（旧 xtcp 77d45585，3 轮，XTCP/native）：

| snd_buf | DL P1 | DL P4 | DL P16 | DL P64 | UL P1 / P4 / P16 |
| --- | --- | --- | --- | --- | --- |
| 64 KiB | 0.22 | 0.72 | 0.9 | — | 1.12–1.17 / 1.04 / 0.95 |
| 256 KiB | 0.56–0.65 | 1.09–1.23 | 1.08–1.20 | 1.36–1.46 | 1.14–1.22 / 1.05–1.15 / 0.95–1.03 |
| 1 MiB | 0.92–0.99 | 1.15–1.18 | 1.06–1.15 | 3 轮中 1 轮卡死 | 1.00–1.15 / 0.96–1.05 / 0.96–0.99 |

- xtcp 更新到上游 f71548f（#102）后，KCC v2 的 cwnd 目标缺少 Linux `bbr_set_cwnd`
  的 ack-aggregation 与 quantization 余量，大 snd_buf 下多流 DL 减半；#106（补丁 0038）
  已修复，P4/P16 DL 回到旧版水平。
- 大 snd_buf × 多流的卡死根因：xtcp 全局固定缓冲池（约 36 MiB，修复前无堆兜底）被发送缓冲
  占满后，入向 ACK 因申请不到缓冲被丢弃，snd_buf 无法释放，形成死锁。默认 64 KiB 在
  数百条满载流时同样可能触发。#108 的 0039/0040 已用 TX 余量与有界 heap overflow
  修复（集成说明 §11.188），#109 已把 Linux 默认 snd_buf 调为 1 MiB。
- 结论：调大 snd_buf 默认值是 G1/G2 下行缺口的主要手段，1.7 的前置修复已经合入。上行
  P4/P16 在各档 snd_buf 下都只有约 1.0×，不受 snd_buf 影响，需要单独工作项。

#### 2.4.2 当前 main 复测与上行多流归因（2026-10-26）

冻结产品源码 `add778946e268ad765a284b682dda02a5cba1b85`；新构建二进制 SHA-256
为 `6c6b7e327b7d64a06ab7e5edd0159cc6a72d52f824645fdb10f916bbf1965363`。
使用固定上游 f71548f、0038/0039/0040、Linux snd_buf 1 MiB；M3 开关全部保持关闭，
shards=1、客户端与 iperf 同绑 CPU 49、GSO on、cap32、KCC、AES-256-CFB、
direct memory bridge、gather=65536、wait=1 ms。构建与测量均持共享锁；测量经过
空闲检查，外层独立 network/mount namespace，内层三端 netns。

远端本次证据根目录记为 `W=/home/ppp-m2/m2b-20261026`。`m2b-source.tgz`、
`binary.sha256`、`frozen-source.json` 与每组的 `provenance.json` 保存冻结证据。
产品源码未改；测量工具先修复同文件系统 bind mount 被 `os.path.ismount()` 误拒绝的问题，
改读 `/proc/self/mountinfo`，继续核对 backing 目录身份。初次启动前拒绝记录保留在
`W/runs/`、`W/runs-isolated/`；实际测量位于 `W/runs-v3/`。
包装器在 Popen 时调整方向和 snd_buf，`provenance.args` 保留 measure 的原始模板；
实际参数以 `matrix/ordered-runs.json` 和 `matrix-metadata.txt` 为准。
早期诊断沿用已有 `ordered-runner.py` 的公共 perf 缓存配置
`/home/ppp-m2/source/build/private/m2/perf-cache`，存在新目录以外的缓存写入可能，
这是本次执行偏差。AES-128 对照及后续报告改用本次专用 `W/ordered-runner.py` 与
`W/perf.config`，公共缓存没有清理。

**新基线：30 秒正式区间、omit=2 秒、三轮交错。** 每轮依次测 UL P1/P4/P16、
DL P1/P4/P16、DL P64；第一、三轮 native 在前，第二轮 XTCP 在前。结果根目录为
`W/runs-v3/baseline/pass-{1,2,3}/`，分组名为 `ul-p1-4-16`、`dl-p1-4-16`、
`dl-p64`，每格保存 `matrix/round-1/{stack}-gso-on-p{P}-{direction}/result.json`。
吞吐列是各栈三轮的中位数（Mbps），比值列是当轮 XTCP/native，不是两个中位数之比。

| 方向 / P | native 中位数 | XTCP 中位数 | 第 1 轮比值 | 第 2 轮比值 | 第 3 轮比值 | 每轮 ≥1.2× |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| UL / 1 | 1124.60 | 1230.94 | 1.095 | 1.072 | 1.090 | 否 |
| UL / 4 | 1083.59 | 1070.41 | 0.991 | 0.991 | 0.945 | 否 |
| UL / 16 | 1099.58 | 1012.98 | 0.921 | 0.923 | 0.913 | 否 |
| DL / 1 | 1811.82 | 1715.20 | 0.954 | 0.953 | 0.917 | 否 |
| DL / 4 | 1744.60 | 2138.17 | 1.134 | 1.226 | 1.246 | 否 |
| DL / 16 | 1739.22 | 1837.05 | 1.067 | 1.050 | 1.051 | 否 |
| DL / 64 | 944.60 | 1249.25 | 1.019 | 1.323 | 1.384 | 否 |

`W/summary.json` 保留逐轮原值，`W/audit.json` 核验 42 格 result/qualification 全部
pass、零速流为 0、9 组冻结/监测/清理通过，并核对实际 argv 与 M3 关闭状态。
另有 20 格定向诊断通过，放宽 watchdog 仍失败的 `p64-long` 现场单独保留。
G2 **未达成**：必选六组均未每轮达到 1.2×；DL P4 仅第二、三轮达标，补充项 DL P64
也仅第二、三轮达标。DL P1 已不再受历史 64 KiB snd_buf 的约 350 Mbps 上限限制，
但比值仍低于 1。DL P64 的 native 三轮为 1522.65 / 944.60 / 891.68 Mbps，存在明显
波动；三轮未重现 pool 耗尽或零速流，不代表长期稳定性验收。共享 VM 即使使用测量锁
和空闲检查也不能完全消除宿主调度影响；本轮提供方向性证据，不替代独占机器的 A 榜验收。

**20 秒定向诊断。** `W/runs-v3/diagnostic/base/matrix/round-1/` 中 native/XTCP
P4/P16 四格均 pass、零速流为 0：

| P | native Mbps | XTCP Mbps | XTCP/native | XTCP PPP 核数 | 同核 iperf 核数 | 所选核心忙碌率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 1090.40 | 1027.19 | 0.942 | 0.848 | 0.108 | 97.9% |
| 16 | 1078.26 | 988.59 | 0.917 | 0.866 | 0.107 | 98.5% |

- 核心容量基本用尽，不能把 PPP 未独占 1.0 核误判成窗口瓶颈：iperf 与 PPP 本来就共用
  一核。`cpu-process-perf.csv`、`cpu-iperf-perf.csv`、`cpu-*-proc-stat.txt`、
  `result.json` 给出上述数值。`client-ss-tin-samples.txt` 保存 TCP_INFO 独立采样。
- `stats.ndjson` 中两格 upload budget reject、direct upload backpressure、ingress drop
  均为 0，预算上限仍为 32 MiB；这组没有触发 #100 的预算限制。direct accepted
  bytes/chunks 的全程平均块长约 9 KiB，逐块分配、校验和与 ACK 等固定成本仍存在。
- `cpu-process-perf.data` 的 49 Hz CPU 样本中，AES-256-CFB 加密占 XTCP P4/P16
  的 33.80%/33.66%，native 为 33.90%/34.20%；它是共同的大头，不能单独解释
  XTCP/native 的缺口。XTCP 的 checksum 类符号约 4%，另有协议栈、分配、引用计数与
  mutex 成本。原始报告为 `W/perf-{native,xtcp}-gso-on-p{4,16}-ul.txt`；样本量仅
  约 800/格，百分比是定向证据，不能据此宣称精确的 C4/C5/C6 保留率。

**单变量 A/B（每方案 20 秒、单轮，单位 Mbps）。** 路径为
`W/runs-v3/diagnostic/{base,gather-off,wait-off,aes128}/matrix/round-1/`；16 格均
result/qualification pass、零速流为 0，冻结与清理通过。

| 方案 | native P4 | XTCP P4 | 比值 | native P16 | XTCP P16 | 比值 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 默认 gather / wait / AES-256 | 1090.40 | 1027.19 | 0.942 | 1078.26 | 988.59 | 0.917 |
| gather=1（不聚合） | 1077.99 | 762.53 | 0.707 | 1066.47 | 791.50 | 0.742 |
| gather=65536，wait=0 | 1097.18 | 927.46 | 0.845 | 1075.46 | 956.32 | 0.889 |
| 仅改为 AES-128-CFB | 1218.73 | 1155.53 | 0.948 | 1178.19 | 1051.76 | 0.893 |

- 关闭 gather 后 XTCP P4/P16 吞吐下降 25.8%/19.9%，关闭等待下降 9.7%/3.3%。
  `datapath-client.jsonl` 中全程 carrier bytes/send_calls 显示，默认平均写入约
  42,969/33,072 B，wait=0 降到 23,818/22,927 B，不聚合降到约 9 KiB。
  对照 native 默认约 43,658/43,580 B。多流时逐流聚合变小，放大 carrier、分配与唤醒
  的固定成本；目前 gather 和 1 ms 等待确实有收益，不应关闭。
- AES-128 使 XTCP P4/P16 增加 12.5%/6.4%，native 同时增加 11.8%/9.3%；比值没有
  接近 1.2。加密是共同的 CPU 大头，更换密码套件不是 G2 修复。
- 默认诊断的 PPP CPU 成本：native P4/P16 为 6.50/6.83 ns/B，XTCP 为 6.66/7.19
  ns/B；同核 iperf 占用也更高。P4 的聚合大小已接近 native，仍有差距，不能把全部
  差距归因于 gather。已确定上限在单核 CPU 容量，尚未把剩余差距收敛为一个可修复的
  函数。下一步是按每字节成本拆分协议栈/校验和/分配和 carrier，加密考虑跨流批量；
  C4/C5/C6 完整分层与长期/损伤验收仍待做。

以上为单轮筛选，不是新默认值的稳定性验收；本次不修改产品 C++、密码套件或 M3 默认值。

#### 2.4.3 P64 UL watchdog：iperf 周期统计阻塞结束计时（2026-10-26）

旧现场根目录 `/home/ppp-m2/runs/poolfix/new-p64-64k-ul-a/` 的
`matrix/round-1/xtcp-gso-on-p64-ul/timeout-diagnostics/` 中 iperf 主线程为 D，
连接仍为 ESTAB，接收端超时前仍有流量；19 个正式区间跨约 48.66 秒，不能因为清理后
`flows_closed=65` 就归因为 FIN 收尾。

新 native 对照 `W/runs-v3/diagnostic/p64-long/` 把 watchdog 从 52 秒放宽到 150 秒，
仍在约 59 秒后以控制消息 Broken pipe 失败。独立采样的
`iperf-kernel-stacks.ndjson` 多次捕获主线程
`getsockopt(TCP_INFO) → tcp_get_info → __lock_sock`。iperf 3.18 的
`iperf_stats_callback` 遍历所有流同步查询 TCP_INFO；client 主循环先运行这些 timer
callback 再检查结束条件。同核 64 个发送线程下，统计的 socket 锁等待拖延结束计时。
源码证据为 ESnet iperf 3.18 的
[iperf_api.c](https://github.com/esnet/iperf/blob/3.18/src/iperf_api.c)、
[iperf_client_api.c](https://github.com/esnet/iperf/blob/3.18/src/iperf_client_api.c)，以及 Linux
v6.12 [tcp.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp.c)，本次下载副本保存在 W 中。

单变量对照 `W/runs-v3/diagnostic/p64-i0/` 仅给 client 加 `--interval 0`：20 秒正式
区间、原 52 秒 watchdog，native 970.24 Mbps、XTCP 878.07 Mbps，两格均 pass、
64 条流全部非零，清理与冻结校验通过。该失败的直接机制是 harness 使用 iperf 周期
统计的方式；没有证据把这一次超时归为产品 FIN/close 缺陷，也不能据此消除所有历史
P64 产品问题。

runner 因而只对 `client-single-core × UL × P>=64` 使用 `--interval 0`，在 dry-run、
metadata 与 result JSON 记录 `iperf_report_interval`。其他格继续每秒统计，最终逐流
统计仍保留；该组合不再有 iperf 每秒区间，诊断时沿用 stats/ss 独立采样。

正式修改复测位于 `W/runs-v3/diagnostic/p64-fixed/`：同一产品二进制，20 秒、52 秒
watchdog，native 982.57 Mbps、XTCP 889.89 Mbps，两格 result/qualification 均 pass，
64 条流全部非零，实际正式区间 20.10/19.85 秒，metadata/result 的 interval 均为 0；
冻结与 netns 清理通过。工具源码及指纹为 `W/src-fixed/`、`W/frozen-source-fixed.json`、
`W/harness-fixed.diff`。本地 planner/CPU 合同、冻结清单测试、bind mount 回归通过，
远端 bind mount 隔离冒烟与拒绝未隔离运行的回归通过；没有修改产品 C++ 默认行为。

### 2.5 文档与代码不一致

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
| 0.5 | 核实 §2.5 的直连桥默认值 | S |

### WS1 正确性前置（阻塞 WS2）

| 项 | 内容 | 量 |
| --- | --- | --- |
| 1.1 | 修本地补丁栈 FIN 顺序 bug：0005+0013 后一个带 FIN 的段若同时清理了过期乱序节点，会跳过 CLOSE-WAIT。改为先处理 FIN（上游 PR 版本已按此顺序） | S |
| 1.2 | 本地补丁栈让 xtcp 上游测试 13–15 个失败：0004 按 Linux 语义把带时间戳的载荷改成 1448 B，需要逐个核实并更新测试预期；0005 的立即 ACK 带 TSecr=0，需修正回显 | M |
| 1.3 | `XTCP-GSO-STALL-001` 根因与修复 | M |
| 1.4 | P64 DL server wedge；direct 路径间歇低值（resume_effective=0 的那一轮） | M |
| 1.5 | 上游合并进展：liulilittle/xtcp#1 合入后，把补丁栈 rebase 到上游新版本，删除已合入的补丁 | S |
| 1.6 | 单连接下行固定上限已定位为 snd_buf 64 KiB；#109 已把 Linux 默认值调为 1 MiB，配套 1.7 已合入；当前矩阵见 §2.4.2 | S |
| 1.7 | 缓冲池耗尽死锁已由 #108 的 0039/0040 修复（TX 余量 + 有界 heap overflow，集成说明 §11.188）；继续监测高并发压力 | M |
| 1.8 | P64 UL watchdog 已定位为 iperf 3.18 同核发送时的 TCP_INFO 统计阻塞；runner 对应组合关闭周期统计，保留逐流结果（§2.4.3） | S |
| 1.9 | UL P4/P16 的 G2 缺口：单核 CPU 基本饱和，预算未触顶；继续降低加密、逐块处理与 carrier 成本（§2.4.2） | M |

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
| 4.1 | C4/C5/C6 完整保留率仍待测；本次 perf 定位 AES-256 加密约占 PPP 样本 34%，AES-128 配对没有消除 G2 差距（§2.4.2） | S |
| 4.2 | 当前 gather=65536 / wait=1 ms 已有效：关闭聚合 P4/P16 降约 26%/20%，关闭等待降约 10%/3%；继续查逐流聚合与每块固定成本，保持当前默认（§2.4.2） | M |
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

1. §2.5 直连桥的预期默认值。
2. M2 的目标数值是否采用 §1 的 G1/G2。
3. Windows/Android 真机测试环境由谁提供、何时可用。
4. 3.6 单连接流水线是否列入本季度范围，还是等 M2 数据后再定。
