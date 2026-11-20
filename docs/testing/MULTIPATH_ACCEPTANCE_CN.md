# 多路径实现与生产验收
> **范围：** 本页是默认关闭的多路径功能验收记录，不是生产可用声明；设计背景见 [多路径设计](../design/MULTIPATH_DESIGN_CN.md)。
> **证据：** 下文的离线测试、模拟承载、隔离网络和提交记录分别标为当前源码证据、旧产物证据或局部观察；它们不互相替代，也不自动证明真实业务通过。
> **验收标准：** 将“隔离网络矩阵”和 `production-fault-matrix` 视为业务门禁；“实施与证据清单”逐项给出当前证据和仍需验收的条件。
> **未验证缺口：** 真实 CDN/VPN 入口、完整故障恢复、跨平台行为、异构 RTT 上界、性能收益和安全审查等缺口必须保留在最终报告中，不能由本地模拟推断完成。
> **路径：** 测试集合与构建边界见 [开发测试](../development/TESTING_CN.md)；发布、部署和故障处理见 [多路径发布](../operations/MULTIPATH_ROLLOUT_CN.md) 与 [运维索引](../operations/README_CN.md)。


依据：`docs/design/MULTIPATH_DESIGN_CN.md`。功能默认关闭，完成本表前不视为生产可用。单元测试、模拟传输和真实端到端测试分别记录，不能互相替代。

## VMUX 交付速率估计器

进程启动前设置 `OPENPPP2_VMUX_PATH_ESTIMATOR=ewma|bbr|kcc` 可选择算法，未设置时默认使用 `kcc`；显式未知值仍回落到 EWMA。设置仅影响本地 VMUX 估计，旧配置和协议使用同一协商入口。Adaptive 排程与运行快照共用逐承载估计结果，在 ACK 采样有效时按估计容量排程。运行快照分别报告最近的交付样本速率和估计容量。

三个算法共用 VMUX payload ACK 样本。采样窗口取发送与 ACK 经过时间的较大值，重传条目的 ACK 仍释放缓冲和参与既有字节记账。速率样本只使用未重传条目。发送队列和本地写入预算用于推断应用受限。BBR 与 KCC 模式取最近十个交付轮次的带宽峰值。KCC 模式增加连续三个非应用受限轮次增长低于 25% 的满带宽检测。

容量样本超过 5 秒失效。偏低的应用受限样本仍更新最近交付速率，但不会刷新容量，也不会在容量失效后给调度提供替代速率。KCC 模式的满带宽判定目前供估计器测试读取，Adaptive 调度仍使用容量。排队延迟来自承载 RTT 遥测，KCC 容量计算尚未使用该信号。FEC 恢复的交付来源无法从现有 ACK 格式识别。

## VMUX 池级收益试验

客户端在同一 VMUX、同一发送方向统计首次被对端 VMUX ACK 确认的 PUSH 净载荷。跨链重传和已脱离承载的条目也只计一次；该指标不代表目标应用已读取。未协商 reliability 的传统 turbo 使用队列策略，不报告实测增益。

有持续积压且有空余槽位时，一次只拨一条。以加入前窗口为基线，排除拨号和握手后的预热窗口，再比较全池 ACK 速率、RTT 排队时延和重传成本。收益至少提高 5% 且至少提高 1024 B/s，时延和重传成本不过线才保留；已测无益时只回收试验链并冷却 60 秒。换链、需求不可比、无 ACK 或超时算不可判定，不作为零收益。空闲缩链不低于已协商的下限。

`client.multipath.enabled`、`policy=adaptive` 与 `mux.turbo` 同时开启时，请求可选能力位 `0x10`。仅在认证多路径、flow-v2 和 reliability 均协商成功且对端回显该位时，第一条即可建立；原 wire count 成为硬上限。正常补连以已保留的目标数为准。failover 和未回显的新旧端组合仍预建原池，不把探测当作容灾冗余。

离线验收：`pool_controller_test` 验证比较窗口、正负收益、未知、超时和下限；`vmux_retransmit_buffer_test` 核对会话净载荷记账；`vmux_net_churn_integration_test` 用本地模拟承载核对启动门槛、保留/回滚和迟到拨号令牌。共享瓶颈与独立瓶颈真实吞吐比较仍需使用接收应用实收结果。

