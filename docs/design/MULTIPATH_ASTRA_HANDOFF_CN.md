# 多路径工作交接（下一阶段开发）
> Status: Handoff evidence
> Type: Design
> Last verified: 2026-11-20


以下「本轮交接」为当前状态；后面的交付镜像记录仅供追溯，不代表当前分支或测试结果。

## 本轮交接

### 续办实测（2026-11-20）

- 大帧回归：以 `67c72617` 为基线重建。旧传输实现用同一测试夹具重跑 20 次，2 次超时；将完成回调移到 `record_write_mutex_` 和 `secure_mutex_` 释放之后，目标用例 20/20、传输套件 11/11 通过。临时夹具强制走失败清理，进程以 Boost.Test 失败码 201 退出。seal 和 carrier write 仍按原顺序提交。
- Linux 候选 SHA-256 为 `fed9bdf1d561e01e18c91b496fa81df8db0cb59a307f0ada00f11de52f04bdb1`。独立和共享瓶颈各跑固定 `path1`、固定 `path2` 和 Adaptive 三组，统一使用 multipath 认证封装。每组均完成 6 路上传和 6 路下载，逐流核对字节、SHA-256 和回执。
- 用实收有效载荷除以各方向配置的发送窗口。独立瓶颈最佳固定单链为上行 9.83、下行 11.18 Mbps，Adaptive 为 16.46、11.18 Mbps；共享瓶颈最佳固定单链为 9.75、11.08 Mbps，Adaptive 为 11.34、11.16 Mbps。每个拓扑的 Adaptive 都记录了 3 次拨号、3 次附加承载、3 次 `gain.unknown` 和 3 次退役，没有观测到 `gain.keep`、`gain.reject` 或 `idle.retire`。业务完整性通过，扩缩容策略尚未通过。上述每组只有一次，未测尾延迟及重排。
- 四个真实故障用例均在 `asymmetric-5-80-200` 上单独运行，均返回 1。末路径和首路径在 1.5 秒黑洞中未观察到隔离退出，原流分别完整交付 28,508,160 和 15,073,280 字节。全断 1.5 秒时原流完整交付 21,987,328 字节，但两端未记录进入恢复窗口。全断 32 秒时 workload 自己报 socket timeout，两端未记录进入恢复窗口或会话到期。未证明故障路径补连、恢复提交或同一服务端的代次隔离。
- 候选、源码基线和补丁、逐用例结果、两端日志及运行快照保存在本地忽略目录 `build/private/multipath-p0-fed9bdf1/`。下一步要保证注入前目标承载已加入，分别验证黑洞检测和明确断连后的恢复，再将恢复提交、到期、补连绑定到路径与会话代次。池控制需记录基线和试探窗口的 ACK 有效载荷、offer、延迟与重传，定位 `gain.unknown`。

以下旧记录保留用于对照。以前记录的大帧失败和持续负载失败适用于当时的构建。

- 源码定位：`multipath-kcc-default`，记录时 HEAD 为 `c8a4cda2`，工作区另有未提交改动。不要用 HEAD 代替实测二进制的身份，也不要清理或回退无关改动。本轮没有提交或推送。五路最终复核均因模型不可用（404）未产出报告。
- 首链：`VEthernetExchanger.cpp` 对新会话先发 `CreateRequest`，仅在创建响应不明确时进入已认证的查询恢复；多路径主链内层握手后要求 `MultipathNoiseV1`、活动绑定和 exporter，不再重复发起旧认证。普通 TCP/WS 认证不变。
- 上传回执：`vmux_skt.cpp` / `vmux_net.cpp` 双方协商方向性 FIN；此前写入排空后才关闭本地 TCP 写方向，允许反向回执。旧对端保留整流关闭语义。
- 大帧：`TransmissionFrameLimits.h` 统一 Base94 编码后读取上限，不放宽解码后 PPP 或 Noise record 的限制；`ITransmission.cpp` 无 transport cipher 分支在自有缓冲区变换，避免修改 VMUX 保留用于重传的数据。
- 配置语义：VMUX 默认 KCC 风格交付估计不等于接管底层 TCP 拥塞控制，也不读取 XTCP flow 的原生 KCC 遥测。池决策和 per-link 估计分别验收。

### 已跑证据与发布门槛

以下是隔离 Linux 拓扑、真实客户端/服务端及 SOCKS → VMUX 业务，不是 WAN/CDN。最后一轮二进制 SHA-256：`621ebf4ff26ab43e575efd3c4c2b4755745f15e8f93ba151578738904cc90b6c`。本机证据目录 `/tmp/openppp2-repair-check.uM9Rve/release/`：`verification.log`、`direction/result.json`、`scale/{independent,shared}/events.ndjson`；临时目录不是持久制品，不提交原始配置或日志。

| 项目 | 已观测结果 |
|---|---|
| 固定单 carrier 短传 | 上行 94.93、下行 91.87–91.92 Mbps |
| Adaptive 短传 | 上行 34.88–35.08、下行 35.30–35.31 Mbps；短测没有证明多 carrier 聚合 |
| 独立瓶颈持续负载 | 上传阶段活动链 `1→2→1→2`，上传后空闲 `2→1`。上传仅 2/6 流通过，下载 6/6；有 EOF/超时，不通过稳定性验收 |
| 共享瓶颈持续负载 | 上传仅 2/6 流通过，下载 0/6，脚本中止；不能据此宣布已正确识别容量平台 |

短传每方向每轮 8 MiB，要求长度、摘要、精确 EOF 和发送端收到反向回执；速率按接收端有效负载计时。固定与 Adaptive 的载荷封装不同，短传数值不能当作扩容收益。`vmux_receive_semantics_test`、`vmux_net_churn_integration_test` 在最后一轮 2/2 通过；认证策略测试此前 3/3 通过。`multipath_transmission_test` 的 `primary_mux_reads_large_encoded_frames_without_a_second_handshake` 最近有记录的复测仍失败并超时，短传成功不覆盖此失败。

### 下一位先做什么

1. **先解决大帧回归，不发布。** 单独重建并运行上述用例，记录读写、控制 record 和退出状态。优先核查 `MultipathTransmission::SubmitSealedRecord`：同步完成可能在持有 `record_write_mutex_` 时调用可继续提交分片的回调；这是待验证的锁重入假设，不是已确认根因。修复应保持 seal/submit 的顺序、完成通知恰好一次，并保证失败后测试能收敛退出。
2. **再重跑持续负载。** 固定与 Adaptive 使用同构认证封装和同一拓扑；关联拨号、暖机、试验保留/拒绝、退役事件与单链 payload 增量、全池唯一 ACK payload、RTT、重传及业务回执。必须先做到所有流正确完成，再判断独立瓶颈新增链的收益、共享瓶颈拒绝无收益链和空闲回到 floor。只看到链数变化不能区分策略退役与故障退出。
3. **保留未验收范围。** 公网/CDN、故障连续性、全断恢复、新改动的 sanitizer 与跨平台验证均未由本轮覆盖。只在明确授权的隔离环境重跑 PPP；不改宿主路由、DNS 或系统代理。

### 故障矩阵交接

`tests/integration/multipath/run.py production-fault-matrix` 已接入末条路径、首条配置路径、全路径短断及全路径长断四个用例。每例使用不自动重连的 SOCKS → VMUX → echo 流校验字节数、SHA-256 和半关闭。长断分支在旧流失败后另起一对进程检查新会话；这不证明同一服务端内的会话代次隔离。移交变更时须包含尚未跟踪的 `tests/integration/multipath/recovery_workload.py`。

**真实故障矩阵未运行。** 后台前置检查发现默认 `bin/ppp` 不可执行，未启动 PPP、未生成矩阵结果。已有 Python 语法和 CLI 检查、全断后路径门禁独立检查，以及本地 SOCKS 回环（1,350,656 字节）；回环未覆盖最新的新会话分支，不能作为候选内核验收。

取得当前源码构建的 Linux `ppp` 后，在具备权限的隔离环境先加 `--cases single-path-outage` 跑单项，再换新输出目录运行默认四项：

```sh
sudo python3 tests/integration/multipath/run.py production-fault-matrix \
  --binary /path/to/ppp --profile asymmetric-5-80-200 \
  --output ./multipath-results/fault-matrix-first
```

记录源码版本、候选 SHA-256、命令、退出码、各用例 JSON 和两端日志。当前首个用例失败会停止矩阵，剩余项算未测。优先核对以下证据缺口：

1. **实际恢复。** 当前日志匹配没有绑定故障路径、注入窗口或 session generation。短黑洞可能仅由 TCP 重传扛过；进入恢复窗口不等于恢复提交，`last carrier exited; closing session` 也不是宽限到期。应区分隔离退出、补连、恢复提交和到期，并排除 workload 自身超时、数据错误及进程退出造成的长断误判。
2. **实际业务字节。** TBF 只能报承载侧字节，含控制、ACK、重传和封装；应用总量与摘要不能把 payload 归因到每条路径。应接入同一窗口内按 path ID 的 VMUX payload 计数，区分重传与唯一交付；单路径接管须核对黑洞期间的存活路径进展，而非仅在解除黑洞后取增量。
3. **扩展范围。** 独立上传/下载、多流、单向黑洞、RST、共享瓶颈和丢包拓扑以及资源回落未完成故障验收；不能由单流回显推断通过。

## 历史交付记录

日期：2026-11-19。以下记录对应当时的交付镜像。

## 目标

同一客户端通过 TCP 直连、WS/WSS 反代及不同 CDN 入口，并行连接同一服务端进程的同一逻辑会话。首版重点是 VMUX TCP 流聚合、单路径故障连续性与全断后的有界恢复。普通 HTTP CDN 仅使用其支持的 WS/WSS。

不包含多网卡聚合、跨服务端进程共享会话或服务端重启后保留原 TCP 流。UDP、原始 IP、P2P、FRP 的覆盖范围须分别说明。

## 当前状态

- 本交付镜像（分支 `codex/multipath-production`）HEAD 为 `ef966bd4`（feat(multipath): publish session path control events），工作区干净。正式工作树 `/root/.codex/worktrees/multipath-production/openppp2` 的基线为 `10c76d75`（含 13 个提交的历史拆分）；镜像在克隆自该基线后另加三个提交：`baf8255` 统一 Path Control 集成、`d384307` 默认 adapter 能力门控、`ef966bd4` 发布 session path control 事件。此前工作树中的未提交改动已在镜像中提交化，`ppp/`、`tests/`、`docs/`、`ppp.vcxproj` 与正式工作树当前工作状态逐字节一致。
- `multipath_transmission_test` 的 mux 饱和竞态与清理挂死由 `10c76d75` 修复（提交记录：该用例 15/15、完整套件连续两轮）。2026-11-19 在正式工作树以当前源码重建后独立复核：单用例 40/40、完整二进制 5/5、并发 8 实例 0 失败、`ctest -R '^multipath_'` 9/9 通过、ASan 版单用例 10/10。早于 2026-11-18 22:19 构建的测试二进制不含该修复，复跑前必须重建。
- 交付时 `builds/openppp2-linux-amd64-*` 是各 Linux 变体的 CMake 定义，不是二进制。本镜像不含 Linux `ppp` 二进制。现行构建已合并到根 CMake，由 `build-openppp2-by-builds.sh` 选择变体。
- 当前可复用的离线证据包括配置、会话、路径管理、调度、绕行租约、wire codec、安全通道、运行快照、传输认证和 VMUX 接收语义测试；详见[验收清单](../testing/MULTIPATH_ACCEPTANCE_CN.md)。
- 真实 CDN/VPN 业务、多路径实际带宽/尾延迟、主承载故障连续性、全断恢复、跨平台构建和 ASan/UBSan/TSan 新鲜度仍未全部闭环，不能据局部单测宣布生产可用。

## 请优先安排

1. 核对最新改动、正在进行的工作与构建产物，确认协商修正已纳入最终二进制；更新一份可信的通过/失败/未测清单。
2. 用 SOCKS → VMUX 通路验证实际 TCP 业务。FRP 流量不能证明 VMUX 多路径聚合。核对双向字节数和摘要，并证明多条承载实际传输业务数据，不能只看连接数或启动日志。
3. 同环境比较最佳单路径与多路径，覆盖不同 RTT、限速、丢包和共享瓶颈；记录吞吐、尾延迟、重排与重传开销，不预设带宽一定相加。
4. 验证部分入口失败、单路径断开与补连、主创建承载退出、全部断开后宽限内恢复和超时失败；重点检查恢复代次、未确认控制操作对账及旧回调隔离。
5. 检查认证与会话绑定、协商兼容、资源上限和清理；补齐跨平台构建与真实反代/CDN 验证的缺口。
6. 维护验收矩阵和结果证据；任何新结论必须绑定源码提交、命令、退出码和适用范围，不能用旧日志或“连接已建立”替代业务证据。

测试通路提醒：proxy-only 模式下 SOCKS 连接普通业务 IP 可能走本地 Rinetd；连接虚拟子网目标又会提前进入 lwIP/NAT，不能据此证明 VMUX。当前测试方向是以 policy v2 的明确 `proxy` 策略连接服务端命名空间中的非虚拟测试地址，让连接进入 VMUX 分支。先核实调用链和 VMUX 计数，再开展业务矩阵。测试脚本默认二进制位置也需与实际产物核对。

## 工作约束

继续开发时优先复用现有协议、会话和测试设施，避免重复实现；不要提交真实地址、凭据或私有配置。未经明确授权，不启动 PPP 客户端、不修改路由/DNS/代理、不打断活动连接；需要运行隔离 PPP 业务测试时，先确认已有授权范围。

## 参考

- [原始需求](MULTIPATH_REQUIREMENTS_CN.md)
- [详细设计](MULTIPATH_DESIGN_CN.md)
- [安全承载协议](multipath/secure-carrier-v1.md)
- [验收清单](../testing/MULTIPATH_ACCEPTANCE_CN.md)
- [发布与回退](../operations/MULTIPATH_ROLLOUT_CN.md)
- [隔离网络测试设施](../../tests/integration/multipath/README.md)

## 本轮追加交接（2026-11-21）

### 带宽聚合复测

- 当前工作区保留四个未提交改动：`ppp/app/mux/vmux_net.cpp` 以及 `tests/integration/multipath/README.md`、`run.py`、`six_flow.py`。本轮没有提交或推送，也没有清理这些已有改动。
- VMUX 发送采样已加入在途容量判断。`track_sent_frame` 只有在全局数据队列为空、本地链路队列低于水位且承载未达到估算 BDP 时，才将样本标记为应用受限。该改动通过既有 VMUX 单元和集成测试，但仍属于实验性修复。
- 候选二进制 SHA-256 为 `26c810d4e0d023e1c01e60134c050641144d37a5eb8551fd3781a6c4d3b9ad63`。证据目录为 `build/private/multipath-flight-fix.KrEs8B/`，目录已忽略，不应提交其中的凭据、配置或日志。
- 在 `independent-90-125-180`、固定三承载、六条业务流的真实隔离拓扑中，KCC 上传应用有效吞吐约 246.26 Mbps，下载约 299.30 Mbps。EWMA 上传约 297.80 Mbps，下载约 299.30 Mbps。两种模式均完成 6 条上传和 6 条下载，逐流字节数、SHA-256 和回执一致。
- KCC 上传和下载各 5 个六秒窗口均没有同时达到 path1/path2/path3 各自 95% 限速。EWMA 下载 5 个窗口中 2 个达到阈值，上传 0 个达到。该结果不能证明 600 Mbps 持续聚合，仍未通过发布门槛。
- 作为对照，同一拓扑的直接 `iperf3 -P 4` 三路径测试曾观察到接近 100/200/300 Mbps 的各路径窗口，说明 TBF 拓扑本身可以承载目标速率。VMUX 调度、容量估计、应用发送窗口或重排约束仍需继续定位。

