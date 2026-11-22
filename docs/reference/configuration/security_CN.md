# 加密与认证

`key` 控制加密与混淆，`transport-auth` 可为连接额外启用认证。部署前请核对两端配置，生产环境还应参阅[安全指南](../../operations/SECURITY_CN.md)。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## `key`

### 结构

```json
{
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "sb": 0,
    "protocol": "aes-128-cfb",
    "protocol-key": "<replace with a unique random secret>",
    "transport": "aes-256-cfb",
    "transport-key": "<replace with a different random secret>",
    "plaintext": false,
    "masked": false,
    "delta-encode": false,
    "shuffle-data": false,
    "simd-auto": true
  }
}
```

### 字段

#### `kf`

**类型：**int · **省略时：**`0`

头部混淆参数。**省略时是 0，不是 154543927**；两端一致即可，建议显式写出。

#### `kx`

**类型：**int · **省略时：**`0`

混淆参数，`< 0` 归零。

#### `kl`

**类型：**int · **省略时：**`0`

混淆参数，限制在 `0..16`。

#### `kh`

**类型：**int · **省略时：**`0`

混淆参数，限制在 `0..16`。

#### `sb`

**类型：**int · **省略时：**`0`

混洗块大小，限制在 `0..5536`。

#### `protocol`

**类型：**string · **省略时：**`aes-128-cfb`

协议层（握手/控制）加密算法，OpenSSL 算法名。不支持的名字回退为 `aes-128-cfb`。

#### `protocol-key`

**类型：**string · **省略时：**`"ppp"`

协议层密钥。空串回退为 `"ppp"`。

#### `transport`

**类型：**string · **省略时：**`aes-256-cfb`

传输层（数据）加密算法，不支持时回退为 `aes-256-cfb`。

#### `transport-key`

**类型：**string · **省略时：**`"ppp"`

传输层密钥。空串回退为 `"ppp"`。

#### `plaintext`

**类型：**bool · **省略时：****`true`**

`true` 表示**不加密**。省略即明文，务必显式写 `false`。

#### `masked`

**类型：**bool · **省略时：**`true`

载荷掩码。

#### `delta-encode`

**类型：**bool · **省略时：**`true`

差分编码。

#### `shuffle-data`

**类型：**bool · **省略时：**`true`

字节混洗。

#### `simd-auto`

**类型：**bool · **省略时：**`true`

自动选择 SIMD 加速实现，仅本地生效，不需要两端一致。

### 启动警告

启动时打印 `[security] WARN` 的情况（只警告、不阻止启动）：

- 任一密钥等于默认值 `"ppp"`，或短于 8 字节；
- `plaintext` 为 `true`；
- 算法属于 RC4 / DES / Blowfish / CAST5 / SEED / IDEA，或密钥长度低于 128 位。

## `transport-auth`

在 `key` 之外，再用 32 字节预共享密钥对连接做认证。默认关闭。

### 结构

```json
{
  "transport-auth": {
    "handshake-timeout-ms": 5000,
    "keys": [
      { "id": "example", "state": "active", "secret-file": "/etc/ppp/example.hex" }
    ]
  },
  "server": { "transport-auth": { "enabled": true } }
}
```

### 字段

#### `transport-auth.handshake-timeout-ms`

**类型：**int · **省略时：**`5000`

限制在 `1000..30000`。

#### `transport-auth.keys[]`

**类型：**array · **省略时：**空

最多 8 个；`keys` 不是数组或元素不是对象时加载失败。

#### `transport-auth.keys[].id`

**类型：**string

1–63 个字符，小写字母或数字开头，只能包含 `a-z 0-9 . _ -`，不得重复。

#### `transport-auth.keys[].state`

**类型：**string

`active`、`verify-only` 或 `revoked`。启用时必须**恰好一个** `active`；`verify-only` 最多 2 个（用于轮换）。

#### `transport-auth.keys[].secret-file`

**类型：**path

`active` / `verify-only` 必填。内容必须恰好是 64 个小写十六进制字符（无换行）；必须是普通文件、属于当前用户、组和其他用户无任何权限（`chmod 600`）。**Windows 不支持读取 secret-file。**

#### `server.transport-auth.enabled` / `client.transport-auth.enabled`

**类型：**bool · **省略时：**`false`

分别在服务端 / 客户端启用。