本轮 Linux 离线 CTest：上述三个目标，加 `vmux_path_estimator_test`、`vmux_wave4_pto_rtx_test`、`vmux_negotiation_test`、`multipath_path_manager_test` 和 `vmux_receive_semantics_test`，共 8/8 通过。路径槽位测试区分主动回收与故障退避。未启动 PPP，未执行真实网络或双向吞吐测试。

## 已有验证与缺口

以下为正式分支的局部证据，不能替代真实 CDN/VPN 业务验收。

复核口径（2026-11-19，正式分支 `codex/multipath-production`，基线提交 `10c76d75`，当前工作区含未提交的统一 Path Control 实现）：本节把每条声明按四种状态区分——**已在当前源码重编并通过**、**仅旧产物通过、新鲜度未确认**、**已知不稳定/竞态待修**、**未验证**。提交 `10c76d75` 已合入 mux 饱和用例的确定性修复；提交说明记录该用例 15/15 通过、完整套件连续两轮通过。未具备监听 socket 权限的环境不能把传输套件现场启动失败计为产品回归；真实 CDN/VPN 业务仍须单独验收。

- `multipath_configuration_test`、`multipath_bypass_lease_test`、`client_underlying_socket_protector_test` 已在 Linux 离线构建并通过。
- `multipath_secure_channel_test` 已通过；`tests/crypto/multipath_reference_vectors.py` 使用独立 Python Noise/AEAD 实现核对握手、双向记录和上下文 exporter 向量。独立密码学向量在当前源码上复跑 **PASS**（已在当前源码重编并通过）。
- `multipath_transmission_test` 在回环 TCP、WS 和临时 CA 签发证书的 WSS 上覆盖安全握手、控制记录、原 PPP 握手和超过单记录长度的双向数据传输；其中 mux 饱和用例的竞态与清理挂死已由 `10c76d75` 修复。提交验证记录为该用例 15/15 通过、完整套件连续两轮通过；此项从未经过 CDN 或完整 VPN 会话。
- `tests/integration/multipath/run.py infrastructure-check` 已独立复核 namespace、qdisc、连通性、目标 RTT 范围及清理；没有据此认定 PPP 多路径数据面通过。
- 桌面前端已有配置与状态视图；前端测试和构建通过。Rust 工具链缺失，Rust 兼容性测试仍待运行。
- 会话/控制与 wire codec 初期断言失败已修复，最新普通构建的会话、wire codec 和调度目标通过。统一 Path Control、VMUX 估计器、pool controller、路径管理、会话、调度、运行快照和 VMUX 重传相关八个目标已在当前源码上完成独立 ASan/UBSan 重编并通过，协议集成和真实业务仍需继续验收。
- 原 Noise 握手/exporter、VMUX ACK/重传/收发背压六个相关回归目标通过（旧快照结果）。
- 当前源码的 `multipath_session_test`、`multipath_scheduler_test` 和 `multipath_path_control_test` 已在独立 `/tmp/openppp2-tsan` 构建中通过 TSan。地址租约并发目标的 TSan 仍未重编验证，这不证明整个会话与传输生命周期没有竞态。
- 带抖动、丢包和限速的 `lossy-20-80-200` 隔离基础设施检查通过。完整生产二进制的单路径对照、多路径业务传输与故障恢复结果尚缺。
- 客户端、服务端完整接入、主连接故障连续性、全断恢复及不同质量业务流量矩阵均未验收。尤其恢复代次切换后的未确认控制操作对账，必须经过实际会话验证。
- 2026-11-15，`cmake --build build/production --target ppp -j 2` 在修正服务端认证方式枚举限定名后报告成功，产物为仓库 `bin/ppp`。**该结论仅适用于当时的旧快照**：那次构建覆盖的源码里其实还含有编译错误——`ppp/app/client/VEthernetExchanger.cpp` 的 lambda 捕获缺少 `this`。2026-11-18 修正该 `this` 捕获后，当前源码的 Linux 完整内核编译/链接**通过**（已在当前源码重编并通过）。Windows/macOS/移动端运行行为与真实 CDN 场景尚无通过证据。
- 同轮（旧快照）重编并运行配置、会话、路径管理、调度、绕行租约、wire codec 与运行快照七个目标，全部通过。wire round-trip 已补充非零 operation watermark 及其保真校验。当前源码状态见下一条。
- 2026-11-18 对当前源码重编并运行八个 multipath 离线测试加 `runtime_snapshot_test`：configuration、session、wire_protocol、path_manager、scheduler、bypass_lease、secure_channel 与 `runtime_snapshot_test` 均 rc=0（已在当前源码重编并通过）。`multipath_transmission_test` 的饱和用例随后在 `10c76d75` 中修复，并以提交验证记录补足 15/15 与完整套件两轮结果；`multipath_secure_channel_test` 此前只被旧产物覆盖，现已从最新源码重编并通过。
- 当前工作区新增的统一 Path Control 离线覆盖包括 `multipath_path_control_test` 13 cases、scheduler projection 15 cases、PathManager snapshot facade、VMUX estimator 4 cases、pool controller 和 VMUX attribution 14 cases。共享 build 目录为只读，因此使用独立 `/tmp/openppp2-tests` CMake 构建，八个受影响目标均由 CTest 通过，同一组目标又在独立 `/tmp/openppp2-asan` ASan/UBSan 构建中 8/8 通过，LeakSanitizer 因 ptrace 限制关闭。
- 2026-11-18 对当前源码重编并运行 `transport_auth_lifecycle_test`：通过（已在当前源码重编并通过）。新增竞态用例 `transport_auth_completion_survives_late_caller_resume` 已确认进入该二进制；此前旧二进制内该符号数为 0，即旧结果并未覆盖这一用例。
- SOCKS 测试必须确认目标触发 VMUX：proxy-only 模式的普通目标地址可能走本地 Rinetd；虚拟子网目标则提前进入 lwIP/NAT。隔离业务设施正在改为 policy v2 明确 `proxy` 策略的非虚拟测试地址；FRP 或虚拟子网 NAT 业务不作为 VMUX 聚合证据。
- 同轮 VMUX 完整内核行为测试 `bin/vmux_receive_semantics_test` 通过；安全通道、VMUX 重传缓冲、可靠性协商、ACK 与收发背压六个相关离线目标通过；独立 Noise 向量复核通过。当前源码八个受影响多路径目标的 ASan/UBSan 结果已补录，使用 `ASAN_OPTIONS=detect_leaks=0`，LeakSanitizer 受本机 ptrace 限制，TSan 下的 bypass_lease 仍未重编验证。
- 完整传输套件此前出现 WS/WSS 清理阶段 sanitizer 崩溃和 mux 饱和用例挂死；相关 fixture 清理顺序、并发状态检查以及饱和驱动方式已在 `10c76d75` 合入。该提交的验证记录为 15/15 饱和用例通过、完整套件连续两轮通过。该结果仍只覆盖本地回环/TCP/WS/WSS，不等同于真实 CDN/VPN 业务验收。
- 逐路径指标已加入稳定 path ID 映射与快照，统计口径为 VMUX 帧字节（含重传），不等同应用 goodput；已补聚合测试。设计要求的有效业务收发量仍需补充可验证口径，不能以帧计数替代。完整内核 VMUX 行为目标包含新增的迟到 ACK/承载 ID 复用回归；最新异常分配处理补编、链接与行为测试已通过。
- 桌面前端本轮 `npm test` 的 42 项测试与 `npm run build` 通过；当前环境仍未找到 Cargo，Rust 后端及 Windows 运行验证未完成。
- 当前源码在独立 `/tmp/openppp2-asan` 构建目录以 ASan/UBSan 重编并运行八个受影响目标，8/8 通过。LeakSanitizer 在本机 ptrace 限制下无法启用，命令使用 `ASAN_OPTIONS=detect_leaks=0`，因此该结果不包含 leak 检测。
- 当前源码的 `multipath_transmission_test` 已完成重编，但本机运行时创建本地监听 socket 被权限策略拒绝（`Operation not permitted`），9 个依赖监听器的用例未执行。该结果计为环境限制下的未验证，不能替代 `10c76d75` 提交级回环传输记录。
- Linux 隔离网络已用真实客户端、服务端验证认证首链、单链及 adaptive 的双向业务：每次 8 MiB，接收方核对字节、SHA-256 和 EOF，发送方在半关闭后收到回执。该检查不证明额外路径承载了有效业务，也不证明吞吐提升。
- 独立限速和共享限速下均观测到负载期间活动 carrier 从 1 增至 2，第二路径有实际字节。早期六流上传的退役阶段出现接收端 `gap_timeout`；不得只凭连接数回落认定无损缩容，或将未收到回执的流计入有效吞吐。
- 加入退役 ACK 等待后的六流实测（Linux 隔离网络，二进制 SHA-256 前缀 `36af3bbc4452`）：两种拓扑上传均出现 `1 → 2 → 1`。独立限速上传 4/6 条、共享限速上传 1/6 条完成回执；下载阶段均为单 carrier，最终回执均为 0/6。已验证自动增减，未通过并发完整性验收。缺回执不等于没有传送字节，也不能据此断言是退役导致了下载失败。
- 随后在当前源码重编二进制（SHA-256 `c939bc504333818b6c9117debfa8b5ab7673129b7b9555b71663a7dc5b3a9035`）运行新增的 `six-flow-integrity`：独立瓶颈和共享瓶颈两种 12 ms/24 Mbit 配置均完成 6/6 上传与 6/6 下载；每流正字节数、SHA-256、发送方回执和接收方确认均通过。该结果覆盖并发完整性与回执，不代表吞吐优于单路径，也不覆盖 CDN/TLS 终止或异构 RTT。
- 使用 `OPENPPP2_VMUX_PATH_ESTIMATOR=kcc` 对同一二进制（SHA-256 `c939bc504333818b6c9117debfa8b5ab7673129b7b9555b71663a7dc5b3a9035`）重跑两组隔离 netns 验收：`independent-12ms-24mbit` 与 `shared-12ms-24mbit` 均 PASS，均为 6/6 上传、6/6 下载，逐流 SHA-256、字节数和双向回执全部通过。固定 25 s 上传/12 s 下载窗口的观测总字节分别为独立瓶颈上传 `51,544,064`、下载 `16,613,376`，共享瓶颈上传 `35,291,136`、下载 `16,777,216`；这些是单次完整性场景观测，不是生产吞吐承诺或最优性结论。
- 为选择默认估计器，在同一 fresh 二进制和同一隔离验收 harness 下各重复 3 次 EWMA/KCC（两种 12 ms/24 Mbit 配置，全部 12 次均 PASS）。独立瓶颈中 KCC 的 25 s 上传平均 `52,811,093` vs EWMA `52,101,120`（`+1.36%`），下载平均 `16,777,216` vs `16,678,912`（`+0.59%`）；共享瓶颈中 KCC 上传平均 `35,334,827` vs EWMA `35,859,115`（`-1.46%`），下载平均 `16,678,912` vs `16,711,680`（`-0.20%`）。用户已选择将默认切换为 KCC；EWMA 仍可显式选择。该单一 12 ms/24 Mbit 矩阵不足以证明所有生产网络上的最优性。