### 调度器复测与当前结论（2026-11-21）

- 已在 `ppp/app/multipath/Scheduler.h` 修正探测计时：`probe_eligible=false` 的 VMUX 控制/ACK 选择不再刷新该承载的 payload probe 定时器；新增 `tests/cpp/multipath_scheduler_test.cpp` 回归覆盖，直接编译运行 26 个用例通过。
- 候选 `927cda6ae4ec285ce1b00d3e1baf27bdd217e8b893f4e41dda15e45dafaf7481` 在六 worker、乱序 opaque carrier、36 秒双向六流中不再出现 path 缺 receiver-unique payload；`r10` 的完整性、会话、carrier matching 和 receiver-unique gates 通过，但 per-path 95% rate gate 未通过。有效吞吐约上传 515.47、下载 301.21 Mbps，仍不能宣称持续聚合。
- 同一六 worker 夹具用已知速率控制 `e60bbca120ff75effdeaaed43a52a7d012c1f17494b35a7ca7d72d03377deaf3` 重跑 `r12`：上传三路径均达 95%，下载 path1 每窗约 0.7 Mbps 且 receiver-unique 缺失，确认原先的末承载下载饥饿不是 reducer 映射假象。
- 去掉 `flow_bytes=8 MiB` 的隔离实验 `r11` 仍未达到 per-path gate；该字段不是已证实的单一根因。`r10`/`r11` 证据均保存在忽略目录 `build/private/mux-blind-pacing-matrix-20261121/`。
- 临时将私有 TCP pacing 控制器的 `app_limited` 判定改为忽略仍有 TCP 在途数据或接收窗口受限的样本，`r14`/`r15` 仍未达到 per-path gate，且结果波动；该实验已撤回，不能作为修复依据。
- r13 临时 scheduler trace 显示每条链的 `send_credit` 保持 262144、重传接近零，payload 选路持续覆盖三条链；`upload/download-wire.ndjson` 的路径速率与各 socket 的 `SO_MAX_PACING_RATE` cap 对齐。因此当前直接限速点是私有 blind pacing 控制器的 cap 收敛/回退（含 `app_limited` 样本），不是 scheduler credit 被压空。该 trace 仅作诊断，不属于盲测证据。
- 换序盲测 `r17/swapped-capacity` 的完整性、会话、carrier matching 和 receiver-unique payload 均通过，但 per-path 95% rate gate 失败；上传约 360.09、下载约 397.11 Mbps，不能证明换序后达到三条路径目标速率。
- 在线降速/恢复 `r18/live-rate-drop` 的完整性、receiver-unique payload 和故障期间 reduced-cap gate 通过，但 pre-fault rate gate 失败，path3 恢复窗口仅上传 0.559、下载 0.366 原速，恢复验收未通过。
- 共享瓶颈 `r18/shared-aggregate` 的完整性、会话、carrier matching、receiver-unique payload、aggregate cap 和 utilization gates 全部通过；aggregate utilization median 0.8758，属于共享上限场景的有效结果，但不替代独立三路径聚合验收。
- 结论：探测饥饿修复改善了业务完整性，但没有证明 100/200/300 Mbps 三路径持续聚合；第三轮故障恢复/共享瓶颈实验暂缓，不提交、不推送。

### blind pacing cap 收敛复测（2026-11-21）

- 在同一 `isolated-original`、六流、双向 36 秒、固定承载池和相同隔离 TBF 拓扑上，连续重建并复测候选 `r19`–`r24`。候选 SHA 依次为 `f9fc86eae9307d187c63e02652ae9bb44c1d8f1e0c02c3ddd5e428700ea9de82`、`ecd7e6167b370585a9488d12583ef9272b22ad91f3f5da36157272de83d55b66`、`36099b08f6b56b6e9a1f82111780100498a9665dc3b4fc448d09ee2c930e3044`、`e0e60614872519fee6bc2d4f15ef8724432867b4b396d8328fc323ae859d3ab1`、`720e68c74025f8f66980da8b6b94391692a99d22d4821a1e9dc380c825cf9741`、`3b77025353b358e9fd9c21027fe5769f13834970dabdc1f9204257774f319ab5`。证据均在 `build/private/mux-blind-pacing-matrix-20261121/trials/r19`–`r24/isolated-original/`。
- 私有 pacing 控制器新增并通过离线回归：应用受限中止不回退已验证 cap；失败 probe 保存上界并用二分式下一探测；RTT 增长只有在交付低于 `delivery_floor` 时才回退；已有失败上界未达到前不触发低 cap `probe_converged`。独立控制器测试输出 `tcp_pace_controller_test: all checks passed`。
- 六次实测的完整性、会话身份、carrier matching、receiver-unique payload、aggregate cap 和 transient cap 全部通过，但每次 `per-path-95` 仍失败；因此 cap 收敛修复尚未达到验收。最新 `r24` 上传约 536.03、下载约 260.25 Mbps。最新日志仍显示服务端反向承载有低 bootstrap cap（约 2.66/5.56/5.41 Mbps）及 RTT/二分搜索停留，说明剩余问题不再是单纯 `app_limited` 回退。
- `r24` 的上行窗口大多达到目标，下载窗口仍明显不足；结果在乱序 opaque carrier 和双向竞争下有显著运行间波动，不能用一次成功窗口替代五窗口门槛。未提交、未推送，也未宣布持续聚合完成。

### cwnd/RTT ceiling escape复测（2026-11-21）

- 在 `r24` 后继续加入 socket 自身 `snd_cwnd*mss/srtt` ceiling 以解除过低 bootstrap cap，并将单次跳升限制在 `fast_step` 内；离线回归仍通过。候选 `r25` SHA 为 `834a4a13bd8f393e55102c883feee9d148526427c1226f6712bc9551d2fc031e`，`r26` SHA 为 `d755f5ec8f193d57bff55c82df7b160495ab88f55a6b99078890edcc865a05c0`，证据在对应 `trials/r25`、`trials/r26`。
- `r25` 上传约 528.81、下载约 226.94 Mbps；`r26` 上传约 436.99、下载约 370.93 Mbps。两次完整性、carrier matching、receiver-unique payload 和 cap 上限通过，但 `per-path-95` 仍失败。
- ceiling escape 能使部分承载离开低 bootstrap cap，但也会触发新的 RTT/交付回退和较大运行间波动；`r26` 服务端 path1 仍在约 6–16 Mbps 的探测/回退区间，未证明它接近 100 Mbps。该方向不能作为完成标志，当前仍不得提交、推送或宣布带宽聚合完成。

### 已排除的实验方向

- 曾尝试将每链路发送高水位从 256 KiB 提高到 8 MiB，以覆盖 300 Mbps、180 ms 路径的带宽时延积。完整内核构建因 `websocket.cpp` 编译进程被系统杀掉，未形成候选压测结果。该改动已撤回，当前工作区没有保留高水位参数变化。
- 不能把高水位改动作为修复依据，也不能把单个达到阈值的窗口描述为持续三路径聚合。

### 接手顺序

1. 先以低并发构建或复用已有可执行候选，验证 `PathScheduler::SelectAdaptive` 的实际选路比例、每链路 send credit、`queued_bytes_`、`wire_inflight_bytes`、容量估计和应用受限标记是否同步。重点检查 256 KiB 本地水位、32 MiB可靠重传窗口与 180 ms BDP 之间的关系，避免直接放大常量。
2. 在同一六流脚本中增加按 path ID 的唯一 VMUX payload 统计，并与 TBF 窗口、重传字节和应用回执对齐。只看 TBF 或链数变化不足以证明业务聚合。
3. 先跑短时单向上传和下载，再跑 36 秒双向六流。每次保留候选 SHA、逐窗口 `upload-wire.ndjson`/`download-wire.ndjson`、`client-stats.ndjson`、两端日志和 `six-flow-integrity.json`。
4. 若修复涉及高水位、调度估计或重传窗口，必须补充对应单元测试，覆盖高 RTT、未知容量、应用受限样本和多链路信用耗尽场景。
5. 在三路径持续窗口达到验收标准前，不提交、不推送，不宣布带宽聚合完成。故障恢复矩阵、部署兼容、TSan race 和公网/CDN 仍沿用前文未完成状态。

### 当前工作区状态

- 未提交文件：`ppp/app/mux/vmux_net.cpp`、`tests/integration/multipath/README.md`、`tests/integration/multipath/run.py`、`tests/integration/multipath/six_flow.py`。
- 本轮未修改 PR 隔离 worktree，也未处理 PR #152/#153 冲突。
- 真实网络测试仅使用已授权的隔离 netns。没有改动宿主路由、DNS、系统代理或活动服务。

## 二进制封装和接收背压复测

- 实现：Noise prologue 不变。新端在认证后的 Create、Query 或 Join 准入阶段确认支持才切换二进制外层封装；旧端继续使用 Base94。原有认证和 Noise 加密保持不变。
- 仅切换封装的候选在三路径上传中退出一条承载，三个流因缺帧失败；下载未执行。128 条记录的逻辑队列饱和时原先会关闭承载，回归测试已复现。现在队列满时停止读取、保留一条待入队记录，队列腾出空间后续读，不提高队列上限；释放承载会唤醒等待的泵和消费者。回归覆盖排空和释放。
- 最终 Linux 候选 SHA-256：`06b4ca1c47afe46375e62a897fceb044f4e4cf34be8c41bb85dd50e5da172a47`。本地证据：`build/private/binary-framing-bounded-lANoTO/`；其中 `source-head.txt`、`source.patch`、`provenance.json`、`byte-stage-report.json` 和 `run/` 记录构建身份、命令、计数及两端完整性结果。
- 同一 `independent-90-125-180`、固定三条 TCP 承载、六业务流，先上传 25 秒再下载 12 秒。改前有效吞吐 288.48 / 260.40 Mbps；最终候选 420.26 / 403.31 Mbps。两次均完成六路上传和六路下载，逐流字节、SHA-256 和回执一致。
- TCP 发送字节 / VMUX 发送帧字节（path1 / path2 / path3）：上行由 2.68× / 1.64× / 1.64× 降至 1.01× / 1.01× / 1.01×；下行由 2.73× / 1.66× / 1.64× 降至 1.08× / 1.07× / 1.05×。这是窗口对齐的实测倍率，不是协议固定开销。最终运行观测到接收背压，未见压测中的承载隔离或缺帧超时。
- 最终候选分别作为客户端和服务端，与旧候选 `f0b0bdf9ca2d655a7004dcbc537a6684b9f8e0aa993d654a95924bde0527a139` 做了两次真实互通。固定双 TCP 承载、12 秒双向、每个方向六流全部通过。结果分别在 `legacy-server/` 和 `legacy-client/`。`multipath_transmission_test`、`multipath_secure_channel_test`、`vmux_receive_semantics_test` 均通过。
- `path3` 在上传采样窗的 TBF 速率约 183–196 Mbps，仍明显低于 300 Mbps 配额；封装开销已不再是该轮的主要差额，600 Mbps 持续聚合、故障恢复和跨平台/公网部署门槛仍未通过。没有提交或推送。

## Path3 未跑满定位（2026-11-21）

- 单独只启用 path3 时，三个 6 秒上传窗口的 TBF 线速为 299.95、299.68、299.49 Mbps；六流业务有效吞吐为 265.82 Mbps。三路径同时运行时，path3 只有 196.19、182.52、183.80 Mbps；同一 TCP 承载没有重传，`cwnd` 中位数约 12,842，`snd_wnd` 约 23 MiB，问题不是 path3 的 TBF 或 TCP 丢包。
- 三路径上传稳态窗口中 path3 的 VMUX 观测中位数：`capacity_rate` 约 24.6 MB/s、`wire_inflight` 约 4.10 MiB、`queued_bytes` 约 192 KiB、`send_credit` 约 64 KiB、调度权重 25、队列延迟约 34 ms。path1/path2 的容量估计约 12.9/24.2 MB/s。VMUX 数据队列深度为 0、`rtx_parked` 为 0；path3 的 TCP `send_q` 约 3.8 MiB，说明下层 TCP 尚有空间，但 VMUX 没有持续供给该承载。
- 发送端 TCP 采样把三路径窗口标为 `app_limited`，而 VMUX 的 `app_limited` 标记没有误报为应用受限。单 path3 时 TCP 仅少数采样为 `app_limited`，`send_q` 约 6.9 MiB。当前容量估计取实际已交付速率；path3 被调度到约 180–196 Mbps 后，估计稳定在约 24.6 MB/s，`SelectAdaptive` 的到达时间比较继续减少它的份额，形成自抑制反馈。每秒一次的单帧 probe 不足以把 path3 推入新的高容量测量轮。
- 将 path3 RTT 从 180 ms 改为 90 ms，上传仍为约 196.7、206.1、192.2 Mbps；因此 RTT 不是单一根因。将估计器切换为 EWMA 后为 203.3、179.4、172.5 Mbps，BBR 后为 167.0、163.4、178.0 Mbps，均未解除供给不足；估计器模式本身不是修复。
- 24 流对照中客户端 `rtx.bytes` 达到约 33.55 MiB、24 个 flow 进入 `rtx_window.hold` 并最终超时。这证明 32 MiB session retransmission window 是高并发的第二个限制，但不是 6 流 path3 低于配额的首要原因。下一步应先为 adaptive path capacity 做有预算的连续探测/避免低估反馈，再单独复测 6 流；不要直接放大 256 KiB 高水位或 32 MiB 常量。

## Adaptive Probe Burst 设计与单测（2026-11-21）

- `PathScheduler` 保留原有每 `probe_interval_ms` 选择一次候选路径的节奏；一旦选中 probe，新增每路径的剩余字节预算，在预算耗尽前连续选择该路径。默认预算为 16 MiB，构造参数可缩小，硬上限为 32 MiB；候选的 `probe_budget_bytes` 仍可进一步收紧，且每个 frame 必须同时满足路径状态、active-probe 能力和真实 `send_credit`。
- burst 只改变调度选择，不绕过 VMUX 的 256 KiB 本地高水位、链路撤出、可靠重传窗口或发送信用。预算按实际 payload frame 大小递减；不足一个 frame 的预算不会启动 probe。路径代次变化会通过现有 tracker 清理丢弃旧 burst。
- 单测覆盖：预算内连续三个 frame 后回到最佳路径；候选预算限制为两个 frame；小于一个 frame 的候选预算不触发 probe。`multipath_scheduler_test` 已通过（1/1）。
- 本轮尚未证明生产三路径吞吐改善。下一步先用新候选完成短时三路径上传/下载，再按原验收脚本跑持续六流；仍不放大高水位或重传窗口，不提交、不推送。

## Probe Burst 三路径复测结果（2026-11-21）

