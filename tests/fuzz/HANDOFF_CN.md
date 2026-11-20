# VMUX 健壮性测试交接

## 范围

本次只给已有 VMUX 增加离线测试工具，不修改线上帧格式、生产协议代码、多路径调度或多路径测试。PR 分支为 `feat/vmux-protocol-fuzz`，目标为 `Miaocchi/openppp2:main`，从 `8a774f3e` 单独整理；不要把含有其他改动的 `multipath-kcc-default` 当作 PR 分支。源提交 `c8a4cda2` 留在原分支，合入本 PR 后不应重复摘取。

| 目标 | 覆盖范围 |
|---|---|
| `vmux_ack_frame_fuzz` | 任意字节解码、合法 ACK 编解码逐字段比对、ACK tracker 区间顺序和数量上限 |
| `vmux_fec_frame_fuzz` | 原始及变异、截断输入解析；合法帧组的成员标识、单帧恢复与原始数据逐字节比对 |
| `vmux_flow_control_fuzz` | 重排缓冲区的插入、取出和查找；检查条目与字节账目、已入队对象不会被重复取出、flow context 准入规则 |

## 已验证

在 Linux 上用 Clang 19.1.7 / Ninja 构建 Release，打开 ASan 和 UBSan。三个目标分别从一份合成种子起跑，使用 `-seed=42 -runs=10000`，全部完成且未触发 sanitizer 或断言。现有 ACK tracker、FEC codec、重排、flow 准入、RX 队列 CTest 共 5 项全部通过。临时种子和日志未纳入提交。

## 接手方式

- 按 [README](README.md) 使用 Clang 和 `ENABLE_VMUX_FUZZ=ON` 构建。默认不构建这些目标，不变更现有 CI 门槛。Release 也保留断言；UBSan 报错立即中止。
- 短跑时给每个目标各自的合成种子目录。流控制输入需要 2 字节头部，每条操作再用 10 字节，空语料的短跑可能进不了操作循环。
- 回归输入先保存在本地，再用同一构建的目标执行 `<目标路径> <输入文件>`。提交语料前确认不包含真实会话数据或凭据。
- 不把 codec 或缓冲区属性检查当成端到端验证：未覆盖 VMUX 主会话的 SYN/PUSH 分发、真实载流、多路径切换或移动端真机。本次没有启动 `ppp` 或修改宿主机网络。