## 实施与证据清单

状态口径（对应上一节的复核口径，避免把旧产物结果当作当前源码结论）：**已在当前源码重编并通过** / **仅旧产物通过、新鲜度未确认** / **已知不稳定/竞态待修** / **未验证**。下表“当前证据”栏只列已核实到当前源码的离线与模拟证据；凡只被旧产物覆盖或结果不稳定的项，均不计为该项通过。

| 设计范围 | 必须证明的行为 | 当前证据 |
|---|---|---|
| 配置与兼容 | 严格类型/字段/数量校验，旧配置不变，服务端独立开关，错误不泄漏秘密 | 已在当前源码重编并通过：`multipath_configuration_test`、`transport_auth_configuration_test`。服务端独立开关的真实部署组合仍**未验证** |
| 安全承载 | TCP/WS/WSS 内层认证加密；TLS 终止后仍保护源站流量；角色、身份、能力绑定；新连接新密钥 | 已在当前源码重编并通过：`multipath_secure_channel_test`；传输套件的竞态修复已合入 `10c76d75`，提交验证记录为 15/15 饱和用例和完整套件两轮通过。真实 CDN 与 TLS 终止后的源站流量仍**未验证** |
| 密码与协议 | 协议布局冻结；原 Noise v1 语义不变；记录长度/nonce/重放/密钥清理；独立审查 | 已在当前源码重编并通过：`tests/crypto/multipath_reference_vectors.py` 独立向量 PASS。独立安全审查仍未做，故仍待验收 |
| 会话加入 | 单创建意图，预留/确认/提交/查询，错误身份与候选、确认丢失、并发和迟到回调 | 已在当前源码重编并通过：`multipath_session_test`（含 watermark 保真断言）。真实会话中的恢复代次对账仍**未验证** |
| 所有权与并发 | listener/context 间安全交接，不迁移活动 TLS/WS，队列有限并支持取消 | 当前源码的八个多路径目标已通过 ASan/UBSan 重编运行，`PathControlOwner` 队列上限、关键事件保留和停止路径有离线覆盖。TSan `bypass_lease`、跨 executor 实际交接和真实 TLS/WS 生命周期仍未验证 |
| 路径生命周期 | 部分可用启动、限制并发建连、退避抖动、黑洞探测、补连和先建后退 | 已在当前源码重编并通过：`multipath_path_manager_test`。隔离 netns 故障注入与真实入口下的补连仍**未验证** |
| DNS 与绕行 | IPv4/IPv6 候选拨号前保护，引用计数，更新与取消释放，多平台行为明确 | 已在当前源码重编并通过：`multipath_bypass_lease_test`、`client_underlying_socket_protector_test`。多平台行为仍**未验证** |
| 调度与背压 | adaptive/failover 双向调度，单流并行，异构 RTT 下在途与重排预算有界 | 已在当前源码重编并通过：`multipath_scheduler_test`；隔离 netns 的 `six-flow-integrity` 在独立/共享 12 ms、24 Mbit 配置均 6/6 双向通过。异构 RTT 下的实测在途/重排上界仍**未验证** |
| TCP 连续性 | 双向字节完整且无重复，SYN/FIN/RST、半关闭、慢目标与受限重传 | 已在当前源码重编并通过：`transport_auth_lifecycle_test`（含新增 `transport_auth_completion_survives_late_caller_resume` 竞态用例）、`vmux_receive_semantics_test`；`multipath_transmission_test` 的 mux 饱和竞态修复已合入 `10c76d75`，提交验证记录为 15/15 通过；新增六流验收在两种瓶颈配置均核对 6/6 上传和 6/6 下载回执。慢目标与受限重传的更长时真实业务实测仍**未验证** |
| 控制与恢复 | 主物理连接失效仍可用；控制序号/ACK/幂等；全断恢复和固定宽限期 | 已在当前源码重编并通过：`multipath_wire_protocol_test`（含非零 operation watermark；codec 证明范围为 v1 自洽 round-trip 与严格拒绝，不含跨版本字节级互解，不主张向后兼容）、完整内核 VMUX 行为目标的迟到 ACK/承载 ID 复用回归。主连接失效连续性与全断恢复仍**未验证** |
| 过期与重启 | 失败候选不续期；进程重启后旧流明确失败；无旧数据注入新会话 | 待验收 |
| 非 TCP 范围 | UDP 选健康承载与允许丢包；原始 IP/P2P/FRP 的适用范围逐项说明 | 待验收 |
| 运维与产品 | 配置/UI 接入，路径与会话状态/指标，秘密不落日志，受控启停与发布回退文档 | 已在当前源码重编并通过：`runtime_snapshot_test` 的多路径快照字段；桌面前端 `npm test` 42 项与 `npm run build`。Cargo 缺失使 Rust 后端未测；启停回退文档与秘密审计仍**未验证** |
| 平台与质量 | 全内核编译、相关回归、ASan/UBSan/TSan、跨平台构建和资源回落 | 已在当前源码重编并通过：Linux 完整内核编译/链接、八个 multipath 离线测试加 `runtime_snapshot_test`（传输套件另有 `10c76d75` 的 15/15 与全套件两轮提交验证）、八个受影响目标的 ASan/UBSan 构建与运行、2026-11-19 九个 `multipath_*` 目标 TSan 重建复跑：8 通过；`multipath_transmission_test` 断言全过但报告 6 处 data race（同一根因：Beast websocket stream 的完成回调在 io_context 普通 executor 上执行、操作发起在 strand/测试线程，违反 stream 须同 strand 的使用契约；产品 `ppp/net/asio/websocket` 的线程契约待澄清修复）、`transport_auth_lifecycle_test`、独立密码学向量。ASan/UBSan 本轮使用 `ASAN_OPTIONS=detect_leaks=0`，LeakSanitizer 受 ptrace 环境限制；全树 ASan 并发构建会 OOM 杀 cc1plus，非 multipath 目标需 `-j2` 重试。未验证：Windows/macOS/移动端构建与运行 |
| 真实入口 | 直连与反代/CDN 场景业务数据验证；本地 TLS 终止模拟须单独标识 | 待验收 |