- 候选由当前工作树 `build/multipath-binary` Release 构建，SHA-256：`46e93a7fb6e52f332f69a1ce2b92edd3b55b3c4f123c3545b6721d9b69087e69`。证据目录：`build/private/probe-burst-YCkinb/`；保留 `upload-run1/`、`download-run1/` 和 `bidirectional-run1/` 的逐窗 wire、path telemetry、日志及逐流完整性结果。
- 独立三路径、固定每路径一条承载、六流单向 smoke 均通过完整性校验：上传有效吞吐 `332.86 Mbps`，下载 `316.56 Mbps`。上传逐窗 TBF 约为 `path1 93.5/94.1/72.0`、`path2 199.4/187.7/200.0`、`path3 25.7/26.9/97.7 Mbps`；下载约 `path1 71.3`、`path2 199.5`、`path3 97.1 Mbps`，未达到三路径持续配额。
- 36 秒双向六流同样通过完整性校验，但有效吞吐为上传 `231.24 Mbps`、下载 `234.92 Mbps`。上传五个窗口的 path3 仅 `18.3/24.0/24.3/1.8/13.1 Mbps`，下载为 `25.1/24.8/128.9/116.4/183.6 Mbps`；该设计在本候选上没有证明带宽聚合改善，不能宣布验收通过。
- 结论：probe burst 的单元语义已确认，但生产效果仍为负/不稳定。下一步应先分析 probe 选择、实际 send credit 与 path3 代价的逐调用对应关系，再决定是否调整 burst 预算或调度公平性；不直接放大高水位或重传窗口，不提交、不推送。

## 逐次 Scheduler Trace 定位（2026-11-21）

- 诊断候选 SHA-256：`0cf8925949467253e7d5e5ed3d90eb27513cbcadb42f6782b2b994ddcb8f4faa`；证据目录：`build/private/scheduler-trace-88vN71/`。环境变量 `OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON` 输出每次 `tx_link_pick` 的 sequence、payload、status、选中链路、probe/burst 剩余预算及全部候选的 send credit、queue、wire inflight。
- 12 秒上传 trace 的客户端有 6,679 次调用，6,654 次为 new payload；path1/path2/path3 选中业务 payload 分别约 136.14/186.38/110.23 MB。path3 候选在 new-payload 调用中 6,626/6,654 次有足够 frame credit，queue 中位数为 0，wire inflight 中位数为 64 KiB；它没有因为本地队列或 send credit 被持续阻塞。
- 同一 trace 中 6,020/6,654 次 new-payload 调用同时存在至少两个正的 burst 剩余预算。path3 有正预算但未选中的调用约 4,951 次，其中全部发生在另一个 burst 已占用调度时；path2 同类约 3,183 次。path1 的 queue 中位数为 256 KiB、send credit 中位数仅 4 bytes，却仍有 4,072 次候选为零 credit，说明其旧 burst 在 credit 恢复后会重新参与竞争。
- 根因已缩小为 **每路径 tracker 可同时持有 burst，drain loop 按 `rx_links_` 顺序扫描并优先返回第一个可用正预算 tracker**。这绕过了 arrival cost、容量、权重和 RTT 的比较；高 RTT 的 path3 即使 credit 充足、queue 为空，也会被较早链路的 burst 抢占。trace 还显示 VMUX 实际 probe interval 为 `10 ms`，不是 Scheduler 默认值，因此多个 path 的 burst 很快重叠。
- 这组 trace 启用了逐调用落盘和 flush，只用于归因，不能作为吞吐基准。已添加 `tests/integration/multipath/analyze_scheduler_trace.py`，可报告 zero-credit、queue/flight 限制、burst overlap 与连续选路；未提交、未推送。下一步应先用单一 burst owner 或按 cost/fairness 选择 active burst 的最小修复及单测，再关闭 trace 复测三路径。

## 单一 Probe Owner 修复复测（2026-11-21）

- 修复：`PathScheduler` 增加显式的单一 burst owner。owner 仍满足真实 send credit、状态和 active-probe 准入；owner 暂时不可发送、代次消失或预算耗尽时清理所有旧 tracker 预算，再按正常 arrival cost 选择下一次 probe。没有放大 VMUX 256 KiB 高水位、32 MiB 重传窗口或 session credit。
- 新增回归用例 `adaptive_probe_keeps_only_one_burst_owner`，覆盖 owner 因 credit 阻塞后启动另一条 probe 时旧预算被清理。`build/test/multipath_scheduler_test` 21/21 通过；`build/multipath-binary` 的 `ppp` Release 目标构建通过。构建仍有现存 jemalloc allocation-size 和 GCC 不识别 `-Wno-*` 选项警告，无新增错误。
- 直接候选短测：实际二进制 SHA-256 为 `51450512e196b93eadefd425153d5365c79ce78bb25dd74af36a8810eb10e1a4`；证据目录 `build/private/scheduler-owner-fix-20261121/`。`independent-90-125-180-stable`、固定三路径、每路径一条承载、六流、上传 12 秒通过逐流字节/SHA-256/回执/EOF 校验，实收 `549,748,736` 字节，`364.28 Mbps`。
- 同一候选启用逐调用 trace 的短测证据在 `build/private/scheduler-owner-trace-20261121-run/`；角色 wrapper 只为分离 client/server trace，实际二进制 SHA 仍为上述值。客户端 8,340 条、服务端 1,219 条 trace 均无 JSON 错误；任一调用同时有正 `probe_remaining_bytes` 的链路数最大为 1，`overlap_records=0`，probe 未选 owner 次数为 0。客户端 path1/path2/path3 业务 payload 分别为 `126,908,248`、`266,336,540`、`148,879,837` 字节，trace 短测实收 `542,015,488` 字节、`360.65 Mbps`。
- 结论：单一 owner 约束已在生产候选的隔离三路径上传短窗中证明消除了旧的 burst 重叠归因；它没有证明 600 Mbps 持续聚合，也没有覆盖下载、双向、故障恢复或跨平台部署。下一步仍需关闭 trace，按持续上传/下载/双向验收矩阵复测；在此之前不提交、不推送、不宣布带宽聚合完成。

## 单一 Probe Owner 后的持续验收与实验结论（2026-11-21）

- 当前默认候选为 `e015d803874a41f478ea9f3bbb34dae2701bc859a59396316702cb8e93ce4aed`，证据目录为 `build/private/current-owner-upload12-20261121/`、`current-owner-upload36-20261121/`、`current-owner-download12-20261121/` 和 `current-owner-bidirectional36-20261121/`。这些运行均使用 `independent-90-125-180-stable`、固定三路径、每路径一条承载、六流、`--tcp-turbo --pool-metrics`，六流字节、SHA-256、回执和 EOF 均通过。
- 12 秒上传有效吞吐为 `398.33 Mbps`；36 秒上传为 `430.04 Mbps`。36 秒上传的五个 6 秒 TBF 窗口分别约为 `path1 69.9–85.1`、`path2 124.7–185.5`、`path3 195.1–242.8 Mbps`，没有一个窗口同时达到配置限速的 95%（95/190/285 Mbps）。
- 12 秒下载有效吞吐为 `385.46 Mbps`，唯一采样窗口约为 `path1 79.8`、`path2 175.4`、`path3 191.3 Mbps`。36 秒双向运行仍完整通过，但有效吞吐仅上传 `257.54 Mbps`、下载 `221.18 Mbps`，各路径窗口也未达到聚合门槛。
- 同一候选的单路径上传对照均完整通过：`path1 91.27 Mbps`（TBF 约 100）、`path2 174.36 Mbps`（约 200）、`path3 243.47 Mbps`（约 298.5）。单路径 TBF 可跑满而多路径 VMUX 未跑满，故障仍在 VMUX 调度/在途供给/可靠传输耦合，不是拓扑限速失效。
- 已完成但未保留的实验：probe burst 从 16 MiB 收紧到 1 MiB，上传 `396.38 Mbps`；Scheduler interval 改为 1 s，上传 `405.46 Mbps`；强制 `stripe`，上传 `418.53 Mbps`；以容量归一化 service time 为主成本，上传 `396.17 Mbps`。均未达到持续三路径门槛，改动均已撤回。将 VMUX 本地高水位改为 1 MiB 的构建在 600 秒内未完成链接，没有形成候选或压测证据；当前源码仍为 256 KiB，不能把该方向当作结果。
- 结论：单一 probe owner 的重叠归因已消除，但当前候选仍不能证明 600 Mbps 持续聚合。没有提交或推送；故障恢复、跨平台、真实 CDN/反代及持续尾延迟仍未验收。

## 高水位与 TCP 在途窗口实验（2026-11-21）

- 以当前单一 owner 候选 `e015d803874a41f478ea9f3bbb34dae2701bc859a59396316702cb8e93ce4aed` 做的 LD_PRELOAD 诊断仅把 Linux `TCP_NOTSENT_LOWAT` 从 256 KiB 改为 4 MiB；不改 VMUX 源码。六流上传完整性通过，`notsent-lowat4m-upload12-20261121/` 的有效吞吐为 `432.01 Mbps`，与默认 12 秒 `398.33 Mbps` 同一数量级，不能证明 TCP notsent 低水位是首要瓶颈。
- 将 VMUX 本地高水位及对应 TCP notsent low-water 临时改为 1 MiB，候选 SHA-256 为 `68a369a54d83c04899aa8248943869d0ecc47e0fbc78a89e2fd4137ad9bdedab`，证据目录为 `build/private/highwater1m-upload12-20261121/`；六流完整性通过但有效吞吐仅 `154.15 Mbps`，path3 TBF 窗口为 `0 Mbps`。
- 将同一实验改为 4 MiB，候选 SHA-256 为 `f2c33b8db6f356504f67bcd82c9427f02883d01989ea59709c2d5f21ec3ad2de`，证据目录为 `build/private/highwater4m-upload12-20261121/`；六流完整性通过但有效吞吐仅 `192.85 Mbps`，path3 约 `15.7 Mbps`。两个高水位实验均已撤回，当前源码和恢复后的 Release 构建仍使用 256 KiB；不能把放大高水位作为修复或发布建议。
- 12 流对照也未提高供给：当前 owner 候选的 12 秒上传完整性通过，`current-owner-upload12flows-20261121/` 为 `404.44 Mbps`，低于 36 秒六流的 `430.04 Mbps`。六流应用并发数不是当前 600 Mbps 缺口的直接证据。
- 结论仍不变：当前最强证据指向 adaptive probe/容量反馈与 VMUX 在途耦合；没有达到 600 Mbps 持续三路径窗口，不提交、不推送、不宣布聚流完成。

## 容量模型重新探测复测（2026-11-21）

- `PathScheduler` 现在只在候选已有新鲜容量模型、且当前 delivery rate 比容量低至少 20% 时，允许 `at_capacity` 路径重新进入 probe；未知或过期容量仍保持原有保护，不会盲目探测。新增 `probe_revalidates_a_capacity_model_when_delivery_lags`，并保留未知容量跳过和 active burst 跨容量状态的回归覆盖。
- `build/test/multipath_scheduler_test` 的 25 个测试全部通过；`build/multipath-binary` 的 `ppp` Release 目标构建通过。当前候选 SHA-256 为 `c9b11016cc2137825bee4137c468b6047b5862ce5288bed42629e1c00ea3875f`。
- 短测和持续矩阵证据在 `build/private/capacity-revalidate-20261121/`：上传 12 秒 `397.57 Mbps`，下载 12 秒 `377.30 Mbps`，36 秒双向上传/下载分别 `256.29/245.19 Mbps`；六流完整性、字节数、SHA-256、回执和 EOF 均通过。容量重新探测没有改善此前的持续聚流缺口。
- 角色分离 trace 在 `build/private/capacity-revalidate-trace2-20261121/`，客户端 `9,546` 次调度、无 JSON 错误；path3 仍有 `4,331` 次选中、`282,423,468` 字节业务负载，但其 12 秒 trace 实收仅 `407.04 Mbps`。path3 的 send credit 大多数时间可用，未证明本地 frame credit 是唯一瓶颈。
- 结论：容量重探测是有界、可回归的安全策略，但未达到 600 Mbps 持续三路径验收；不提交、不推送，仍需围绕可靠传输在途供给或 ACK/应用受限反馈取得新的因果证据。

## 仅提高 VMUX 高水位的对照（2026-11-21）

- 完整重建在 600 秒超时，未链接；随后从已验证的 Release 静态库在私有目录重新链接，基线 ELF 和按当前源码重编的 `vmux_net.cpp.o` 均与归档逐字节一致。实验只替换该对象中的 VMUX 每承载高水位（256 KiB → 1 MiB，亦用作未知/重传承载信用的启动窗口），TCP `TCP_NOTSENT_LOWAT` 仍为 256 KiB。未替换工作区默认二进制。
- 两组使用同一隔离拓扑 `independent-90-125-180-stable`，参数为 `six-flow-integrity --fixed-pool --carriers-per-path 1 --direction upload --duration 12 --tcp-turbo --pool-metrics`；每组三条 TCP 承载，退出码 0，六流字节、SHA-256、回执和 EOF 均通过。
- 基线 `c9b11016cc2137825bee4137c468b6047b5862ce5288bed42629e1c00ea3875f` 实收 609,189,888 字节、404.74 Mbps；实验 `c9fc3a3380fa018b678fc8d140ae089e6bf39ff0f49f4585f5d697a03c1faea5` 实收 210,010,112 字节、139.40 Mbps。约 6 秒稳态窗的 path1/path2/path3 TBF 速率分别从 77.23/161.93/215.87 降为 70.33/58.83/23.44 Mbps。path3 接收端唯一 VMUX payload 从 156,082,932 降至 16,774,656 字节；两个窗口均未记录 VMUX 重传字节。
- 证据：`build/private/mux-local-write-ahead.kTb0iX/` 下的 `relink.py`、`build.log`、`control-run/` 和 `experiment-run/`，含候选 SHA、命令、退出码、逐窗线速、双方统计及完整性结果。1 MiB 改动只留在私有实验目录，生产源码保留 256 KiB；不提交、不推送。
- 结论：只放大 VMUX 本地高水位在这次配对短测中也明显退化，并非先前同时改变 TCP 低水位才出现的退化。旧 trace 中 path3 在 8,507/9,508 次业务帧调度调用有足够信用；本地高水位不能按 RTT 直接等同于 ACK 在途窗口。下一次对照应固定两个水位，按 path ID 对齐有信用但未被选中、落选代价、ACK 在途和接收端唯一 payload，验证 path3 供给不足的条件。此次未跑下载或持续双向，600 Mbps 验收仍未通过。

## 线程瓶颈与保留线格式的 Shuffle 优化（2026-11-21）

