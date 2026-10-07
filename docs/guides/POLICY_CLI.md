# Policy CLI

> **Status:** Unpublished local-workspace implementation. The current published/downloadable client does not include `client.policy` or `ppp policy`.
> **Last verified:** Workspace source and Linux live report, 2026-10-07.
> **Chinese:** [策略 CLI](POLICY_CLI_CN.md)

This guide describes the opt-in v2 policy implementation in the local kernel workspace. It is not a release announcement. The existing `client.routing` interface documented in [Routing and DNS](ROUTING_AND_DNS.md) remains applicable to the older published kernel. A configuration must use one policy generation; v2 rejects legacy route and DNS policy sources when they conflict.

## Create and check a policy

`init` writes policy and rule files, not a server/node configuration. It does not overwrite existing target files. Supported templates are `direct-all`, `proxy-all`, and `split-cn`. `split-cn` requires both `--geoip` and `--geosite`; each accepts an HTTP/HTTPS URL, which is recorded as a remote source without downloading, or a local regular file no larger than 64 MiB.

```sh
ppp policy init --out ./policy --template split-cn --runtime tun \
  --geoip ./geoip-test.txt --geosite ./geosite-test.txt --json
ppp policy check --config ./client-test.json --runtime tun --platform linux --json
```

The config needs `client.policy.version` set to `2`, a rules path, and a DNS resolver declaration. For example, an upstream may use a documentation address and reserved test domains:

```json
{
  "client": {
    "policy": {
      "version": 2,
      "rules": { "path": "./policy/rules.txt" },
      "dns": {
        "mode": "real",
        "resolvers": {
          "direct": {
            "via": "direct",
            "servers": ["udp://192.0.2.53:53"]
          },
          "proxy": {
            "via": "proxy",
            "servers": ["udp://192.0.2.53:53"]
          }
        }
      }
    }
  }
}
```

The referenced rules file must bind both DNS actions to declared resolvers:

```text
default proxy
dns direct direct
dns proxy proxy
[direct]
=service.example
```

`check` validates configuration, rule syntax, available local rule sets, and declared runtime/platform capabilities. Capability output is based on source paths; it does not establish that a VPN session works. `explain` evaluates policy without performing DNS lookup:

```sh
ppp policy explain --config ./client-test.json --runtime tun --platform linux \
  --domain service.example --network tcp --port 443 --json
```

V2 rule files require exactly one `default direct`, `default proxy`, or `default reject` directive. Action groups are `[direct]`, `[proxy]`, and `[reject]`. Conditions include exact domains (`=host.example`), suffix domains (`host.example`), subdomain-only wildcards (`*.example`), `keyword:value`, `regexp:value`, and IPv4 CIDRs. `[dns:NAME]` groups add resolver-specific domain exceptions; exactly one `dns direct NAME` and one `dns proxy NAME` binding are required, and both names must be declared resolvers. The resolver `via` field independently selects direct or proxy egress for its upstream connection.

When multiple rules match, explicit rules take precedence over expanded `set:NAME` rules. Among domain conditions, exact matches win first, followed by suffix/subdomain matches, keywords, then regular expressions. The longest matching suffix wins; for the same suffix length, `*.example` (subdomains only) wins over `example` (domain and subdomains). Equal-priority ties follow declaration order. IPv4 CIDRs are compared by prefix length, with the longest prefix winning. These rules describe policy selection, not network reachability.

Declare rule sets under `client.policy.rule-sets`, then reference them from an action group with `set:NAME`:

```json
{
  "client": {
    "policy": {
      "rule-sets": {
        "geoip-test": {
          "format": "geoip-text",
          "tag": "test",
          "source": { "path": "./geoip-test.txt" }
        },
        "geosite-test": {
          "format": "geosite-text",
          "tag": "test",
          "source": { "url": "https://rules.example.test/geosite.txt" }
        }
      }
    }
  }
}
```

Use exactly one `source.path` or `source.url`. Supported formats are `geoip-text`, `geoip-dat`, `geosite-text`, and `geosite-dat`; dat formats require a tag. Local sources are read and validated during check. A remote-only source declaration cannot pass offline validation until materialized; `update` fetches and validates remote content. `init --template split-cn` shows a generated `set:geoip-cn` / `set:geosite-cn` arrangement.

