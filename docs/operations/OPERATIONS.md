# Operations and Troubleshooting

> **Status:** Current
> **Type:** Operations guide
> **Last verified:** Runtime lifecycle, CLI help, stats, console UI, diagnostic sources, and v2.1.7 policy/DNS sources, 2026-10-07
> **Parent index:** [Operations](README.md) · **Chinese:** [运维与故障排查](OPERATIONS_CN.md)

## Start with observable state

Use the selected process mode and explicit configuration path first:

```bash
./ppp --mode=server --config=./server.json
./ppp --mode=client --config=./client.json
./ppp --mode=proxy  --config=./client.json
```

The runtime snapshot phase names are:

```text
idle → starting → preparing_host → connecting → handshaking →
applying_policy → connected → reconnecting → stopping → idle|failed
```

A phase is useful evidence, not proof that every host-network action or application flow works.

## Built-in local observability

| Surface | Use | Boundary |
|---|---|---|
| `--stats-json=<path|stdout>` | Write local runtime statistics as NDJSON. | It is a local output option, not a network metrics endpoint. |
| Runtime snapshot | Contains role, phase, endpoint/transport information, traffic, capabilities, and last error state. | Access follows the hosting/UI path; no public REST API is implied. |
| Console UI | Local interactive commands include `openppp2 help`, `restart`, `reload`, `exit`, and `info` when the UI is available. | Do not treat it as remote management. |
| Diagnostics | Inspect process output and the current error state; error formatting APIs are available to code that embeds the runtime. | A code-level diagnostic API is not an operator HTTP API. |

Restart controls are CLI-only:

| Flag | Meaning |
|---|---|
| `--auto-restart=<seconds>` | Auto-restart interval; `0` disables it. |
| `--link-restart=<count>` | Reconnection-attempt threshold for link restart; `0` disables it. |

## Troubleshoot by phase

| Symptom | First checks |
|---|---|
| Fails before `starting` or exits immediately | Mode/config path, process permissions, single-instance condition, and configuration parse errors. |
| Stays in `connecting` | Server URI, network reachability, local route policy, and upstream proxy configuration if used. |
| Stays in `handshaking` | Matching endpoint/transport/key configuration and the selected WebSocket/TLS path. |
| Stays in `applying_policy` | Virtual-interface availability, route/DNS permissions, and platform host state. |
| Reaches `connected` but carries no intended traffic | Host routes, bypass/DNS inputs, resolver behavior, remote server policy, and application test path. |
| Managed authentication fails | `server.node`, `server.backend`, C++/Go shared key, manager mode, and node record. |
| Server IPv6 fails | Linux-only server boundary, IPv6 mode/CIDR, host capability, TUN, routing/NDP/NAT66 prerequisites. |

## Safe operating sequence

1. Capture the exact command, selected config path, and initial output.
2. Confirm mode-specific host effects: listener bindings for server/proxy, or virtual interface/routes for normal client mode.
3. Record phase and last error before changing configuration.
4. Change one variable at a time; route/DNS and firewall changes can obscure one another.
5. Use an explicit maintenance/rollback procedure for host-managed settings rather than assuming the application can restore unrelated state.

## v2 Policy and DNS Diagnostics (v2.1.7 target)

The v2 policy, DNS, and durable-store capabilities below are part of the v2.1.7 code release target; v2.1.6 does not include them. Check the [release page](https://github.com/Miaocchi/openppp2/releases) for v2.1.7 package availability. The status file is local and is not a public REST endpoint; its writer lease permits one process to update a given identity at a time and is released when the owner closes.

| Symptom | Evidence to inspect |
|---|---|
| DNS answer differs by policy or resolver | Confirm the selected rule/resolver and `via` action, then inspect local status counters for cache hits, misses, coalescing, timeouts, upstream failures, and cancellations. Do not infer TCP/53 or encrypted-DNS interception from upstream UDP/TCP/DoH/DoT support. |
| Fake-IP changes after restart or is unavailable | Check the configured identity and pool, exclusive store lock, snapshot/journal integrity, mapping count, exhaustion, and persistence-error counters. A mismatched/corrupt store or failed durable append is an error condition; do not delete or recreate it as a first recovery step. |
| Policy update is prepared or fails | Inspect the redacted source metadata, validation diagnostic, durable current/previous state, and process identity in the local status record. Startup validates `CURRENT` and can restore validated `PREVIOUS`; a fetched candidate is fully compiled before runtime publication. Failed publication restores the durable pointer where possible. Shutdown cancels the updater, and a cancelled operation must not publish late results. |
| Status is missing or stale | Check whether another process owns the status writer lease and whether local status writes are failing. Policy execution can remain enabled without status reporting; the status file is observability evidence, not the authority for routing decisions. |

Keep private policy stores, status files, runtime configs, and logs in protected local storage. Share only redacted diagnostics; do not include node endpoints, credentials, private paths, or raw configuration in a report.

For host recovery, compare the captured pre-test configuration, resolver settings, route tables, and policy rules with post-stop state. A running process or a successful HTTP probe alone does not prove complete restoration. The public sanitized [Linux live report](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md) records its own before/after checks: service state, configuration hash, resolver file, routes/rules, and temporary interface/route cleanup. Its IPv6 route difference is limited to dynamic RA expiry and a recreated TAP link address; it does not establish general IPv6 validation. HTTPS and some proxy TCP probes remain failed; the report covers only its named Linux checks.

## Do not assume

- a `connected` snapshot is not an end-to-end traffic test;
- `--stats-json` is not Prometheus or a remote observability service;
- Console UI commands are not an authenticated remote administration protocol;
- no undocumented `/metrics`, lease, or IPv6-state REST endpoint is supplied by this runtime;
- default-route protection is not a universal kill switch.
- that v2.1.6 already contains the v2 capabilities described above; these are targeted for v2.1.7, with package availability shown on the release page;
- resolver upstream support for UDP/TCP/DoH/DoT means client TCP/53 or encrypted DNS is intercepted.

## Related pages

- [Deployment model](DEPLOYMENT.md)
- [Security model](SECURITY.md)
- [Routing and DNS](../guides/ROUTING_AND_DNS.md)
- [Management backend](../guides/MANAGEMENT_BACKEND.md)
- [Error codes](../reference/ERROR_CODES.md)