- 在 `independent-90-125-180-stable`、100/200/300 Mbps、每路径一条 TCP 承载、六业务流的双向 36 秒对照中，归档基线 `c9b11016cc2137825bee4137c468b6047b5862ce5288bed42629e1c00ea3875f` 实收上/下行 `254.47/244.11 Mbps`。活跃窗客户端、服务端最忙线程的 CPU 占用中位数为单核的 `98.9%/99.5%`；该结果是线程瓶颈证据，不是整机 CPU 上限。只把两端 `concurrent` 由 1 提到 4 得到 `188.75/411.30 Mbps`，仍没有一窗三路径同时达标。
- 保留改动：`ppp/cryptography/ssea_simd/ssea_simd.h` 的非二幂大小 `shuffle_data_opt` / `unshuffle_data_opt` 每帧计算一次倒数，逐字节以整数乘法和一次校正得到完全相同的余数和交换顺序。未改变 Wire 字节、加密或认证。`ssea_misaligned_tail_test` 1/1 通过，包含旧实现编码字节和新实现解码的对照。
- 私有 Linux Release 候选 `a5af49c81df4a64f3f68edf60ce9c87c022a183f6c0d6816a8034aa4602478db` 由已复验摘要的基线归档和当前源码编译的 `ssea.cpp.o` 重新链接；与私有原型逐字节一致，不是整个当前工作树的完整重建。36 秒单向上传 `491.57 Mbps`、下载 `464.33 Mbps`；36 秒双向 `325.60/276.24 Mbps`。三个运行的各方向均通过六条流的字节、SHA-256、回执和 EOF 检查；每方向五个窗口，三个 path ID 在每窗均交付唯一 VMUX payload、共用同一 VMUX 会话，且每路径只匹配一条承载。95/190/285 Mbps 三路径逐窗门槛四个业务方向均为 `0/5`；双向两方向之和不作聚流通过依据。
- 仅用于定位的配对候选在现有二进制协商后跳过强制 legacy payload 变换、保留配置的外层 AES 和 Noise，双向实收 `472.11/412.60 Mbps`，门槛仍为 `0/5`；它变更现有封装版本的线字节，因此不保留到生产源码，也不与旧端混跑。三个并行单 TCP `iperf3` 对照在同拓扑的双向合计约 `518.56/522.72 Mbps`；该数字是一次 TCP 应用有效吞吐参照，不替代 TBF 门槛。
- 证据：`build/private/mux-shuffle-source-20261121/` 中的编译身份和摘要；`build/private/mux-capacity-oracle-20261121/` 中的 `shuffle-source-{upload,download,bidirectional}-36/`、`profile-bidirectional36/`、`shuffle-source-gate.ndjson`、`diagnostic-summary.ndjson` 及 `*-cpu.ndjson`。未提交、未推送；只运行于已授权隔离 netns。600 Mbps 持续聚合仍未通过。

### 每承载执行器与同窗 TCP 对照

- 仅为主链 VMUX 子通道分配独立 strand 时，4-worker 对照双向为 `234.09/385.93 Mbps`，未解决子承载和底层收发共用执行器的问题。将底层 TCP 移到独立 strand 的第一版在服务端准入后异常退出；旧准入协程仍使用原 strand 的裸指针。将原 strand 的共享引用捕获在准入协程中后，诊断候选 `dd31d3f1980dd21be59d0046cff0f3d243c3252a97754bd8333335076932ccae` 正常完成两轮 36 秒双向六流校验。`concurrent=4` 为 `519.38/251.31 Mbps`（三路径 TBF `0/5`、`0/5`）；`concurrent=6` 为 `393.91/483.08 Mbps`（`0/5`、`1/5`）。迁移和寿命修复仍在私有候选中，未进入生产源码，也未通过断线与关闭竞态矩阵。
- 不经过 PPP/VMUX 的匹配对照：同一拓扑、每路径一个原生 TCP socket、同样的 36 秒和五个 6 秒 TBF 窗口。`iperf3` 单向上传有效吞吐合计 `543.75 Mbps`，三路径线速门槛 `3/5`；双向为 `491.51/522.71 Mbps`，门槛 `0/5`、`1/5`。这不是拓扑的理论上限，但证明原生单 TCP 对照也未稳定通过当前双向门槛；不能把全部缺口归因于 PPP，也不降低验收门槛。
- 同样 `concurrent=6`，把线格式兼容的 shuffle 优化与生命周期修正一起使用，候选 `6e79a07527403ef70b4dfdc9e6fc58d37562bd78ea0ff1880253b1cff859d055` 得到 `494.17/372.20 Mbps` 和 `1/5`、`0/5`；变化没有在双向同时兑现。候选仍为诊断用途，证据在 `build/private/mux-capacity-oracle-20261121/carrier-shuffle-six-bidir36/`。
- 证据：`build/private/mux-carrier-executors-lifetime-20261121/`、`build/private/mux-capacity-oracle-20261121/carrier-executors-{lifetime,six}-bidir36/` 和 `build/private/mux-direct-tcp-wire-20261121/`。

### 固定权重与 BBR 复测

- 在相同三承载、六业务流、6-worker 双向负载中，仅把调度改为按已知的 `1:2:3` 容量比例分配，仍受真实 send credit 约束。诊断候选 `67892aacc115b12c1ef30b629022f54542f07b106d277760531e94d2a18171ae` 的双向实收为 `534.36/479.86 Mbps`，三路径 TBF 同窗达标 `2/5`、`0/5`。两方向五个窗口都验证了同一 VMUX 会话的三路径唯一业务 payload。固定权重是实验参照，不是正式配置或修复；证据在 `build/private/mux-capacity-oracle-20261121/bidirectional-oracle-six-bidir36/`。
- 原生 TCP 对照的 `iperf3` JSON 明确记录了两端都使用 BBR。显式在新建隔离命名空间指定 BBR 的复测仍是 BBR 对 BBR，不是算法切换：直接单向上行合计 `538.51 Mbps`、同窗达标 `2/5`；直接双向 `495.71/514.97 Mbps`、`1/5`、`1/5`。同一 `6e79a075…` 候选的双向复测为 `435.57/445.51 Mbps`，两个方向都为 `0/5`。证据在 `build/private/mux-direct-tcp-bbr-20261121/` 和 `build/private/mux-capacity-oracle-20261121/carrier-shuffle-six-bbr-bidir36/`。两次 BBR 跑分的方向差异说明单次实验不足以量化优化增益；持续聚合仍未通过。
- 用实测容量替代 `1:2:3`、仅当三条路径均有新鲜估算时启用同一分配规则，候选 `14ff90a1a320f8106085d73b33ef8f93902abcfcacbe604d0fad25874143360f` 完成双向六流，但只有 `305.13/424.85 Mbps`，同窗达标 `0/5`、`0/5`。上行 path3 前三个窗口的容量估算中位数为 `38.2/52.2/59.1 Mbps`，线速 `30.9/41.0/63.9 Mbps`；同时本地 send credit 中位数仍有约 `192/128/96 KiB`。最后一窗估算与线速才到 `300.4/298.5 Mbps`。说明该规则可在有本地信用时维持对快路径的低估，且此私有实验有估算后不走原 probe 选择，不能把退化单独归因于权重公式；不保留策略。证据在 `build/private/mux-capacity-oracle-20261121/measured-fairness-six-bidir36/`。
- 真正切换到 CUBIC（仅隔离命名空间）后，原生 TCP 单向上行合计 `554.90 Mbps`，`5/5` 窗口三路径 TBF 均达标；双向却只到 `420.38/406.52 Mbps`，两个方向都为 `0/5`。双向 path1 上行为 `33.04 Mbps`、path2 下行为 `42.67 Mbps`；原生 path1 的平均 TCP RTT 约 `1.13–1.17 s`，远高于配置的 `90 ms`。同一 `6e79a075…` PPP 候选用 CUBIC 双向实收 `474.87/311.88 Mbps`，三路径都有唯一业务 payload（两个方向各 `5/5`），TBF 均为 `0/5`。证据：`build/private/mux-direct-tcp-cubic-20261121/` 与 `build/private/mux-capacity-oracle-20261121/carrier-shuffle-six-cubic-bidir36/`。这验证了原拓扑的单向线速门槛可达，不支持把 CUBIC 作为双向修复，也没有改变验收门槛。
- 同样 BBR、6-worker、36 秒双向再做一轮配对，并在 client/server 各采集三条 TCP socket 的 `ss -tin`。固定权重和普通调度分别实收 `439.42/505.21` 与 `496.26/353.09 Mbps`，TBF 达标分别为 `1/5、1/5` 与 `1/5、0/5`。固定权重的末窗上行 path3 只有 `117.15 Mbps`，但同窗 TCP 中位数仍有 `207,064 B` 待发，`unacked/cwnd` 同为 `5,370` 段，RTT `594.51 ms`、最小 RTT `180.05 ms`；该窗不是仅靠 VMUX 增加选路就能解决的空队列。另一次固定权重运行中，30 个上行快照只有一个总 VMUX flight 到达 `31 MiB`，下行零个；当前证据也不支持直接增大 32 MiB 默认总窗口。证据在 `build/private/mux-capacity-oracle-20261121/{oracle,carrier-shuffle}-six-bbr-sockets-bidir36/` 和相邻 `*-sockets.ndjson`、`*-tcp-summary.txt`。仍未达到持续聚合验收。
- 固定权重对照只改一个已逐字节复验的 TCP 对象，在三个测试 socket 上设置 `SO_MAX_PACING_RATE` 为各自线速，保留原 BBR、TBF、ACK 窗口和调度。候选 `026a20519bff372f60dda4d2939102711fc9ba0090114282825e8ab704974d27` 的 `ss` 证明两端三条 socket 都应用了 `100/200/300 Mbps` 上限。双向六流实收 `531.14/527.98 Mbps`，各方向 `2/5` 达到三路径同窗门槛；每窗三路径仍各有唯一 VMUX payload。上行 path3 的线速范围从无上限配对的 `117.15–299.97` 提升到 `276.79–300.07 Mbps`，窗口 TCP RTT 中位数由 `512.74–597.38` 降到 `202.83–239.18 ms`。证据在 `build/private/mux-oracle-pacing-20261121/` 和 `build/private/mux-capacity-oracle-20261121/oracle-pacing-six-bbr-bidir36/`。只证明这一轮压低了排队与深度塌陷；已知容量和地址是实验条件，未写入生产源码，仍不能认定持续聚合通过。
- 固定权重下重复做 CUBIC 配对：不设 pacing 上限为 `529.62/424.34 Mbps`，同窗 `3/5`、`0/5`；设置相同上限为 `520.48/551.64 Mbps`，同窗 `2/5`、`4/5`。两次的每个窗口均有三路径唯一 payload，六流校验均通过。pacing 同样改善了这一轮较弱方向，但仍未持续达标。设上限后，双方 VMUX 总在途在 `12/60` 个窗口内快照达到至少 `31.9 MiB`，其中上行 `8/30`、下行 `4/30`。与未限速 BBR 个案不同，此裁剪条件下有必要单独对照会话级 32 MiB 窗口；不同时修改 8 MiB 每流窗口。证据为 `build/private/mux-capacity-oracle-20261121/oracle-pacing-six-cubic-bidir36/` 和 `oracle-six-cubic-sockets-bidir36/`。
- 保持同一 `026a2051…` 候选、固定权重、TCP pacing 与 CUBIC，仅用隔离配置把会话窗口从 `32 MiB` 改为 `48 MiB`（每流仍为 `8 MiB`）。双向实收 `543.19/552.55 Mbps`，原 `95/190/285 Mbps` 同窗上行 `3/5`、下行 `5/5`；所有窗口都是同会话三路径唯一 payload，六流完整性通过。总在途峰值分别为 `42.4/47.0 MiB`，有 `20/30`、`22/30` 个样本用到了旧上限以上的空间。下行首次在这组双向诊断中跑满全部验收窗，上行仍失败；不是正式自适应候选，也不是改变默认窗口的依据。证据：`build/private/mux-capacity-oracle-20261121/oracle-pacing-six-cubic-window48-bidir36/`。
- 32 MiB 组的两端日志记录上行 `8,148`、下行 `3,516` 次会话窗口 hold；48 MiB 组均为 `0`，但仍有上行 `954`、下行 `3,187` 次每流窗口 hold。48 MiB 组最慢的上行 path2 窗为 `153.40 Mbps`，TCP 待发中位数 `166,856 B`，`unacked/cwnd` 为 `6,408/6,409` 段；并非单靠继续加会话窗口可以解释。正常自适应调度加相同 pacing（32 MiB）则实收 `479.35/482.94 Mbps`，同窗 `0/5、0/5`；仍有 `885.15/954.47 MiB` 的帧被标记为探测发送。证据在 `adaptive-pacing-six-cubic-bidir36/` 及上述对照的 `six-flow-0-{client,server}.log`。
- 只在容量未知或过期时允许 probe，其他选路不变。诊断候选 `c754b46e2873c89ccdc03818e9d2d5365bd2e4ac2eb2eafb5265c6105db28d82` 在 CUBIC、相同 pacing、32 MiB 窗口下，探测选择降至客户端 `15` 次、服务端 `6` 次；双向实收仍只有 `465.93/457.13 Mbps`，同窗均为 `0/5`，三路径及每流校验通过。没有证据支持把持续缺口归因于探测次数，未保留此策略。证据在 `build/private/mux-capacity-oracle-20261121/adaptive-bootstrap-probes-six-cubic-bidir36/`。

### 双向线速首次全窗通过（仅诊断候选）

- 保持固定 `1:2:3` 权重、CUBIC、48 MiB 会话和 8 MiB 每流窗口，仅把 TCP pacing 上限从已知路径线速的 `100%` 降为 `97%`，为反向 ACK 留出余量，TBF 与 `95/190/285 Mbps` 门槛不变。候选 `53db47e74f03dd7eccd25fe4dc99bb7a02799e57964fbae64b25fef92a0be5a0` 在 36 秒双向六流中实收 `556.88/554.75 Mbps`。上、下行均 `5/5` 窗三路径达标，同一 VMUX 会话每窗验证三路径唯一业务 payload，各方向六流字节、SHA-256、回执和 EOF 均通过。窗口 path1/path2/path3 测得 `99.07–100.09 / 196.74–200.21 / 296.67–300.25 Mbps`（两方向合并范围，不加总两方向）。
- 对比相同 CUBIC 和 48 MiB 组，path3 的窗口 RTT 中位数范围从上行 `457–794`、下行 `455–829 ms` 降为 `278–445`、`263–441 ms`；每流 hold 上行 `954→69`、下行 `3,187→286`，两组会话级 hold 均为 `0`。两端日志及 `ss` 确认 `97/194/291 Mbps` socket pacing 生效。证据在 `build/private/mux-oracle-pacing-slack-20261121/` 与 `build/private/mux-capacity-oracle-20261121/oracle-pacing-slack-six-cubic-window48-bidir36/`。这是一个真实隔离 TCP 聚流通过结果，不是 600 Mbps 应用有效载荷，也未证明自动估速、一般网络、WS/WSS 或可发布生产策略。
- 同 SHA 和参数独立复测：实收 `558.94/550.26 Mbps`，上行 `5/5`、下行 `4/5`。下行第三窗 path1 和 path2 分别为 `92.43/185.24 Mbps`，path3 为 `299.41 Mbps`；该窗未过门槛。两次共 `19/20` 个方向窗口达标，所有窗口均有三路径唯一 payload，逐流完整性通过。这是接近线速的可复测证据，不是可重复的全窗通过。证据在 `build/private/mux-capacity-oracle-20261121/oracle-pacing-slack-six-cubic-window48-bidir36-repeat/`。
- 同样的 `97%` TCP pacing、CUBIC 和 `48/8 MiB` 配置，仅用最大可用发送信用选承载、同信用时优先累计发送更少者，移除固定 `1:2:3` 调度权重。诊断候选 `f27d84e9c92d8d6698dd94cf11b073c383db5491f3319f3c3dfc0ae22186a9e7` 实收 `554.87/550.28 Mbps`，双向均 `5/5` 全窗通过，六流完整性和同会话三路径唯一 payload 均通过。该候选仍依赖已知路径速率设置 socket pacing，不等于自动估速或生产默认。证据：`build/private/mux-credit-first-slack-20261121/`、`build/private/mux-capacity-oracle-20261121/credit-first-slack-six-cubic-window48-bidir36/`。
- 发送信用候选同 SHA 独立复测为 `553.67/549.97 Mbps`，上行 `5/5`、下行 `4/5`；下行第三窗 path1 仅 `85.81 Mbps`，path2/path3 仍为 `200.03/300.00 Mbps`。两轮累计 `19/20` 个方向窗口达标，所有窗口均为同一会话三路径唯一 payload，六流完整性未退化。固定权重并非达到线速的必要条件，但暂未复现连续两轮全窗通过。证据在 `build/private/mux-capacity-oracle-20261121/credit-first-slack-six-cubic-window48-bidir36-repeat/`。
- 仅撤去 socket pacing 上限，保留发送信用策略、CUBIC、48/8 MiB、拓扑及负载。对照 `f15dd0b9500a0a7904905677001d55f38084a98e2362e02f5166f480ec9824d9` 实收 `470.54/247.23 Mbps`，三路径同窗 `2/5、0/5`，但每窗仍三路径有唯一 payload，逐流完整性通过。上行 path3 由 `299.88` 降到末窗 `111.32 Mbps`。这组对照不能用仅改选路来替代 TCP pacing；证据在 `build/private/mux-capacity-oracle-20261121/credit-first-unpaced-six-cubic-window48-bidir36/`。
- **隔离验收复现通过：** 发送信用策略不变，TCP pacing 为已知路径线速的 `96%`（`96/192/288 Mbps`），保留 CUBIC 和 `48/8 MiB`。候选 `e60bbca120ff75effdeaaed43a52a7d012c1f17494b35a7ca7d72d03377deaf3` 连续两轮 36 秒双向六流，应用实收分别为 `555.57/555.12`、`555.78/554.53 Mbps`。每轮每方向 `5/5` 个 6 秒窗口均达到原 `95/190/285 Mbps` 门槛，共 `20/20`；各路径跨方向、跨轮次最低实测线速为 `98.39/196.63/294.86 Mbps`，单方向三路径同窗线速合计约 `589.88–600.50 Mbps`。每窗同一 VMUX 会话、每路径一个 TCP socket 且均有唯一业务 payload；逐流字节、SHA-256、回执和 EOF 全部通过。两端日志分别确认三个 pacing 上限生效，两轮均未记录会话级或每流窗口 hold。
- 可复验结果：`build/private/mux-credit-pacing96-20261121/` 中的源码差分及重链接脚本；`build/private/mux-capacity-oracle-20261121/credit-pacing96-six-cubic-window48-bidir36{,-repeat}/` 中的候选与逐窗证据。已验证的是已知线路上限的 Linux TCP 隔离配置；未修改生产默认、未提交或推送，不声称 600 Mbps 应用净荷、自动估速、混合 WS/WSS 或故障恢复通过。