## 隔离网络矩阵

每个场景同时测试单流、多流、上传、下载和双向传输，以同环境最佳单路径为对照。记录配置、随机种子、构建 SHA、逐字节摘要、吞吐、延迟分位数、恢复耗时、峰值内存、重排占用及额外重传；失败或缺测不能计为通过。

TCP 半关闭验收：发送方写完后执行 `shutdown(SHUT_WR)`；接收方必须收到约定长度、核对摘要并读到 EOF，然后沿同一连接返回回执。只有发送方收到回执才记为该方向成功。短传吞吐按接收方有效负载和接收耗时计算；不能用 carrier 字节数替代。

自动扩缩容验收单独使用持续负载，记录空闲、上传、上传后空闲、下载和下载后空闲各阶段的活动 carrier 数，以及每条路径的字节增量。全程只有一条 carrier 说明没有观测到扩容，也没有验证缩容；单凭吞吐或测试进程正常退出不能判为通过。

| 场景 | 路径条件 | 故障/约束 |
|---|---|---|
| 同质基线 | 相同 RTT 与速率 | 单路径与多路径对照 |
| 异构延时 | RTT 5 / 80 / 200 ms | 慢路径不造成无界排队 |
| 异构质量 | 低延时快路径 + 高延时限速/抖动/丢包路径 | 端到端正确性与尾延迟 |
| 共享瓶颈 | 多入口共用一个限速段 | 不把连接数增加当作带宽叠加 |
| 部分失败 | 一入口拒连/握手失败 | 其余入口建立业务 |
| 承载故障 | 显式断开、单向黑洞、空闲关闭 | 业务连续、重传有界、后台补连 |
| 主连接故障 | 原创建承载退出 | 其他承载继续现有 TCP 流 |
| 全断恢复 | 全入口断开后恢复 | 宽限内恢复，超时失败，失败候选不延长期限 |
| 事务竞争 | 丢确认、重复加入、最后连接退出与加入并发 | 一致提交及代次隔离 |
| 压力与清理 | 慢目标、长连接、反复建连/取消 | 内存有界、无泄漏、资源回落 |