The resolver `servers` URI selects upstream transport, such as UDP, TCP, DoT, or DoH. That describes the policy engine's upstream connection. It does not widen client packet interception: the current TUN interceptor handles UDP destination port 53, not arbitrary TCP/53 or application DoH/DoT sessions.

With `dns.mode: auto`, the generated TUN template uses Fake-IP (`198.18.0.0/16`); HTTP and SOCKS runtimes use real DNS and preserve the original domain for local proxy policy. Fake-IP mappings use durable storage (default `./dns-fake-ip`, resolved beside the config). Do not delete that storage while clients may still cache addresses from its pool: changing the pool or identity, or clearing mappings, requires a restart and clearing client DNS caches. There is no online clear operation.

## Migrate and export

`migrate` writes a v2 draft and leaves the source configuration untouched:

```sh
ppp policy migrate --config ./old-client-test.json --out ./policy-draft \
  --bypass ./old-bypass-test.txt --dns-rules ./old-dns-test.txt \
  --runtime tun --platform linux --json
```

Exit code `5` means the draft has unresolved differences and needs review. Host-route equivalence does not prove whole-policy equivalence. DNS upstream egress, cache scope, Fake-IP, ECS, IPv6/AAAA, geo projection, and platform routing can require separate decisions. The current migration is a draft, not a whole-policy conversion guarantee.

`export` materializes a checked local ruleset or durable bundle into a policy-only output. It does not export the whole application config or Fake-IP mappings, and it does not fetch remote sources:

```sh
ppp policy export --config ./client-test.json --store ./policy-store \
  --out ./portable/client-test.json --json
```

## Update and status

`update` is the only CLI operation that downloads declared remote policy sources. Select its egress explicitly, using either a network interface or a local SOCKS5 endpoint. For direct interface updates, provide a UDP bootstrap resolver only when a source URL uses a hostname that must be resolved:

```sh
ppp policy update --config ./client-test.json \
  --interface eth0 --bootstrap udp://192.0.2.53:53 --json
```

```sh
ppp policy update --config ./client-test.json \
  --proxy-endpoint 127.0.0.1:1080 --json
```

`client.policy.updates.via` must match the selected update transport. The config's `bootstrap` entries are explicit direct UDP resolver endpoints for resolving update hostnames; examples must use test-only addresses. No credentials or production endpoint belongs in a guide.

Automatic runtime updates are separate from the one-shot CLI command. Setting `client.policy.updates.enabled` to `true` schedules updates at `interval` (default `24h`) using the configured `via`; the runtime can validate and publish the update into that running session. The CLI `update` command is prepare-only and leaves activation for a later start. For a direct-interface update, bootstrap endpoints are needed only when a remote source URL has a hostname that must be resolved; numeric-address URLs need no bootstrap. Proxy-endpoint mode rejects `--bootstrap`.

A successful update stores a validated durable candidate. With the runtime-compatible default store, the report can say `prepared_for_next_start`; it does not activate the candidate in an already running process. The update report names `runtime_active_version` and `prepared_version` separately; the `status` command reports them as `active_version` and `prepared_version`. The default durable store is `.ppp-policy` beside the configuration file. With a custom `--store`, the bundle is outside the runtime's configured store and the report says `prepared_in_custom_store`; use `export` to create a policy file that the application can load.

`status` reports aggregate counters and update state for the config identity. A status record, successful command, or running process does not prove a working VPN session.

```sh
ppp policy status --config ./client-test.json --json
```

## Commands and limits

The seven commands are `init`, `check`, `explain`, `migrate`, `export`, `update`, and `status`. Exit codes are `0` success, `2` argument/config/rule error, `3` unavailable source/store/status lease, `4` unsupported selected capability, and `5` migration draft requiring review. `--json` emits schema 1 diagnostics.

Linux live evidence covers selected HTTP, CONNECT over port 80, SOCKS UDP, DNS, Fake-IP, and controlled policy update cases. The DNS upstream probe timed out at 2 seconds and succeeded after the timeout was raised to 8 seconds; this does not show that every public resolver is reachable. HTTPS CONNECT, SOCKS HTTPS, proxy TCP, and TUN HTTPS probes still failed; this is not a successful HTTPS claim. Windows, macOS, Android, and iOS were not built or run for this workspace change. See [Linux live report](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md) for the tested cases and remaining gaps.