## 当前源码重建与三方向复测（2026-11-21）

- 重建与单测：`ninja -C build/multipath-binary -j16 ppp` 成功；`bin/ppp` SHA-256 为 `0a938ddaa23fa81536e3f29854f2ebc5212d57ba8d8f6d7ed391431bfe517e86`。`ninja -C build/test -j16 multipath_scheduler_test && build/test/multipath_scheduler_test --log_level=test_suite` 运行 26 个用例，输出 `No errors detected`。
- 候选身份：三方向运行均由 wrapper 注入 `OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON` 后执行 `bin/ppp`。wrapper SHA-256 分别为 upload `37e43a240a8742338bb31c769bb2644f19ad002acec3de969ff633563f43b8ec`、download `f0d3a3068af4de6349871b57c50e1bc831ccf6d2e0fec7b7b96f1f9b1b01f078`、bidirectional `b870ff2ab5e1be4b388602d1142f85f36b901e1b0daa693d7c047e5211566fef`；这些是 trace wrapper 的 SHA，不是另一个 ppp 候选，实际被测 `ppp` 均为上述 `0a938dda…` 的 `bin/ppp`。`runs/<方向>/candidate.json` 记录的 `sha256` 与各 wrapper 一致。
- 证据路径：`build/private/mux-current-source-20261121/runs/{upload,download,bidirectional}/`。拓扑为 `independent-90-125-180-stable`，36 秒、六业务流、每路径一条 TCP 承载、固定池、`--tcp-turbo`；每组输出 `PASS: 6 verified flows per selected direction`，各方向 `six-flow-integrity.json` 为 `passed=true`，六个流的字节数、SHA-256、回执和 EOF 均通过。
- 实测业务有效载荷：upload 2,256,633,856 B、500.904 Mbps；download 1,857,159,168 B、411.559 Mbps；bidirectional upload 1,496,219,648 B、331.940 Mbps，download 1,979,613,184 B、439.511 Mbps。
- 业务完整性通过不等于持续三路径聚合通过：本轮没有给出每个 6 秒窗口按路径的 `95/190/285 Mbps` 同窗达标统计，也没有达到 600 Mbps 持续聚合验收；此前的高线速结果属于固定拓扑与参数下的隔离诊断对照，不能外推到本轮重建的 `bin/ppp`。
- probe gate 诊断：当前未提交源码在 `ppp/app/multipath/Scheduler.h` 新增 `ProbeGate` 位掩码（NotEligible、NotProbeEligible、InvalidState、AtCapacity、Budget、Best、NoCredit、ProbeInterval、Recency、NoTracker、ProbeOwner），以及 `needs_capacity_probe`、`can_probe`、`gate_mask`、`last_selected_ms` 和探测剩余字节；`ppp/app/mux/vmux_net.cpp` 在 trace 的候选记录中新增 `needs_capacity_probe`、`probe_can_probe`、`probe_gate_mask`、`probe_last_selected_ms`、`probe_diagnostic_remaining_bytes`，运行时另有 interval、clock、owner 字段。仅在设置 `OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON` 的 trace 环境启用该诊断采集。
- 本轮 bidirectional trace 为 client 26,538 条、server 33,831 条；其中 `selected_probe=true` 分别为 13,420 和 13,623。三方向 trace 均包含新增的 `needs_capacity_probe`、`probe_can_probe`、`probe_gate_mask`、`probe_last_selected_ms`、`probe_diagnostic_remaining_bytes` 候选字段，以及 interval、clock、owner 字段。旧 trace 的 `probe=0` 来自更早候选，不能代表 `0a938dda…` 这份重建源码。
- 双向 gate 统计按每条 trace 的每个 candidate observation 计数，不把多个 bit 当互斥类别。client 共 79,614 个候选观察：`probe_interval` 41,673（52.3%）、`recency` 31,807（40.0%）、`probe_owner` 33,656（42.3%）、`best` 26,480（33.3%）、`at_capacity` 19,182（24.1%）、`no_credit` 11,186（14.1%）、`not_probe_eligible` 10,839（13.6%）；server 共 101,493 个：`probe_interval` 51,774（51.0%）、`recency` 50,415（49.7%）、`probe_owner` 36,352（35.8%）、`at_capacity` 46,645（46.0%）、`best` 33,831（33.3%）、`no_credit` 13,950（13.7%）、`not_probe_eligible` 10,779（10.6%）。`mask=0` 分别为 client 4,476（5.6%）、server 3,415（3.4%）。
- 选中分层：双向 client 26,480 次选中，其中 13,420 次为 probe（50.68%）；server 33,831 次选中，其中 13,623 次为 probe（40.27%）。所有 `selected_probe=false` 的选中 candidate 都带 `best` bit（client 13,060/13,060、server 20,208/20,208）。`mask=0` 的 client 4,476 个观察中有 3,308 个（73.9%）被选为 probe、994 个（22.2%）在同一帧未被选中、174 个（3.9%）来自无 best 的 backpressure 默认诊断；server 的对应数字为 2,952（86.4%）、463（13.6%）和 0。按路径的 selected/probe 为 client path1 11,942/7,226、path2 6,191/3,562、path3 8,347/2,632；server path1 11,579/3,904、path2 7,079/2,739、path3 15,173/6,980。
- `mask=0` 既非实际 probe 的必要条件也非充分条件：它在 selected_probe 中仅占 client 3,308/13,420（24.6%）、server 2,952/13,623（21.7%）；同一帧未中选的 mask=0 candidate 分别有 994 和 463 个。上述计数按 candidate observation 统计；无 best 的 backpressure 记录未执行真实 gate 计算，默认 mask=0 不应解释为“所有 gate 放行”。
- gate 字段是新 probe 尝试的静态资格快照，采集发生在 active probe owner 分支之前；因此已进入 burst 的 `selected_probe=true` 记录仍可能带 `probe_interval`、`recency` 或 `probe_owner` bit，不能把这些 bit 直接解释为该次 probe 被拒绝。双向被 probe 候选中，带非零 mask 的比例仍分别为 client 75.4%、server 78.3%；`probe_owner` 在这些被 probe 候选中为 0，但记录级 owner 已激活，体现了采集时序错位。统计能直接证明的是命中分布和字段对应关系；“双向 server 的 `at_capacity`/`recency` 更高导致其 probe 率低于 client”仍是相关性，不是因果证明。
- 与单向对照：upload 的 client/server probe 率为 36.75%/0.33%，download 为 0.49%/41.43%；双向为 50.68%/40.27%。双向两端均有大量 probe，且业务完整性通过；这不能替代按 path、按 6 秒窗口的持续聚合验收。
- 未完成与状态：本轮只在既有的未提交工作树上继续了 `ppp/app/multipath/Scheduler.h`、`ppp/app/mux/vmux_net.cpp` 和忽略目录中的实验输出；工作树此前已有大量未提交改动，不能声称干净。未提交、未推送，也不声称持续三路径聚合或 600 Mbps 验收完成。所有运行仍限于已授权的隔离 netns。

## P0 容量回退与有界公平债务及隔离复测（2026-11-21）

- 实现（`ppp/app/multipath/Scheduler.h`）：P0 容量回退把容量样本的可用性独立于投递/指标新鲜度。`CapacityIsFresh` 只认 `capacity_rate_bytes_per_second` 非零且 `capacity_updated_ms` 在 `MetricsMaxAgeMs`（5 s）内；过期或零容量不会被提升为新鲜容量，`ProjectCandidate` 遇到不新鲜或低置信度样本仍回退保守 RTT/吞吐估计，因此新鲜 delivery rate 在容量样本过期后仍可参与调度，不会被错误按旧容量定价。
- 有界公平债务替换原先按 session 累计的 `FairnessTime`：按承载 `id + generation` 维护 tracker；每次 `Select()` 差分单调的 `sent_payload_bytes`，用当前吞吐信念把字节差定价成服务毫秒；每 `FairnessDecayHalfLifeMs = 2000 ms` 衰减一半，单次记账上限 `FairnessDebtCapMs = 2 × half-life = 4000 ms`。稳态不动点约 `1.44 × 2000 = 2880 ms`，低于上限，因此 cap 只裁剪瞬时超发，不改变稳态份额。
- 两种关键修正：容量估计被上调时，存储债务按 `priced_rate/current_rate` 立即重定价（即使 sent counter 冻结、没有新字节也会执行），避免早期低估把快路径长期钉住；`sent_payload_bytes` 回退视为新 session epoch，直接清债而不是继续累加。字节服务时间保留小数毫秒再转换为整数，避免逐帧四舍五入系统性高估快路径。
- 单测：`tests/cpp/multipath_scheduler_test.cpp` 现 29 个用例，运行输出 `*** No errors detected`。新增覆盖 `expired_capacity_keeps_a_fresh_delivery_rate_schedulable`（容量过期不被提升为容量，但新鲜 delivery rate 仍可调度）、`early_low_rate_service_does_not_pin_a_corrected_fast_path`（早期低速率债务在速率修正后立即下降，不必等 512 s 生命周期）、`session_length_does_not_inflate_bounded_service_debt`（epoch 有界，停发后债务消退）。
- 构建：`ninja -C build/multipath-binary -j16 ppp` 成功；仅出现既有 allocator/compiler warning，没有新增告警。当前被测 `bin/ppp` SHA-256 为 `cc420c82341d406126fec6e947238b39ca098f4116e1f59159d45d84ee35151c`。
- 完整 C++ 离线套件：`bash scripts/run-cpp-tests.sh` 全部 210/210 通过，multipath label 10/10；日志在 `/tmp/multipath-cpp-tests.log`。
- 隔离复测（失败）：证据 `build/private/mux-bounded-fairness-20261121/`，候选 SHA 同上，driver/fixture 退出码 0，六业务流完整性、字节、SHA-256、回执和 EOF 通过；reducer `review` 退出码 1。拓扑 `independent-90-125-180-stable`，36 秒双向、5×6 秒窗口、每路径一条 TCP 承载、6 流、CUBIC、会话/每流窗口 48/8 MiB。
- 吞吐：upload `479.03 Mbps`（2,157,838,336 B），download `226.84 Mbps`（1,024,000,000 B）。逐路径 30 个方向窗失败 16（upload 3、download 13）：上行 w0–w3 近满（约 88.7–100.1 / 195.8–200.1 / 298.0–300.2 Mbps），w4 降至 `88.67/123.68/134.16 Mbps`；下行 w0–w3 严重不足（窗口聚合 70–255 Mbps，三路径全部不达标），w4 聚合回升到 490 Mbps，path1/path2 通过（98.58/199.16 Mbps），path3 仅 `192.39 Mbps` 失败。逐窗数据见 `summary.json` 的 `window_aggregate_mbps` 与 `per_path_gates`。
- 与最近同合同上一轮 `f15dd0b9500a0a7904905677001d55f38084a98e2362e02f5166f480ec9824d9`（`build/private/mux-blind-pacing-matrix-20261121/trials/r27/isolated-original`，15/30 失败窗）相比，本轮 16/30 未改善；下行仍是从低供给单调爬升的形态，不是调度份额不足的形态。
- 判定：业务完整性通过不等于 95% 三路径利用率通过；bounded fairness 是有界、可回归的正确性改进，但不是本次 600 Mbps 三路径同窗验收的最终修复。未提交、未推送；所有运行仅限已授权隔离 netns，不宣称目标完成。
- 下一步应优先分析下行早期供给，而不是继续调公平债务常量：w0–w3 下行窗口聚合从 70→168→255 Mbps 的爬升更像接收端/ACK/执行器或窗口启动受限。应按 path 对齐采集早期窗口的下行发送信用、VMUX/每流窗口 hold、ACK 在途、接收端唯一 payload、两端执行器占用和 socket 待发，并对照改变初始窗口/执行器分配或 ACK 供给；在拿到因果证据前不要继续调 `FairnessDecayHalfLifeMs` 或 `FairnessDebtCapMs`。

## capacity-gap probe 撤回、PathDialer socket 选项与最终验证（2026-11-21）

