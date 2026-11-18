# 多路径实现与生产验收

依据：`docs/design/MULTIPATH_DESIGN_CN.md`。功能默认关闭，完成本表前不视为生产可用。单元测试、模拟传输和真实端到端测试分别记录，不能互相替代。

## 已有验证与缺口

以下为开发工作树的局部证据，不能视为发布候选验收。

复核口径（2026-11-18 对当前源码分支 `codex/multipath-fixes`，基线 `c32248c5` 的重编实测）：本节把每条声明按四种状态区分——**已在当前源码重编并通过**、**仅旧产物通过、新鲜度未确认**、**已知不稳定/竞态待修**、**未验证**。最后一次源码编辑（2026-11-15 06:00）此前从未被任何构建覆盖，带旧时间戳的条目描述的是更早（05:55–05:57）快照的结果，已在对应行注明。凡与当前源码不符的旧措辞均已按实测结论改写；未验证项不得由模拟结果或旧产物推断完成。

- `multipath_configuration_test`、`multipath_bypass_lease_test`、`client_underlying_socket_protector_test` 已在 Linux 离线构建并通过。
- `multipath_secure_channel_test` 已通过；`tests/crypto/multipath_reference_vectors.py` 使用独立 Python Noise/AEAD 实现核对握手、双向记录和上下文 exporter 向量。独立密码学向量在当前源码上复跑 **PASS**（已在当前源码重编并通过）。
- `multipath_transmission_test` 曾在回环 TCP、WS 和临时 CA 签发证书的 WSS 上通过安全握手、控制记录、原 PPP 握手和超过单记录长度的双向数据传输。该项属**旧快照结果**，在当前源码上已不可复现，见下方传输套件条目；此项从未经过 CDN 或完整 VPN 会话。
- `tests/integration/multipath/run.py infrastructure-check` 已独立复核 namespace、qdisc、连通性、目标 RTT 范围及清理；没有据此认定 PPP 多路径数据面通过。
- 桌面前端已有配置与状态视图；前端测试和构建通过。Rust 工具链缺失，Rust 兼容性测试仍待运行。
- 会话/控制与 wire codec 初期断言失败已修复，最新普通构建的会话、wire codec 和调度目标通过。安全通道、会话、wire codec、地址租约和调度五个目标的独立 ASan/UBSan 结果为**仅旧产物通过、新鲜度未确认**（见下方 sanitizer 条目）；协议仍在集成扩展，最终版本须重验。
- 原 Noise 握手/exporter、VMUX ACK/重传/收发背压六个相关回归目标通过（旧快照结果）。
- 地址租约并发目标的 TSan 结果为**仅旧产物通过、新鲜度未确认**；这不证明整个会话与传输生命周期没有竞态。
- 带抖动、丢包和限速的 `lossy-20-80-200` 隔离基础设施检查通过。完整生产二进制的单路径对照、多路径业务传输与故障恢复结果尚缺。
- 客户端、服务端完整接入、主连接故障连续性、全断恢复及不同质量业务流量矩阵均未验收。尤其恢复代次切换后的未确认控制操作对账，必须经过实际会话验证。
- 2026-11-15，`cmake --build build/production --target ppp -j 2` 在修正服务端认证方式枚举限定名后报告成功，产物为仓库 `bin/ppp`。**该结论仅适用于当时的旧快照**：那次构建覆盖的源码里其实还含有编译错误——`ppp/app/client/VEthernetExchanger.cpp` 的 lambda 捕获缺少 `this`。2026-11-18 修正该 `this` 捕获后，当前源码的 Linux 完整内核编译/链接**通过**（已在当前源码重编并通过）。Windows/macOS/移动端运行行为与真实 CDN 场景尚无通过证据。
- 同轮（旧快照）重编并运行配置、会话、路径管理、调度、绕行租约、wire codec 与运行快照七个目标，全部通过。wire round-trip 已补充非零 operation watermark 及其保真校验。当前源码状态见下一条。
- 2026-11-18 对当前源码重编并运行八个 multipath 离线测试加 `runtime_snapshot_test`：configuration、session、wire_protocol、path_manager、scheduler、bypass_lease、secure_channel 与 `runtime_snapshot_test` 均 rc=0（已在当前源码重编并通过）；`multipath_transmission_test` 除外，见下方不稳定条目。其中 `multipath_secure_channel_test` 此前只被旧产物覆盖，现已从最新源码重编并通过。
- 2026-11-18 对当前源码重编并运行 `transport_auth_lifecycle_test`：通过（已在当前源码重编并通过）。新增竞态用例 `transport_auth_completion_survives_late_caller_resume` 已确认进入该二进制；此前旧二进制内该符号数为 0，即旧结果并未覆盖这一用例。
- SOCKS 测试必须确认目标触发 VMUX：proxy-only 模式的普通目标地址可能走本地 Rinetd；虚拟子网目标则提前进入 lwIP/NAT。隔离业务设施正在改为 policy v2 明确 `proxy` 策略的非虚拟测试地址；FRP 或虚拟子网 NAT 业务不作为 VMUX 聚合证据。
- 同轮 VMUX 完整内核行为测试 `bin/vmux_receive_semantics_test` 通过；安全通道、VMUX 重传缓冲、可靠性协商、ACK 与收发背压六个相关离线目标通过；独立 Noise 向量复核通过。**ASan/UBSan 措辞需修正**：当时 ASan 构建目录下 secure_channel 与 transmission 仍有待编步骤，2026-11-15 06:00 的改动从未被该目录覆盖，因此“安全通道、会话、wire codec、调度和地址租约五个目标从最新源码重编后通过 ASan/UBSan”这一说法不成立；这五项目标连同 TSan 下的 bypass_lease 目前均为**仅旧产物 rc=0、新鲜度未确认**，须在当前源码上重编重跑后才能计为通过。
- 完整传输套件曾出现 WS/WSS 清理阶段 sanitizer 崩溃；修正 fixture 等待读取泵停止的清理顺序并补充安全记录读取的并发状态检查后，完整十用例 ASan/UBSan 套件在启用泄漏检测的条件下连续三轮通过（每轮 60 秒超时）；随后普通构建重编并通过全部十用例。**该结论在当前源码上复现不了**：当前源码重跑显示 `multipath_transmission_test` **不稳定**。其中 `mux_receive_queue_saturation_closes_the_carrier` 在当前源码连跑 8 次为 1 通过 / 7 挂起（timeout）/ 0 干净失败，判定为测试设施竞态叠加清理挂死，产品侧承载关闭语义本身正确。该项状态为**已知不稳定/竞态待修**：饱和用例的修复正在同一分支上进行、尚未合并，在其合入并在当前源码上重编重跑前，不得声称该套件十用例全部通过。其余九用例在当前源码上通过，但整套结果以饱和用例修复合入后的重跑为准。
- 逐路径指标已加入稳定 path ID 映射与快照，统计口径为 VMUX 帧字节（含重传），不等同应用 goodput；已补聚合测试。设计要求的有效业务收发量仍需补充可验证口径，不能以帧计数替代。完整内核 VMUX 行为目标包含新增的迟到 ACK/承载 ID 复用回归；最新异常分配处理补编、链接与行为测试已通过。
- 桌面前端本轮 `npm test` 的 42 项测试与 `npm run build` 通过；当前环境仍未找到 Cargo，Rust 后端及 Windows 运行验证未完成。
- 隔离双路径业务 smoke 已进入连接阶段，但先前虚拟对端 SOCKS CONNECT 失败，且该通路不能证明 VMUX，测试方案已纠正。尚无成功的 VMUX 业务摘要或路径参与结果；该失败不计为业务验收通过。

