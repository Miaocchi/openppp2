# Encryption and authentication

`key` configures encryption and obfuscation; `transport-auth` adds optional connection authentication. Check that the client and server agree before deploying. See also [Security](../../operations/SECURITY.md).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## `key`

### Structure

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

### Fields

#### `kf`

**Type:** int · **If omitted:** `0`

Header obfuscation parameter. **Omitted means 0, not 154543927**; it only has to match, but write it explicitly.

#### `kx`

**Type:** int · **If omitted:** `0`

Obfuscation parameter; negatives become 0.

#### `kl`

**Type:** int · **If omitted:** `0`

Obfuscation parameter, clamped to `0..16`.

#### `kh`

**Type:** int · **If omitted:** `0`

Obfuscation parameter, clamped to `0..16`.

#### `sb`

**Type:** int · **If omitted:** `0`

Shuffle block size, clamped to `0..5536`.

#### `protocol`

**Type:** string · **If omitted:** `aes-128-cfb`

Protocol-layer (handshake/control) cipher, an OpenSSL name. Unsupported names fall back to `aes-128-cfb`.

#### `protocol-key`

**Type:** string · **If omitted:** `"ppp"`

Protocol-layer key; empty falls back to `"ppp"`.

#### `transport`

**Type:** string · **If omitted:** `aes-256-cfb`

Transport-layer (data) cipher; unsupported names fall back to `aes-256-cfb`.

#### `transport-key`

**Type:** string · **If omitted:** `"ppp"`

Transport-layer key; empty falls back to `"ppp"`.

#### `plaintext`

**Type:** bool · **If omitted:** **`true`**

`true` means **no encryption**. Omitting it means plaintext; always write `false`.

#### `masked`

**Type:** bool · **If omitted:** `true`

Payload masking.

#### `delta-encode`

**Type:** bool · **If omitted:** `true`

Delta encoding.

#### `shuffle-data`

**Type:** bool · **If omitted:** `true`

Byte shuffling.

#### `simd-auto`

**Type:** bool · **If omitted:** `true`

Automatic SIMD implementation selection; local only, need not match.

### Startup warnings

Startup prints `[security] WARN` (warnings only, startup continues) when:

- either key equals the default `"ppp"` or is shorter than 8 bytes;
- `plaintext` is `true`;
- a cipher is RC4 / DES / Blowfish / CAST5 / SEED / IDEA or has a key shorter than 128 bits.

## `transport-auth`

Adds connection authentication with a 32-byte pre-shared key on top of `key`. Off by default.

### Structure

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

### Fields

#### `transport-auth.handshake-timeout-ms`

**Type:** int · **If omitted:** `5000`

Clamped to `1000..30000`.

#### `transport-auth.keys[]`

**Type:** array · **If omitted:** empty

At most 8. A non-array `keys` or a non-object element fails the load.

#### `transport-auth.keys[].id`

**Type:** string

1–63 characters, starting with a lowercase letter or digit, containing only `a-z 0-9 . _ -`; unique.

#### `transport-auth.keys[].state`

**Type:** string

`active`, `verify-only`, or `revoked`. When enabled, **exactly one** `active` key is required; at most 2 `verify-only` keys (for rotation).

#### `transport-auth.keys[].secret-file`

**Type:** path

Required for `active` / `verify-only`. Must contain exactly 64 lowercase hex characters (no newline), be a regular file owned by the current user, with no group/other permissions (`chmod 600`). **Secret files cannot be loaded on Windows.**

#### `server.transport-auth.enabled` / `client.transport-auth.enabled`

**Type:** bool · **If omitted:** `false`

Enable on the server / client.
