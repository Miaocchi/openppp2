# 多路复用

`mux` 只控制单个隧道会话内的逻辑流调度与缓冲。它不配置多路径入口；多路径请看客户端的 [`client.multipath`](client_CN.md#multipath) 和服务端的 [`server.multipath`](server_CN.md#multipath)。单路径的连接数由客户端参数 [`--tun-mux`](../CLI_REFERENCE_CN.md) 设置，默认为 `0`（关闭）；启用多路径后，客户端根据路径槽位数和 `client.multipath.max_connections` 确定 carrier 数量。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## 结构

```json
{
  "mux": {
    "mode": "compat",
    "turbo": false,
    "connect": { "timeout": 20 },
    "inactive": { "timeout": 60 },
    "keep-alived": [0, 0],
    "congestions": 134217728,
    "intent": {
      "enabled": false,
      "mode": "auto",
      "window_ms": 250,
      "min_samples": 16384,
      "hysteresis": 20,
      "hold_ms": 5000,
      "rules": []
    },
    "flow": {
      "reorder": { "bytes": 67108864, "timeout": 400 },
      "session_reorder": { "bytes": 33554432 },
      "max_open": 4096,
      "unknown_cid": { "max": 64 }
    },
    "tx": { "queue": { "max": 4096, "stall": 8000 }, "ctrl": { "budget_frames": 32 } },
    "reliability": {
      "enabled": true,
      "rtx": { "bytes": 33554432, "flow_bytes": 8388608, "max_attempts": 8, "pto": false, "fast": false },
      "ack": { "delay": 10 },
      "gap": { "timeout": 3000 }
    },
    "fec": { "enabled": false, "group": 8, "flush": 20 },
    "debug": { "key": "" }
  }
}
```

## `mux` 字段

除 `keep-alived` 和 `congestions` 外，下列数值字段 `<= 0` 时都回退到对应默认值。

### `mode`

**类型：**string · **省略时：**`compat`

调度模式：`compat`（默认，原始行为）、`flow`（低延迟）、`balance`（按流有序的负载均衡）、`stripe`（实验性）。别名：`legacy`/`default`→compat，`flow-v1`/`primary`/`primary-link`→flow，`balanced`/`lb`/`load-balance`→balance，`striped`/`striping`→stripe。无法识别时回退 `compat` 并打印警告。可被 `--mux-mode` 覆盖。

### `turbo`

**类型：**bool · **省略时：**`false`

仅在 `flow` 下有意义：首包选最佳链路 + 预热链路。可被 `--mux-mode-turbo` 覆盖。

### `connect.timeout`

**类型：**int（秒） · **省略时：**`20`

子连接建立超时。

### `inactive.timeout`

**类型：**int（秒） · **省略时：**`60`

子连接空闲超时。

### `keep-alived`

**类型：**int[2]（秒） · **省略时：**`[0, 0]`

保活间隔范围，负数归零。

### `congestions`

**类型：**int（字节） · **省略时：**`134217728`

拥塞窗口预算；`0` 表示不限制，小于 1 MiB 的正数或负数回退为默认值。

### `intent`

**类型：**object · **省略时：**关闭

已有多路径部署要启用双向意图调度，在客户端和服务端的 `mux` 对象中均加入 `"intent": { "enabled": true }`。默认 `auto` 模式可以仅依靠流量行为分类，端口规则可选。首次配置多路径时，两端还要启用各自的 multipath 配置，并使用相同的传输认证活跃密钥。三路径验收使用 `path1` 作为低延迟链路、`path2` 作为备用带宽链路、`path3` 作为首选带宽链路，双端设置 `tcp.turbo: true` 以启用 TCP_NODELAY。

分类结果有 `unknown`、`latency`、`bandwidth` 三种状态。规则优先使用目标端口，未命中规则时根据早期流量的大小、速率和双向突发特征做有界启发式判断。启用后会在握手中请求 `flow_v2`，分类只在双方协商成功时参与跨 carrier 分流，旧式 `compat` 序列空间继续使用原有调度。

`mode` 支持 `auto`、`rules`、`behavior`。`window_ms` 是行为采样窗口，`min_samples` 是进入行为判断前需要观察的字节数，`hysteresis` 是分类置信度迟滞百分比。`rules` 按顺序匹配，每项格式为 `{ "intent": "latency|bandwidth", "ports": [443, 8443] }`。一旦判定为 `latency` 或 `bandwidth`，该分类在连接关闭前保持固定。`unknown` 流的小帧优先走低延迟路径，其余使用共享调度池。

首选组的发送 credit 全部耗尽时，调度器尝试次选组中的健康 carrier。在三个及以上 carrier 的拓扑中，带宽调度按路径 ID 查找首选组 `path3` 和次选组 `path2`。启用前应核对客户端 `multipath.paths[].id`。自定义 ID 的部署若两组均匹配不到，带宽帧可能持续排队。`hold_ms` 控制带宽首选路径劣化后重新尝试的间隔。双端运行时状态在 `multipath.intent` 下报告三类累计连接数、`intent_flip_count` 和 `credit_fallback_count`。

### `flow.reorder.bytes`

**类型：**int · **省略时：**`67108864`

每连接重排缓冲软上限：超过后暂停读取送来该帧的链路（反压），超过 2 倍才重置该连接。

### `flow.reorder.timeout`

**类型：**int（毫秒） · **省略时：**`400`

缺口等待超时（flow_v2 按连接，compat 按会话），从最近一次按序交付开始计时。

### `flow.session_reorder.bytes`

**类型：**int · **省略时：**`33554432`

会话级重排缓冲软上限：超过后暂停读取送来该帧的链路；超过 2 倍才重置连接（flow_v2）或重建会话（compat）。

### `flow.max_open`

**类型：**int · **省略时：**`4096`

最大逻辑流数。

### `flow.unknown_cid.max`

**类型：**int · **省略时：**`64`

未知流 ID 容忍数。

### `tx.queue.max`

**类型：**int（帧） · **省略时：**`4096`

发送队列高水位，达到后对读取限流。

### `tx.queue.stall`

**类型：**int（毫秒） · **省略时：**`8000`

发送队列持续积压超过该时间则重建会话。

### `tx.ctrl.budget_frames`

**类型：**int · **省略时：**`32`

每轮优先发送的控制帧数。

### `reliability.enabled`

**类型：**bool · **省略时：**`true`

协商可靠传输（重传）。

### `reliability.rtx.bytes`

**类型：**int · **省略时：**`33554432`

会话级重传发送窗口（未确认字节）；满时帧留在队列中等待，超过 2 倍才重建会话。吞吐上限约为 窗口 / RTT。

### `reliability.rtx.flow_bytes`

**类型：**int · **省略时：**`8388608`

flow_v2 下的单流发送窗口（不超过 `reliability.rtx.bytes`）；单条连接上限约为 该值 / RTT。

### `reliability.rtx.max_attempts`

**类型：**int · **省略时：**`8`

单帧最大重传次数。

### `reliability.rtx.pto`

**类型：**bool · **省略时：**`false`

超时（PTO）重传。默认关闭：承载链路是可靠字节流，PTO 多数只是把还排在慢链路后面的帧重发一遍。链路断开时，其上未确认的帧总会在剩余链路上补发。

### `reliability.rtx.fast`

**类型：**bool · **省略时：**`false`

dup-ACK 快速重传。默认关闭：跨链路乱序不等于丢包。

### `reliability.ack.delay`

**类型：**int（毫秒） · **省略时：**`10`

延迟 ACK 上限。

### `reliability.gap.timeout`

**类型：**int（毫秒） · **省略时：**`3000`

启用可靠传输时的缺口超时。

### `fec.enabled`

**类型：**bool · **省略时：**`false`

XOR 前向纠错。

### `fec.group`

**类型：**int · **省略时：**`8`

每组数据帧数。

### `fec.flush`

**类型：**int（毫秒） · **省略时：**`20`

未满分组的最长等待时间。

### `debug.key`

**类型：**string · **省略时：**空

调试用共享密钥；两端相同且非空时可用 `--mux-mode-set` 远程切换调度模式。可被 `--debug-key` 覆盖。
