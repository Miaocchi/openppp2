# 多路径工作交接（下一阶段开发）

日期：2026-11-19。实现已合入正式分支，下一阶段以集成验收和缺口收敛为主，不再按从零开发排期。

## 目标

同一客户端通过 TCP 直连、WS/WSS 反代及不同 CDN 入口，并行连接同一服务端进程的同一逻辑会话。首版重点是 VMUX TCP 流聚合、单路径故障连续性与全断后的有界恢复。普通 HTTP CDN 仅使用其支持的 WS/WSS。

不包含多网卡聚合、跨服务端进程共享会话或服务端重启后保留原 TCP 流。UDP、原始 IP、P2P、FRP 的覆盖范围须分别说明。

## 当前状态

- 本交付镜像（分支 `codex/multipath-production`）HEAD 为 `ef966bd4`（feat(multipath): publish session path control events），工作区干净。正式工作树 `/root/.codex/worktrees/multipath-production/openppp2` 的基线为 `10c76d75`（含 13 个提交的历史拆分）；镜像在克隆自该基线后另加三个提交：`baf8255` 统一 Path Control 集成、`d384307` 默认 adapter 能力门控、`ef966bd4` 发布 session path control 事件。此前工作树中的未提交改动已在镜像中提交化，`ppp/`、`tests/`、`docs/`、`ppp.vcxproj` 与正式工作树当前工作状态逐字节一致。
- `multipath_transmission_test` 的 mux 饱和竞态与清理挂死由 `10c76d75` 修复（提交记录：该用例 15/15、完整套件连续两轮）。2026-11-19 在正式工作树以当前源码重建后独立复核：单用例 40/40、完整二进制 5/5、并发 8 实例 0 失败、`ctest -R '^multipath_'` 9/9 通过、ASan 版单用例 10/10。早于 2026-11-18 22:19 构建的测试二进制不含该修复，复跑前必须重建。
- `builds/openppp2-linux-amd64-*` 为各 Linux 变体（sysnat/io-uring/simd 组合）的 CMake 构建定义文件，与主线仓库逐字节一致，CI 变体矩阵使用相同后缀命名；它们不是可执行产物，本镜像不含 Linux `ppp` 二进制，验收方不应将其当作构建输出核对。
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