- 本轮结束时保留在生产源码的改动只有两组：上一节的 P0 容量回退与有界公平债务（`ppp/app/multipath/Scheduler.h`），以及承载拨号的 socket 选项一致性修复（`ppp/app/multipath/PathDialer.cpp`）。容量模型重探测、固定权重、pacing、48 MiB 窗口、gap probe 等实验性策略都没有留在工作树。
- PathDialer 修复内容：`PathDialer::Connect` 以前直接自建 TCP socket、不经过主链的 `NewAsynchronousSocket()`，因此承载 socket 一直使用内核默认缓冲区和传输选项。现在 `socket->open()` 成功后会先调用 `Socket::SetWindowSizeIfNotZero(native_handle, configuration->tcp.cwnd, configuration->tcp.rwnd)` 与 `Socket::AdjustSocketOptional(*socket, endpoint.address().is_v4(), configuration->tcp.fast_open, configuration->tcp.turbo)`，再执行 protect 回调和 `async_connect`；窗口覆盖与 keepalive、TOS、`TCP_NODELAY`、Fast Open 必须在 connect 前生效，语义与 `VEthernetExchanger` 的主链一致。同时 `open` 失败分支改为显式 `Closesocket` 后 `continue`，不再与 protect 失败共用同一条短路表达式。
- 新增 `tests/cpp/multipath_path_dialer_test.cpp`（1 个用例，`tests/cpp/CMakeLists.txt` 注册为 `multipath;isolated` label，并加入 sanitizer 的 `-fno-sanitize=vptr` 例外表）。用例在 loopback 上起 acceptor，用真实 `PathDialer::Connect` 拨号，并在 transmission factory 里对 `native_handle` 断言 `TCP_NODELAY == 1`、`SO_KEEPALIVE == 1`、`SO_SNDBUF`/`SO_RCVBUF >= 64 KiB` 的配置值，从而锁定“窗口与传输选项在 connect 前应用”这一行为。
- capacity-gap probe 已完整实现并**完整撤回**：源码中不再存在 `gap`/`probe_gap` 相关符号或字段，`bin/ppp` 现为 `aeacf13f2794ef75633da071d808dc49546b3fb14a8a76360c5c3e44cc3ab4d6`（撤回后重建，不含 probe 实现）。
- 全双向对照（`build/private/mux-gap-probe-full-bidir-20261121/`）：候选 `9226adcd0b1a5af08ad6483f60d9e9cc448e016227746177a577ad9a9bb850ae`（P0 + bounded fairness + capacity-gap probe + PathDialer options）在同一 `isolated-original` 合同（`independent-90-125-180-stable`、36 秒双向六流、每路径一条 TCP 承载）跑了三次，逐路径同窗 gate cells 分别为 `10/30`、`9/30`、`10/30`。三次中唯一三路径全部达标的窗口是第三次运行的上行 `w0`（预热窗），**稳态三路径全通过窗口为 0**。作为基线，旧 bounded fairness 候选 `cc420c82…` 的配对运行为 `12/30`、历史运行为 `14/30`，且上行分别有 `1/5`、`4/5` 个窗口三路径全通过，其中包含稳态窗口。探针没有提升共同上限：三次运行的双向线速总量均在 `675–873 Mbit/s`，远低于 1000 Mbit/s 级别的路径和，而候选把上行 probe 字节占比推到 `45–61%`（`0.59242/0.60632/0.45011`），下行 `18–48%`（`0.18003/0.24047/0.47637`），高于旧候选的 `0.33996/0.18299` 与 `0.34665/0.45479`。
- rate300 gap probe 对照（`build/private/mux-reverse-rate-20261121/rate-300-gap-probe/`，同一候选）：上行 gate 失败 `11/15`（基线 `13/15`），下行失败 `10/15`（基线 `11/15`）。上行三路径均值从 `91.754/164.463/55.498` 变为 `96.863/106.185/97.528 Mbps`，分配确实更均匀，但线速总量反而由 `311.714` 降到 `300.577 Mbps`；下行三路径为 `50.448/198.272/256.355 Mbps`，总量 `505.075`（基线 `483.091`）。per-path 统计进一步显示 path1 该窗口的 `probe_selection_share` 为 `0.93`、`probe_bytes_per_unique_payload` 为 `0.9753`，即 probe 字节几乎等于承载上的唯一业务 payload——探针在把有效业务当作“探测”，不能用它解释或补齐容量。
- 判定：capacity-gap probe 不能保留，也不能宣称修复；它同时增加了探针开销与选路偏差，且没有任何一轮出现稳态三路径全通过。
- PathDialer 修复的性质必须说清：它只让承载 socket 与主链使用相同的窗口和传输选项，是正确的配置一致性修复；但在 gap probe 候选 `9226adcd…` 中该修复已经存在，三次全双向与 rate300 仍然失败，因此**未证明** socket 选项（缓冲区、`TCP_NODELAY`、keepalive、Fast Open）是吞吐根因，也没有任何证据显示它改变了 gate 结果或线速。
- 撤回后的验证（本机，未提交）：
  - `ninja -C build/test -j16 multipath_scheduler_test multipath_path_dialer_test`：两个目标均 `*** No errors detected`；scheduler 29 个用例，PathDialer 1 个用例。
  - `ninja -C build/multipath-binary -j16 ppp`：成功，仅出现既有 allocator/compiler warning，无新增告警。
  - `bash scripts/run-cpp-tests.sh`：全套 `211/211` 通过，multipath label `11/11`，包含新增的 `multipath_path_dialer_test`。
- 当前状态：仍然没有达到持续 95% 三路径同窗目标。业务完整性通过、单元套件通过、构建通过都不等于 gate 通过；仍未提交、未推送，所有网络运行只发生在已授权的隔离 netns。
- 下一步：不再盲调 probe/fairness 常量，也不再把 PathDialer socket 选项当作吞吐修复。需要的是真实因果修复——按 path 和 6 秒窗口对齐采集两端的早期供给（发送信用、VMUX 与每流窗口 hold、ACK 在途、接收端唯一 payload、执行器占用、socket 待发），先定位每轮都出现的共同上限（双向线速总量 `675–873 Mbit/s`）究竟在哪一层产生，再决定改哪个机制。
 
## VMUX 延迟观测双向复测（2026-11-21）
 
 - 使用当前重建 `bin/ppp`，设置 `OPENPPP2_MULTIPATH_SCHEDULER_TRACE_JSON`，在 `independent-90-125-180-stable`、固定三路径单承载、双向六流、48 秒窗口运行。证据目录为 `build/private/instrumented-bidir-20261121-r2/`；六条选定方向流均通过字节、SHA-256、回执和 EOF 完整性校验。
 - trace 解析无空行、无非法 JSON、无无效记录，共 63,776 条调度记录，覆盖单一 session。三条链的 `tx_post_wait_us` 平均为 94.2/136.0/126.7，最大为 13,496/6,210/9,262；`rx_dispatch_us` 平均为 91.7/119.9/103.4，最大为 13,043/5,317/5,895。VMUX strand dispatch 不是稳定的毫秒级瓶颈。
 - `tx_write_latency_us` 平均为 2,632/2,980/2,099，最大为 554,677/456,922/404,883；该指标包含 `ITransmission::Write` 内的传输处理和底层写完成等待，说明双向负载下存在长尾写完成等待，但当前观测不能把它拆成 crypto、socket backlog 或远端 ACK。`rx_handler_us` 平均仅 5.2/7.3/6.0 微秒；未发生记录级 `rx_backpressure`。
 - 三条链的队列高水位均为 256 KiB；零信用观察主要是 queue-limited（path1 10,800、path2 8,684、path3 5,531），flight-limited 仅 81/0/118 次。trace 中 probe 仍占大量选择：选中字节分别为 705,490,323、1,089,290,020、385,018,125；因此不能把 probe 选择误判为可用业务容量。
 - 因果判断：post-wait 和 VMUX handler 不支持“共享 VMUX strand 是主要稳定瓶颈”；写完成长尾是下一层候选，但现有边界不足以安全拆分为 crypto 或 socket 原因。没有足够证据实施生产修复，未继续改调度常量、probe 或执行器拓扑。`crypto_us` 保持 `null`，避免伪造精度。
 - 本轮只证明 instrumented binary 的业务完整性和观测可用性，不证明 95% 三路径门槛；尚未获得按 path×6 秒窗口的持续 gate 通过证据。下一实验应在 `ITransmission::Write/Read` 边界分别记录 crypto 时间、socket write/backlog 和完成等待，再决定是否修改传输写入批量或窗口策略。

## 自动 pacing 与 BDP 信用接入现状（2026-11-21）

- 工作树范围：当前分支在既有未提交改动之上，新增自动每承载 TCP pacing（`ppp/app/multipath/PathPacingPolicy.h`）与 BDP `AdaptiveFlightCredit`。未提交、未推送；所有网络运行仍只在已授权隔离 netns。
- `PathPacingPolicy` 语义：容量只在新鲜时可用（`DefaultFreshnessMs = 5000`），候选需 `DefaultMaturityMs = 1000` 成熟，回退按 `SlackPermille = 960`（96%）；只有 `telemetry.at_capacity` 之后才施加 cap；Linux 通过 `SO_MAX_PACING_RATE` 设置，未知或过期容量清除上限，不支持的传输为 no-op。
- BDP 信用：`AdaptiveFlightCredit` 用实测 BDP 加一个派生 BDP 头寸计算目标；当前发现实验将 `FlightHeadroomDivisor` 设为 1，以保证 provisional BDP 处仍有完整一个 BDP 的探索余量。`adaptive_send_credit` 只在多承载、本地信用非零且 `telemetry.at_capacity` 时采用飞行信用，否则返回本地信用。
- 单测：`multipath_scheduler_test` 30 个用例、`path_pacing_policy_test` 9 个、`multipath_transmission_test` 21 个、`multipath_path_dialer_test` 1 个，均为 `*** No errors detected`。
- 构建：`ninja -C build/multipath-binary -j16 ppp` 成功，仅出现既有 compiler/allocator warning。
- 通用隔离六流 CLI、当前二进制、默认生成的 32 MiB 配置：`build/private/autopacing-bidir-20261121-r1` 完整性通过，upload `9.632 Mbps`、download `9.038 Mbps`；放开发现期信用后 `...-r2` 完整性通过，upload `24.526 Mbps`、download `12.383 Mbps`。
- 纠正前一版统计：`at_capacity` 不在 `client-stats.ndjson.runtime.multipath.links[]`，而在 `path-telemetry.ndjson.client_start.paths[]`。r2 的 telemetry 窗口中三条路径均多次为 true；当时的容量估计分别只有约 `3.60/1.62/0.93 MB/s`，且 inflight 被 provisional BDP 自锁。后续加大探索余量后，r4 完整性通过，upload `332.117 Mbps`、download `245.069 Mbps`；延迟 pacing 后 r5 为 `309.713/268.266 Mbps`，仍未达到门槛。
- 判定：以上只证明新 policy 与信用路径可构建、可回归且业务完整性通过，不证明 95% 三路径同窗，也不声称自动估速已在一般网络成立。历史成功诊断要求 48 MiB 会话、CUBIC、已知 `96/192/288 Mbps` pacing 且 credit-first，属于隔离对照，不是生产证据。
- 下一因果要求：先让容量发现能在不知道拓扑的情况下自举。可测方向是配置化容量提示，或无上限发现阶段，并重做 estimator/probe，使 `at_capacity` 能在没有预先已知速率的路径上被真实触达；在拿到该证据前不再调 pacing 常量，也不宣称验收通过。
 
 - 判定：上述结果证明 BDP 探索余量能把通用合同从 24 Mbps 提升到约 332 Mbps，但仍未证明持续 95% 三路径同窗；自动容量估计仍低于 `100/200/300 Mbps` 线路，不能宣布成功。
 - 下一因果要求：容量发现必须在未知拓扑下继续增长到真实 plateau，且不能把 provisional BDP 或 pacing cap 反馈成新的上限。需要继续验证 KCC full-bandwidth 状态、probe owner/credit 交互和 session/per-flow window；在新候选获得按 path×6 秒窗口的 gate 证据前，不再宣称验收通过。

## Probe 时钟与最佳路径复测（2026-11-21）

- `Scheduler.h` 将普通选择时钟与 probe 时钟分离，并允许自身容量落后的 best path 进入 probe；新增确定性 scheduler 回归后，`multipath_scheduler_test` 32 个用例通过，`ninja -C build/multipath-binary -j16 ppp` 成功。构建仍有既有 allocator/compiler warnings。
- `build/private/autopacing-bidir-20261121-r6` 六流完整性通过，upload/download 为 `313.697/303.692 Mbps`；path3 probe 不再冻结，但 path1 仍仅约 `6.8 MB` probe、`0.72 MB/s` capacity，说明首次修复未解决 best-path 竞争。
- `build/private/autopacing-bidir-20261121-r7` 六流完整性通过，upload/download 为 `281.320/276.658 Mbps`。三路径 probe bytes 均持续增长（path1/2/3 分别约 `222.7/230.6/224.0 MB`），probe counts 约 `3408/3525/3418`，容量估计最高约 `15.0/18.1/17.6 MB/s`；探测公平性恢复，但吞吐低于 r6，仍远未达到持续 95% 三路径门槛。
- 结论：probe starvation 的两个明确门已修复并有单测与端到端完整性证据，但自动估速/调度仍未证明聚合验收。r7 的低容量平台要求下一步回到传输写入等待、socket backlog、有效发送窗口与 KCC 采样质量的因果分析；不得把该轮结果宣布为带宽聚合完成。
- `git diff --check` 仍被工作树既有未提交改动中的 trailing whitespace 报告阻断，未对无关历史改动做格式化清理。

## 每记录流水线、自动 pacing 与三路径复测（2026-11-21）