## 实施与证据清单

状态口径（对应上一节的复核口径，避免把旧产物结果当作当前源码结论）：**已在当前源码重编并通过** / **仅旧产物通过、新鲜度未确认** / **已知不稳定/竞态待修** / **未验证**。下表“当前证据”栏只列已核实到当前源码的离线与模拟证据；凡只被旧产物覆盖或结果不稳定的项，均不计为该项通过。

| 设计范围 | 必须证明的行为 | 当前证据 |
|---|---|---|
| 配置与兼容 | 严格类型/字段/数量校验，旧配置不变，服务端独立开关，错误不泄漏秘密 | 已在当前源码重编并通过：`multipath_configuration_test`、`transport_auth_configuration_test`。服务端独立开关的真实部署组合仍**未验证** |
| 安全承载 | TCP/WS/WSS 内层认证加密；TLS 终止后仍保护源站流量；角色、身份、能力绑定；新连接新密钥 | 已在当前源码重编并通过：`multipath_secure_channel_test`。旧快照曾在回环 TCP/WS/WSS 上通过握手与双向传输，当前源码上该套件不稳定；真实 CDN 与 TLS 终止后的源站流量仍**未验证** |
| 密码与协议 | 协议布局冻结；原 Noise v1 语义不变；记录长度/nonce/重放/密钥清理；独立审查 | 已在当前源码重编并通过：`tests/crypto/multipath_reference_vectors.py` 独立向量 PASS。独立安全审查仍未做，故仍待验收 |
| 会话加入 | 单创建意图，预留/确认/提交/查询，错误身份与候选、确认丢失、并发和迟到回调 | 已在当前源码重编并通过：`multipath_session_test`（含 watermark 保真断言）。真实会话中的恢复代次对账仍**未验证** |
| 所有权与并发 | listener/context 间安全交接，不迁移活动 TLS/WS，队列有限并支持取消 | **仅旧产物通过、新鲜度未确认**：ASan/UBSan 四目标与 TSan `bypass_lease` 当时为 rc=0，但 2026-11-15 06:00 的改动从未被这些构建覆盖，须在当前源码重编重跑 |
| 路径生命周期 | 部分可用启动、限制并发建连、退避抖动、黑洞探测、补连和先建后退 | 已在当前源码重编并通过：`multipath_path_manager_test`。隔离 netns 故障注入与真实入口下的补连仍**未验证** |
| DNS 与绕行 | IPv4/IPv6 候选拨号前保护，引用计数，更新与取消释放，多平台行为明确 | 已在当前源码重编并通过：`multipath_bypass_lease_test`、`client_underlying_socket_protector_test`。多平台行为仍**未验证** |
| 调度与背压 | adaptive/failover 双向调度，单流并行，异构 RTT 下在途与重排预算有界 | 已在当前源码重编并通过：`multipath_scheduler_test`。异构 RTT 下的实测在途/重排上界仍**未验证**（隔离 netns 矩阵未跑） |
| TCP 连续性 | 双向字节完整且无重复，SYN/FIN/RST、半关闭、慢目标与受限重传 | 已在当前源码重编并通过：`transport_auth_lifecycle_test`（含新增 `transport_auth_completion_survives_late_caller_resume` 竞态用例）、`vmux_receive_semantics_test`。已知不稳定/竞态待修：传输套件的 `mux_receive_queue_saturation_closes_the_carrier`（连跑 8 次为 1 通过 / 7 挂起 / 0 干净失败，属测试设施竞态与清理挂死，修复进行中）；慢目标与受限重传的实测仍**未验证** |
| 控制与恢复 | 主物理连接失效仍可用；控制序号/ACK/幂等；全断恢复和固定宽限期 | 已在当前源码重编并通过：`multipath_wire_protocol_test`（含非零 operation watermark）、完整内核 VMUX 行为目标的迟到 ACK/承载 ID 复用回归。主连接失效连续性与全断恢复仍**未验证** |
| 过期与重启 | 失败候选不续期；进程重启后旧流明确失败；无旧数据注入新会话 | 待验收 |
| 非 TCP 范围 | UDP 选健康承载与允许丢包；原始 IP/P2P/FRP 的适用范围逐项说明 | 待验收 |
| 运维与产品 | 配置/UI 接入，路径与会话状态/指标，秘密不落日志，受控启停与发布回退文档 | 已在当前源码重编并通过：`runtime_snapshot_test` 的多路径快照字段；桌面前端 `npm test` 42 项与 `npm run build`。Cargo 缺失使 Rust 后端未测；启停回退文档与秘密审计仍**未验证** |
| 平台与质量 | 全内核编译、相关回归、ASan/UBSan/TSan、跨平台构建和资源回落 | 已在当前源码重编并通过：Linux 完整内核编译/链接（修正 `VEthernetExchanger.cpp` lambda `this` 捕获后）、八个 multipath 离线测试加 `runtime_snapshot_test`（`multipath_transmission_test` 除外）、`transport_auth_lifecycle_test`、独立密码学向量。已知不稳定/竞态待修：`mux_receive_queue_saturation_closes_the_carrier`，修复在同分支进行中、待合并。仅旧产物通过、新鲜度未确认：ASan/UBSan 四目标与 TSan `bypass_lease`，须在当前源码重编重跑。未验证：Windows/macOS/移动端构建与运行 |
| 真实入口 | 直连与反代/CDN 场景业务数据验证；本地 TLS 终止模拟须单独标识 | 待验收 |

## 隔离网络矩阵

每个场景同时测试单流、多流、上传、下载和双向传输，以同环境最佳单路径为对照。记录配置、随机种子、构建 SHA、逐字节摘要、吞吐、延迟分位数、恢复耗时、峰值内存、重排占用及额外重传；失败或缺测不能计为通过。

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

仅在任务专用网络命名空间或回环测试端口中故障注入。不触碰宿主路由、DNS、系统代理或活动安装。未验证的真实 CDN、平台及运行行为必须保留在最终报告中，不能由模拟结果推断完成。