### 真实故障矩阵的通过口径

新增 `production-fault-matrix` 作为真实 candidate 会话门禁。每个 case 都
必须重新启动 client/server，但故障期间的业务是同一条不自动重连的 SOCKS
TCP 流：

| case | 注入 | 通过条件 |
|---|---|---|
| `single-path-outage` | 黑洞最后一条路径 | 原 TCP 流完成；未故障路径在故障后有业务字节 |
| `primary-path-outage` | 黑洞 path1 | 原 TCP 流完成；剩余路径接管业务 |
| `all-short-outage` | 全路径黑洞，时长小于 `resume_grace_ms` | 原 TCP 流恢复并完成 |
| `all-grace-expired` | 全路径黑洞，时长超过 `resume_grace_ms` | 原 TCP 流明确失败；不能把新连接成功当作原流恢复 |

成功恢复 case 必须同时记录并校验发送/接收字节数、双方 SHA-256、半关闭
后的 EOF、无 workload 重连，以及每条配置路径的
`pre_fault_bytes`、`during_fault_bytes`、`post_fault_bytes`、`total_bytes`。
逐路径计数来自隔离网络中的 carrier TBF，是路径实际承载业务的证据，不能
替代应用 goodput，也不能用 established socket、日志或连接数量代替。若任一
配置路径没有达到 `--minimum-path-bytes`，或单路径故障后的存活路径没有
达到 post-fault 门槛，case 失败。

`fault-plan`/`impair` 只负责 qdisc 故障注入；单独运行它们不构成故障恢复
验收。每次运行保留 candidate SHA、拓扑、每个 case 的 JSON、workload 结果、
路径计数快照和两端日志。未执行真实 candidate 矩阵前，不得把下表状态改为
“已验证”。

仅在任务专用网络命名空间或回环测试端口中故障注入。不触碰宿主路由、DNS、系统代理或活动安装。未验证的真实 CDN、平台及运行行为必须保留在最终报告中，不能由模拟结果推断完成。