- 本轮改动仍在未提交工作树中。接收侧改为逐条认证记录排队：`MultipathTransmission::QueueLogicalRecord` 在队列或字节上限不足时只保留当前这一条已认证记录并挂起 `pump_waiter`，不再让 `ApplicationPump` 直接 `break` 退出；承载读取因此随 VMUX 接收暂停一起暂停，而不是退役一条健康承载。`ReadLogicalBytes` 累积到完整逻辑帧后才唤醒读取方，`CloseLogicalChannel` 和 `Dispose` 同时唤醒数据等待与泵等待。相关覆盖包括 `full_mux_receive_queue_resumes_without_disposing_carrier`、`dispose_wakes_pump_waiting_for_mux_receive_queue_space` 以及逻辑写入的整段提交/失败回归。
- `Scheduler.h` 在上一节“probe 时钟与 best path”基础上补齐启动信用地板：已知 BDP 时 `AdaptiveFlightCredit` 以 `bdp + bdp / FlightHeadroomDivisor`（当前 divisor 为 1）作为目标和信用地板，再受 session 份额上限约束；未知 BDP 才回退 startup。最终下限取 `max(minimum_window, startup_window, ...)`，避免极小已知 BDP 把信用量化成单帧而锁死估计。信用合同断言已随该语义更新。
- `PathPacingPolicy.h` 的自动 pacing 语义：发现阶段按 `DiscoverySlackPermille = 1250` 给测得容量保留 125% 余量；结束发现必须同时满足 `at_capacity`、容量样本发布时间晚于本次 headroom grant、已过一个成熟窗且没有 ≥5% 增长。未知或过期容量一律清 cap；未知 payload→wire 比例也清 cap 并拒绝猜测（`UnknownWireRatio`）。VMUX 目前只观测交付 payload、没有 wire 字节计数器，因此不填该比例，自动 pacing 在拿到真实比例前不施加 cap，避免把 payload 速率当成 wire 速率。
- 承载执行器：客户端每条子承载分配独立 strand，拨号、socket 读写完成和 relay 不再排在 VMUX session strand 或其他 path 之后；VMUX strand 只保留 mux 全局状态。服务端没有做同等改造。单独把 primary 业务拆到独立 strand 的实验已撤回，只保留子承载独立 strand。
- 测速 harness 新增 `--concurrent 1..64`，同时覆盖服务端和客户端 trial 配置，并把解析值写入 `topology.json`、`run-evidence.json`、`six-flow-integrity.json` 与实际 trial JSON；未改 `tools/compat` 模板。
- 单测/构建证据（本轮文档落笔前运行，本次未重跑）：`path_pacing_policy_test` 13 个用例、`multipath_scheduler_test` 34 个、`multipath_transmission_test` 23 个，均输出 `No errors detected`；`ninja -C build/multipath-binary -j16 ppp` 成功，只有既有 allocator/compiler warnings。
- 复测合同统一为 `independent-90-125-180-stable`、固定池、每路径一条 TCP 承载、双向六流、`--tcp-turbo --pool-metrics`。所有轮次六流字节、SHA-256、回执和 EOF 均通过；下列吞吐是 harness 的逐路径 TCP wire TBF 窗口值（Mbps），不是唯一业务 payload，门槛为每路径 `95/190/285 Mbps`。
- r13（36 秒、`--concurrent 4`、发现期 pacing 生效，候选 `195af7d1089999c32481ce12b3a58014ce1ca82c64d29f0cacb81839f0087531`，`build/private/autopacing-bidir-20261121-r13/`）：完整性通过，聚合 upload/download 为 `507.105/490.362 Mbps`。逐路径 5 个 6 秒窗口的 upload 中位数 `94.5/185.5/277.7`，download `90.9/183.5/260.5`；没有一条路径稳定达到 95% 线速。
- r15（60 秒、`--concurrent 4`、发现完成门生效，候选 `97d53050d2fca1178305a38ceb613743019c588c31b234659ff7ee115dc6036b`，`...-r15/`）：聚合 `528.415/463.308 Mbps`。9 个窗口的 upload 中位数 `98.1/191.5/287.8`，三路径中位数均略高于门槛，但窗口最小值仅 `92.3/176.2/262.5`，仍有窗口不达标；download 中位数 `85.7/177.6/267.6`，path1 与 path2 明显不足。
- r16（60 秒、`--concurrent 4`、未知 wire 比例禁用 payload 当 wire 的 pacing，候选 `3181422a8ef0fc5d15e1df2ecc65b26e6c6f7eef99d7e245ed8d817e572d55ac`，`...-r16/`）：完整性通过，聚合 `527.533/498.939 Mbps`。upload 中位数 `95.3/187.1/287.0`，download `77.9/189.2/277.8`。相对 r15，下行聚合提高约 35.6 Mbps；这是同一合同下的相关对照，不能证明该改动是唯一原因。download path1 仍是短板（窗口最小值 `73.3`），三条路径没有同时持续达标。
- r17（60 秒、`--concurrent 4`、primary 独立 strand 实验，候选 `c2ff396116703861bcf2fba86d752e1efdcf73422ab5d0868856f8bf53ab473b`，`...-r17/`）：完整性通过但结果为负。upload 聚合 `527.712 Mbps`、中位数 `94.1/188.8/273.9`；download 聚合骤降至 `272.681 Mbps`、中位数 `12.7/11.2/273.3`，即下行几乎全部挤到 path3，path1/path2 被饿死。primary 分离已撤回，只保留子承载独立 strand。
- 判定：所有轮次只证明完整性和观测可用，没有证明持续 95% 三路径同窗利用率，也没有达到 600 Mbps 级应用净荷验收。r13/r15/r16 的 upload path2/path3 已接近或略超门槛，但 path1 始终在 95 Mbps 附近；download 的 path1/path2 更差。r16 的未知比例处理提升了聚合下行，但不是逐路径公平修复。不得把任一单轮聚合值或完整性通过写成三路径目标完成。
- 下一因果阻塞不再是 probe/fairness 常量，而是 primary 与服务端侧的路径调度和连接语义：r17 已证明简单拆分 primary strand 会让下行集中到单条路径，说明两端对 primary/child 的绑定、服务端接收/派发路径和 VMUX 连接到 path 的映射仍在决定下行供给。应先在服务端按 path×6 秒窗口对齐采集连接/承载归属、接收派发、调度选择和信用/在途，再决定是否修改 primary 语义或服务端调度；在拿到该因果证据前不再调 pacing 常量或 fairness 常量，也不宣称验收通过。
- 当前状态：未提交、未推送；工作树包含大量既有未提交改动，不能声称干净。所有网络运行只发生在已授权隔离 netns。

## 发现信用上界与 r20/r21 复测（2026-11-21）

- 本轮只改发现信用的上界，不改变其他调度语义。`PathScheduler::AdaptiveFlightCredit` 新增 `discovery_budget_bytes`：仅当 `at_capacity` 为真（由新鲜容量样本产生）且交付速率至少低于容量样本 20%（`CapacityProbeNeeded`）时，才在 provisional BDP 目标之上追加发现信用，session 份额仍是硬上限；成熟路径的 budget 为 0，不保留永久超额信用。`vmux_net::adaptive_send_credit` 以探测剩余字节（无剩余时用默认 burst 预算）与 `PPP_MUX_LINK_BYTE_HIGH_WATER`（256 KiB）的较小值作为单次计算上限，避免一次把整个探测 burst 放成在途。当前工作树保留的是 256 KiB 上界版本；未提交、未推送，未写入生产默认。
- 单测与构建（本轮未重跑，沿用本轮已报告结果）：`multipath_scheduler_test` 36 个用例通过，`ninja -C build/multipath-binary -j16 ppp` 成功。
- 复测合同统一为 `independent-90-125-180-stable`、60 秒双向六流、`--concurrent 4`、固定池、每路径一条 TCP 承载、`--tcp-turbo`，且不施加 TCP pacing 上限。下列逐路径数字为 harness 的 TCP wire TBF 6 秒窗口中位数，顺序为 path1/path2/path3，门槛仍为 `95/190/285 Mbps`；两轮六流字节、SHA-256、回执和 EOF 完整性均通过。
- r20（发现信用不截到 256 KiB 上界，按无界发放）候选 `0ad4ff987c9a5c93a7d1e52feee4e48cd907a61f63a3a56602ca1fc273b06781`，证据 `build/private/autopacing-bidir-20261121-r20/`：聚合 upload/download 为 `469.670/506.215 Mbps`。upload 中位数约 `86.3/185.8/263.1`，download 约 `97.6/189.5/257.3`。上行极不稳定：path1 九个窗口总体从 `100.0` 滑到末窗 `25.5 Mbps`，path3 中段也从约 `295.7` 降到 `220.4 Mbps`；下行聚合接近 r16，但 path3 中位数仍低于门槛。三路径同窗通过仅 upload `2/9`、download `2/9`。无界的发现信用没有带来整体提升：上行聚合比 r16 低约 `57.9 Mbps`，下行仅高约 `7.3 Mbps`，且上行快路径在运行后段失稳。
- r21（同一发现信用截到 256 KiB）候选 `e74185dfd785e77195c55be0ebc977c9bd90f09f571505953fb7c7a469222c29`，证据 `build/private/autopacing-bidir-20261121-r21/`：完整性通过，聚合 upload/download 为 `535.710/500.115 Mbps`。upload 中位数约 `94.3/194.2/286.9`，download 约 `66.3/189.7/281.1`。上行各窗口稳定得多（path1 `92.4–99.9`、path2 `185.2–200.0`、path3 `275.0–300.0 Mbps`），三路径同窗通过 `4/9`；path1 中位数仍差门槛约 `0.7 Mbps`，未形成持续通过。下行 path1 反而恶化：九个窗口仅 `60.4–78.9 Mbps`，中位数 `66.3`，低于 r16 的 `77.9`，下行三路径同窗通过 `0/9`。
- 对照本轮之前的 r16 无 pacing cap 基线（`3181422a8ef0fc5d15e1df2ecc65b26e6c6f7eef99d7e245ed8d817e572d55ac`）：聚合 `527.533/498.939 Mbps`，upload 中位数 `95.3/187.1/287.0`，download `77.9/189.2/277.8`，三路径同窗通过 upload `3/9`、download `0/9`。r21 的上行聚合比 r16 高约 `8.2 Mbps`、上行同窗通过由 `3/9` 提到 `4/9` 且更稳，但下行 path1 更差，不能算整体改进，也没有达到持续 95% 三路径同窗。
- 判定：有界发现信用修正了“provisional BDP 自锁后无法再产生更大样本”的机制问题，但收益是方向不对称的；256 KiB 上界能稳定上行，却完全没有解决下行 path1 的供给。r20 的无界发放会破坏上行后段稳定性，不能保留。两轮都只有完整性与观测证据，**不得**写成三路径聚合验收通过或 600 Mbps 应用净荷达标。发现信用在 path1 下行修复前只保留为实验候选，不进入生产默认。
- 下一步阻塞在服务端主连接/下行 path1 的归属与方向相关调度，而不是继续调发现信用或 pacing 常量。应先按 path × 6 秒窗口对齐采集服务端的 primary/child 承载归属、接收派发、调度选中、发送信用与在途、ACK 供给和 socket 待发，判定下行 path1 是在连接绑定、接收派发还是信用发放层被饿死；拿到因果证据后再决定是否修改 primary 语义或服务端调度。在此之前不再扩大 discovery allowance，也不宣称验收通过。
 
## 服务端 primary 转发隔离与负载判别（2026-11-21）

- 为验证服务端 primary 的 VMUX 转发是否与 PPP 主循环争用执行器，做了窄实验：保持传输/socket strand 和主 PPP 循环不变，仅将 VMUX 转发连接改到同一 server context 的独立 strand。构建成功；实验随后精确撤回，当前工作树不保留该四文件实验改动，仅保留既有 `VirtualEthernetSwitcher.cpp` binary-framing admission 改动。
- r22（36 秒双向六流、独立转发 strand）完整性通过，聚合 upload/download 为 `529.965/490.868 Mbps`；逐路径中位数 upload `94.2/187.0/279.6`、download `72.0/187.2/276.3 Mbps`。与 r16/r21 相比没有解决下行 path1，且简单拆分 primary forwarding 不能作为生产修复。
- 为区分“六流应用负载不足”与服务端路径供给问题，r23 使用 24 个 download 流、r24 使用 36 个 download 流，均完整性通过。r23 聚合 `534.812 Mbps`，逐路径中位数 `94.2/187.8/298.9 Mbps`；r24 聚合 `533.488 Mbps`，逐路径中位数 `94.2/189.1/299.2 Mbps`。增加流数后 path1 仍约 94 Mbps、path2 约 189 Mbps、path3 约 299 Mbps，说明此前六流并非下载端负载不足；但 path1 仍略低于 95% 门槛，持续三路径同窗仍未证明。
- 因果结论：服务端 primary strand 隔离已被反证为无效方向；高并发下载证明路径可被充分填充，但 path1 的稳定短缺仍需按 path × 6 秒窗口对齐服务端连接归属、接收派发、调度选中、信用/在途、ACK 和 socket 待发。继续调 discovery credit、pacing 或简单执行器拓扑前不得宣称验收通过。
 
## TBF burst A/B 与 TCP 队列诊断（2026-11-22）

 - 测试 harness 新增 `--tbf-burst-kb`，可选 128/256/512/1024，默认保持每路径 128 KiB；共享 router/server TBF 使用 2 倍 burst。解析值写入 `topology.json`、`run-evidence.json` 和成功的 `six-flow-integrity.json`。Python AST、CLI help 和参数化结果均验证通过。
 - 单路径 path1 下行、固定一条承载、36 流、24 秒、`--wire-interval 1`：默认 128 KiB 的 `r35` 与 512 KiB 的 `r37` 均通过逐流完整性，但每秒 path1 线速仍出现相同形态的短降（约 100 Mbps 基线中出现约 64–70 Mbps 窗口）。512 KiB burst 没有消除该现象。
 - 六流对照 `r38` 也通过完整性，但 path1 仍出现约 78、87、65 Mbps 的窗口；因此不是六流并发不足。TBF counters 无 drop，router TBF backlog 约 278–290 KiB；服务端承载 socket 可见 `notsent` 约 196–203 KiB，低 delivery-rate 样本与该队列状态同时出现。
 - 结论：增大 harness burst 不能作为生产修复，也不能把 TBF burst 当作根因；当前证据仍指向单路径 TCP/VMUX 写完成与队列供给的周期性停顿。生产代码未因本 A/B 改动而调整，三路径 95% 持续门槛仍未通过。
 - 下一步：保留 qdisc/socket 诊断输出，按同一时间戳把 `notsent`、`snd_wnd`、VMUX 写完成等待和 path payload 对齐；不继续盲调 burst、pacing 或 fairness 常量。
 
 - 回归验证：`ninja -C build/test -j16 multipath_scheduler_test multipath_transmission_test multipath_path_dialer_test path_pacing_policy_test` 及四个可执行文件均通过（38/23/1/13 个用例，均 `No errors detected`）；`ninja -C build/multipath-binary -j16 ppp` 无需重建。
 - 三路径持续窗口 `r39`（`independent-90-125-180-stable`、固定池、每路径一条承载、双向六流、36 秒、`--concurrent 4`）完整性通过，但逐 6 秒 wire 窗口未通过 95% gate。归一化 path1/path2/path3：upload 为 `100/200/300`、`91.9/181.5/270.8`、`100/200/300`、`90.2/183.0/260.3`、`94.2/189.4/300` Mbps；download 为 `76.3/110.1/72.6`、`63.3/158.3/224.2`、`71.1/170.1/286.6`、`70.2/148.5/238.5`、`67.7/163.5/280.1` Mbps。下行 path1/path2 持续低于门槛，不能宣称三路径聚合完成。
 
## RTT-aware flight credit 与双端 pacing 复测（2026-11-22）

- 在 r39 基线之后，先做 client-only pacing 对照 `r47`：六流完整性通过，但逐 6 秒窗口 upload 为 `100/200/291`、`94/164/253`、`100/200/300`、`90/174/254`、`96/182/290` Mbps；download 为 `77/194/276`、`55/161/218`、`54/186/283`、`45/147/242`、`50/169/259`。客户端单边 pacing 没有解决下行 path1/path2，反而使下行后段更低。
 - 生产候选随后加入 RTT-aware flight headroom：未知或对称 RTT 保持 2x BDP 目标；实测 `srtt/min_rtt` 按 ACK 反转比例将额外 headroom 扩大，最大为 3x BDP；`adaptive_send_credit` 传入 VMUX telemetry 的 `min_rtt_ms`/`srtt_ms`。同时撤回 client-only gate，恢复两端 pacing。新增 scheduler 回归覆盖未知/对称 RTT、2.3x/2.5x/3x 扩大、3x 上限、inflight 扣除和饱和乘法。
 - `r48`（无 ACK-priority）六流完整性通过；逐 6 秒窗口 upload 为 `100/200/262`、`91/184/250`、`100/200/300`、`94/189/276`、`100/199/299` Mbps；download 为 `76/184/276`、`76/169/251`、`86/195/271`、`87/174/270`、`95/199/284` Mbps。下行 path1 后段接近门槛，但 path2/path3 和早期窗口仍不足。
 - `r49`（ACK-priority）六流完整性通过；逐 6 秒窗口 upload 为 `100/200/298`、`94/186/279`、`99/197/290`、`94/197/295`、`98/184/269` Mbps；download 为 `90/198/268`、`93/182/241`、`94/198/285`、`97/191/264`、`91/193/253` Mbps。ACK-priority 提高了下行 path1/path2 的早期供给，但 path3 后段下降；五窗口没有同时达到 `95/190/285 Mbps`。
 - 离线验证：`multipath_scheduler_test` 41 个用例、`path_pacing_policy_test` 14 个用例均 `*** No errors detected`；`ninja -C build/multipath-binary -j16 ppp` 成功，仅有既有 allocator/compiler warnings。工作树仍未提交、未推送，且包含此前多轮未提交改动；`git diff --check` 仍被既有改动中的 trailing whitespace 报告阻断，未做无关格式清理。
 - 当前结论：RTT-aware credit 是有证据支持的最小因果修复候选，显著改善 r48/r49 下行 path1，但尚未通过逐路径持续 95% gate；不得宣布三路径聚合完成、提交或推送。下一步应在相同候选上按 path×6 秒继续对齐服务端连接归属、ACK 供给、socket `notsent` 与 VMUX 写完成等待，而不是继续放大 BDP、burst 或 pacing 常量。
 
