# 多路复用

`mux` 只控制单个隧道会话内的逻辑流调度与缓冲。它不配置多路径入口；多路径请看客户端的 [`client.multipath`](client_CN.md#multipath) 和服务端的 [`server.multipath`](server_CN.md#multipath)。连接数由客户端参数 [`--tun-mux`](../CLI_REFERENCE_CN.md) 设置，默认为 `0`（关闭）；JSON 中没有 MUX 连接数字段。

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
    "flow": {
      "reorder": { "bytes": 8388608, "timeout": 400 },
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

### `flow.reorder.bytes`

**类型：**int · **省略时：**`8388608`

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
