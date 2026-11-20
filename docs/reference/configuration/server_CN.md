# 服务端

在 `server` 中设置地址分配、客户端之间的流量和管理后端。监听地址和端口请看[连接与传输](transport_CN.md)。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## 结构

```json
{
  "server": {
    "node": 0,
    "log": "",
    "subnet": true,
    "mapping": true,
    "backend": "",
    "backend-key": "",
    "ipv4-pool": { "network": "10.0.0.0", "mask": "255.255.255.0" },
    "ipv6": {
      "mode": "nat66",
      "cidr": "fd42:4242:4242::/64",
      "gateway": "",
      "dns1": "",
      "dns2": "",
      "lease-time": 300,
      "static-addresses": {}
    },
    "peer-routing": { "enabled": false, "distribute": true, "allowed-routes": [] },
    "session_resume": { "enabled": false, "grace_ms": 60000 },
    "transport-auth": { "enabled": false }
  }
}
```

## `server` 字段

### `node`

**类型：**int · **省略时：**`0`

节点编号，负数归零。

### `log`

**类型：**path · **省略时：**空

会话日志文件，转换为绝对路径；空 = 不写。

### `subnet`

**类型：**bool · **省略时：**`true`

允许客户端之间通过虚拟子网互通。

### `mapping`

**类型：**bool · **省略时：**`true`

允许客户端端口映射（`client.mappings`）。

### `backend`

**类型：**string · **省略时：**空

管理后端 WebSocket 地址，见[管理后端](../../guides/MANAGEMENT_BACKEND_CN.md)。

### `backend-key`

**类型：**string · **省略时：**空

管理后端密钥。

### `ipv4-pool.network` / `ipv4-pool.mask`

**类型：**string · **省略时：**未配置

写了 `ipv4-pool` 对象就启用：服务端从该网段给每个会话分配虚拟 IPv4（Windows / Linux / macOS 客户端会把它应用到虚拟网卡）。网段或掩码非法时只记录日志、不分配。

### `ipv6.mode`

**类型：**string · **省略时：**关闭

`nat66` 或 `gua`，其他值 = 关闭。当前平台不支持服务端 IPv6 数据面时**加载失败**。 详见 [IPv6 客户端分配](../../guides/IPV6_CN.md)。

### `ipv6.cidr`

**类型：**string · **省略时：**空

前缀，如 `fd42:4242:4242::/64`。`nat66` 未填时使用内置 ULA /64；`gua` 必须是全球单播前缀。前缀非法时关闭 IPv6。

### `ipv6.gateway`

**类型：**string · **省略时：**前缀内第一个地址

不在前缀内时忽略。

### `ipv6.dns1` / `ipv6.dns2`

**类型：**string · **省略时：**空

下发给客户端的 IPv6 DNS。

### `ipv6.lease-time`

**类型：**int（秒） · **省略时：**`300`

租约时间，负数归零。

### `ipv6.static-addresses`

**类型：**object · **省略时：**空

`{ "<客户端GUID>": "<IPv6地址>" }`；不在前缀内或重复的条目被丢弃。

### `peer-routing.enabled`

**类型：**bool · **省略时：**`false`

客户端前缀路由，见[对端前缀路由](../../guides/PEER_PREFIX_ROUTING_CN.md)。

### `peer-routing.distribute`

**类型：**bool · **省略时：**`true`

向客户端分发路由表。

### `peer-routing.allowed-routes[]`

**类型：**array · **省略时：**空

宣告白名单，元素为 `{network, prefix, guid}`：只接受 GUID 与前缀完全一致的宣告。**为空时所有宣告都被拒绝。**

### `session_resume.enabled`

**类型：**bool · **省略时：**`false`

允许断线会话在宽限期内恢复。

### `session_resume.grace_ms`

**类型：**int（毫秒） · **省略时：**`60000`

宽限期，限制在 `1000..3600000`。

### `transport-auth.enabled`

**类型：**bool · **省略时：**`false`

见[传输认证](security_CN.md#transport-auth)。