## 受控闲置份额借用复测（2026-11-22）

- 生产候选在 RTT-aware flight credit 基础上加入受控闲置份额借用：基础目标仍为 live carrier 的 equal share；只有新鲜容量、达到自身 share 且有本地信用的承载可借用其他承载未占用的 share，借用总量同时受需求和 session window 限制。未知/过期容量不借用；计划失败安全回退原 AdaptiveFlightCredit。
- 离线验证通过：`multipath_scheduler_test` 54 个用例均 `*** No errors detected`；`ninja -C build/multipath-binary -j16 ppp` 成功，仅有既有 allocator/compiler warnings。
- `r57-borrow`（`independent-90-125-180-stable`、固定池、每路径一条承载、双向六流、36 秒、`--concurrent 4`）六路逐流完整性通过。逐 6 秒窗口 upload 为 `100/200/285`、`95/181/281`、`100/195/249`、`94/170/227`、`95/200/275` Mbps；download 为 `95/194/288`、`79/161/254`、`67/140/211`、`61/149/221`、`77/163/246` Mbps。
- 借用没有通过三路径持续 `95/190/285 Mbps` gate，且下行 path1/path2 仍显著不足；因此该修复保留为最小、可回滚的实验候选，不得宣布带宽聚合完成、提交或推送。下一步仍需按 path×窗口对齐服务端归属、ACK 供给、socket `notsent` 和 VMUX 写完成等待。

## 受控探测信用与 RTT 基线修复复测（2026-11-22）

- 生产候选加入两项最小修复：容量探测 allowance 使用 scheduler 的 probe burst，而不是固定 256 KiB；首个 ACK RTT 只初始化 SRTT/RTTVAR，第二个样本后才建立 `min_rtt`，后续低值受控收敛。六流完整性、逐流字节/SHA/EOF/回执均通过。
- 离线验证通过：`multipath_scheduler_test` 62 个用例、`vmux_wave3_per_link_rtt_test` 26 个用例均 `*** No errors detected`；`ninja -C build/multipath-binary -j16 ppp` 成功，仅有既有 allocator/compiler warnings。
- `r62-probe-rtt`（`independent-90-125-180-stable`、固定池、每路径一条承载、双向六流、36 秒、`--concurrent 4`）完整性通过：upload 525.13 Mbps，download 约 460.1 Mbps。逐 6 秒窗口同时达到 `95/190/285 Mbps` 的 upload 为 1/6，download 为 0/6；逐 1 秒窗口 upload 为 16/27，download 为 2/27。
- upload 已明显恢复，但 download 仍未通过持续门槛；本轮不能宣布三路径聚合完成、提交或推送。证据目录：`build/private/autopacing-bidir-20261122-r62-probe-rtt`，候选 SHA-256 为 `14695c171811e41983e7fd010af6a01c0308b8a18188ed5c5063870c689de875`。

## 每流重传窗口与 probe/RTT A/B（2026-11-22）

- r63 将默认每流 RTX/reorder 窗口从 8 MiB 提到 16 MiB，`hold_flow` 从 r62 的 1534 次降为 0，但 download 仍只有约 461.7 Mbps，完整 6 秒窗口仍为 0/5；单纯扩大 per-flow 窗口不是充分修复，实验已撤回。
- r64 试验性限制高积压 probe owner、r65 试验性扩大 ACK RTT 反转头寸上限至 12x，均未改善且出现更强的窗口振荡，已撤回；保留原 3x RTT 上限和 probe 语义。
- r67 使用 EWMA 估计器进行 A/B，upload/download 约 408.7/359.0 Mbps，显著劣于 KCC，说明不能以切换估计器替代下行修复。
- r68 KCC 保守基线 upload/download 约 527/待重算，5 个完整 6 秒窗口均未通过三路径 gate；r69 12 MiB 折中窗口 download 约 460.6 Mbps、仅 1/5 窗口通过，已撤回。
- 当前保留的生产候选恢复为 8 MiB per-flow、64 MiB session、KCC、原 3x RTT headroom 和原 probe owner；r62/r68 证据仍显示下行缺口尚未解决。下一步应优先修复并验证测量边界、服务端 path 映射和 probe/容量反馈，而不是继续调大重传窗口或 RTT 常量。

## Probe burst 平滑 A/B（2026-11-22）

- 将默认 scheduler probe burst 从 16 MiB 降为 2 MiB（显式构造器参数和最大上限不变）。离线 `multipath_scheduler_test` 62/62 通过；r70 六流完整性通过，upload/download 约 521.9/463.7 Mbps，完整 6 秒窗口三路径同时达标分别为 2/5、2/5。
- 1 MiB 对照 r71 完整性通过，upload/download 约 533.0/481.9 Mbps，但完整 6 秒窗口同时达标为 3/5、0/5；短测存在明显窗口随机性，不能把 1 MiB 宣布为修复。当前保留 2 MiB 作为候选，仍未通过持续 gate。

- r72 仅作隔离 A/B：通过环境开关清除 Linux `SO_MAX_PACING_RATE`，upload/download 为约 533.5/416.8 Mbps，download 仍 0/5，已撤回开关。pacing 不是下行 path1/path2 的充分根因；默认 pacing 逻辑保持不变。

- r73 对成熟 pacing 目标增加 10% 安全余量后，upload/download 约 526.4/444.7 Mbps，download 仍为 0/5；该余量已撤回。最终代码已重新构建通过，默认 pacing 目标保持原策略。

- r74 将成熟 pacing 目标乘以 1.25 做对照，upload/download 约 517.8/464.6 Mbps，download 仍 0/5；该改动已撤回。最终候选继续使用原 pacing 目标，pacing margin 不是当前生产修复方向。

- r76 曾将实际 ApplyPacingRate 目标乘以 0.96 做 ACK/排队余量对照；六流双向完整性通过，但 upload/download 约 529.1/461.4 Mbps，五个完整 6 秒窗口三路径同时达标为 1/5、0/5。该改动已撤回，不能把固定 96% 余量直接作为生产修复。

- r78 将 app-limited 样本放宽为“发送队列空且链路未达容量”而不要求全局 RTX 清空；完整性通过，但 upload/download 约 526.5/449.7 Mbps，三路径同窗达标 3/5、0/5，改动已撤回，未证明该判定是根因。
- r79 仅将 session RTX 默认窗口临时从 64 MiB 改为 48 MiB；完整性通过，upload/download 约 527.3/460.3 Mbps，同窗达标 3/5、1/5，窗口扩大/缩小均不足以完成 gate，改动已撤回。
- r81 将同一候选改用 `--concurrent 8` 做服务端执行器并发对照；构建和六流完整性均通过。逐 6 秒窗口 upload 为 `100.0/183.6/289.5`、`111.5/208.3/308.0`、`83.3/164.4/249.8`、`99.9/186.5/277.1`、`95.3/200.0/292.8 Mbps`，download 为 `95.2/148.6/228.8`、`94.6/188.1/268.5`、`74.6/135.7/182.7`、`80.2/174.1/254.5`、`85.4/158.8/250.6 Mbps`；三路径同窗通过分别为 upload `2/5`、download `0/5`。增加服务端并发度未修复下行缺口，继续按 path×窗口对齐 server dispatch、ACK、socket `notsent` 与 VMUX 写完成等待，不调整 pacing/credit 常量。
## 全局 flight 预留修复与 2026-11-22 复测

- `BuildFlightCreditPlan()` 现在对 eligible carrier 预留 `max(target, inflight)`，对 draining/avoided carrier 仍预留已有 `inflight`；discovery 和 borrow 只能使用修正后的剩余 session window。该修复避免容量估计下降或承载退役时重复借用在途窗口。
- `tests/cpp/multipath_scheduler_test.cpp` 新增 overhang、ACK 释放、退役承载在途和 discovery regrant 回归；`multipath_scheduler_test` 的 54 个用例通过，相关 multipath 离线测试与 `tests/integration/multipath` Python 分析回归通过。修复没有放大 VMUX 256 KiB 高水位、每流 RTX 窗口或 pacing 常量。
- 下行诊断运行 `build/private/autopacing-down-flight-diagnostic-20261122/` 通过六流完整性。按 path×窗口对齐后，发送端 VMUX 选择仍接近 1:2:3，`queued_bytes` 通常为 0；服务端 socket 的 `pacing_rate` 中位数约为 path1 `101 Mbps`、path2 `234 Mbps`、path3 `313 Mbps`，但 delivery/TBF 窗口仍明显波动。qdisc 无 drop，socket `notsent` 与 BBR `cwnd=4` 样本同时出现，后者仍不能单独证明应用饥饿或持续 recovery。
- 同一诊断合同的 CUBIC 下行对照未消除缺口：TBF 窗口均值约 path1/path2/path3 `91.6/167.1/251.2 Mbps`；默认拥塞控制对照约 `88.9/164.2/247.4 Mbps`。因此当前不把切换拥塞控制作为生产修复。
- 标准并发 `--concurrent 4` 的 36 秒双向六流运行 `build/private/autopacing-bidir-flight-reservation-concurrent4-20261122/` 完整性通过，但持续 `95/190/285 Mbps` gate 未通过：upload 均值约 `89.3/162.4/228.6 Mbps`，download 均值约 `84.9/155.4/208.4 Mbps`，逐窗口也没有稳定三路径同时达标。
- 结论：flight 记账修复是正确且已回归的生产候选，但不是已证实的带宽根因；当前仍不能提交、推送或宣布持续三路径聚合完成。下一步应继续围绕服务端写完成、socket 在途和 BBR/ACK 反馈做因果测量，不再盲调窗口、burst、pacing 或 RTT 常量。
 - 端点分离的 `OPENPPP2_DATAPATH_PERF_JSON` 12 秒下行诊断 `build/private/autopacing-down-datapath-perf-split-20261122/` 完整性通过。服务端累计 frame encode `6.77 s / 62,488` 次、carrier send `14.76 s / 20,869` 次；客户端累计 frame decode `3.75 s / 62,473` 次、carrier receive `71.82 s / 24,992` 次。该日志是端点级聚合且包含阻塞等待，尚不能把任一阶段单独定为 path1/path2 根因；它支持继续按 path 标记写完成和 socket 在途，而不是盲调 scheduler 常量。
 - `bash scripts/run-cpp-tests.sh` 本轮在 360 秒内只完成到 CMake/Ninja `26/1024` 个编译动作后超时，不能宣称完整离线套件通过；此前已验证的窄 multipath/传输/PathDialer/pacing 测试仍通过。环境限制不改变三路径 gate 未通过的结论。
 - 12 秒下行 scheduler trace `build/private/autopacing-down-scheduler-trace-fifo-20261122/` 完整性通过。服务端按 link 的调度候选写入计数为 link1/2/3 `1817/3440/4846`，对应 `tx_post_wait` 平均约 `518/387/331 us`；客户端对应 carrier 读派发平均约 `15/19/19 us`。trace 在每次 scheduler 调用前交换写完成计数，运行末尾未保留最后一批 completion，因此 `tx_write_us` 不能用于完整延迟估计。结果支持“服务端 carrier strand 排队等待存在路径差异”，但不能证明它是持续吞吐根因，也不足以改变逻辑帧 FIFO 完成语义。
 - TCP BDP 对照：将测试显式的 `--tcp-cwnd` 从 `4194304` 提高到 `16777216`。同一候选、同一拓扑的 36 秒单向上传/下载六流均完整通过，TBF 窗口均值分别为 upload `97.4/194.3/287.9 Mbps`、download `97.5/194.7/288.3 Mbps`；每路径 28 个窗口中各有 25 个达到 `95/190/285`，其余为短暂单秒跌落。12 秒单向下载的完整窗口直接达到约 `100/200/300 Mbps`。这证明 4 MiB socket send buffer 对高 RTT 单向供给不足，不能作为三路径性能候选的默认验收窗口。
 - 同一 16 MiB TCP 窗口的 36 秒双向六流仍失败：upload 均值约 `96.6/125.5/185.9 Mbps`、download 约 `61.2/144.5/181.5 Mbps`；三路径同窗 gate 未通过。`--concurrent 8` 的 12 秒双向对照更差，不能归因于简单的 executor worker 不足。
 - 双向 scheduler trace 完整性通过，但服务端 `backpressure` 为 `1545/13377` 调度调用、客户端为 `911/11860`；发送队列最高仅 6 帧（393210 bytes），服务端/客户端同时观测到的三链路 wire inflight 总和最高约 50.3 MiB，低于 64 MiB session RTX cap；scheduler trace 的 `rx_paused_links` 快照为 0，控制队列没有持续堆积。随后启用 pool metrics 的 12 秒对照记录到 server `hold_flow=1033`、`rx.backpressure.pause=63`、`lease_expired=57`，client `hold_flow=2809`；因此不能把 trace 快照的 rx pause=0 解释为没有接收背压。证据排除了“单纯 4 MiB TCP 缓冲”与“只增加 executor 并发”，但尚未证明双向 VMUX/可靠性耦合的具体根因。
 - 当前结论：16 MiB 只向测试夹具证明高 RTT 单向 BDP 需要足够 socket send buffer，不能提交为生产修复，也不能宣布双向持续三路径聚合完成。继续调查应围绕双向时 `backpressure`、每链路 `wire_inflight`/send credit、ACK 到达时间和应用供给的同窗关联；不再盲增 RTX、pacing、burst 或 executor 常量。
 - 临时 config wrapper 将 `rtx.flow_bytes` 提到 16 MiB（session cap 保持 64 MiB），12 秒双向完整性通过但 upload 仅 `91.3/150.7/249.1 Mbps`、download `80.5/134.3/217.3 Mbps`；两端 `hold_flow` 均降为 0，server 仍有 `rx.backpressure.pause=31`、`lease_expired=20`，RTX 峰值约 63.5 MiB。该反事实确认 per-flow hold 是可观测症状而非充分根因，wrapper 和实验配置已删除，生产默认仍为 8 MiB。
 - 进一步的隔离反事实：临时禁用 `rx_pause_link()`，候选 SHA-256 `61e2b703a402fb0569b0fe832023cde039d43223404133a1d3d3ee02964f6cdc`。36 秒双向六流完整性通过，但 upload 均值为 `96.1/187.5/276.0 Mbps`、download 为 `89.7/176.6/248.0 Mbps`；三路径持续 gate 仍失败。禁用暂停确实使 `rx.backpressure.pause/lease_expired` 归零，但 `hold_flow` 仍为 server `751`、client `1294`，且下行整体未改善。该结果排除了“单独移除接收暂停即可修复”这一假设；诊断开关已撤回并重建正常候选。
 - 双向 TCP 窗口反事实继续扩大到 `--tcp-cwnd 67108864`：36 秒六流完整性通过，但 upload 均值 `96.2/185.0/282.7 Mbps`、download `86.8/180.3/248.7 Mbps`，相较 16 MiB 没有实质改善，download path3 仍仅 3/28 个窗口达到 285 Mbps。由此排除“只需把双向 socket send buffer 从 16 MiB 再放大”这一解释；64 MiB 仅作为诊断参数，未进入生产配置。
 - 综合 16 MiB/64 MiB TCP 窗口、禁用接收暂停、per-flow RTX 16 MiB 和 executor 并发 A/B：业务完整性稳定通过，但没有一组双向结果达到三路径持续 `95%` 门槛。当前证据支持“高 RTT 单向供给问题”已被单独隔离；双向缺口仍是可靠发送 credit、ACK/完成释放、重排和应用供给的耦合问题，尚不足以选择安全的生产修复。该验收项保持未完成。
