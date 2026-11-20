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
