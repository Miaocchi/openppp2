# Linux multipath impairment harness

This directory provides a private network fixture and production scenario
adapters. `infrastructure-check` never launches PPP. `production-smoke` starts
only the candidate binary selected with `--binary`, inside the private
namespaces, and reports the measured scope and gates.

## Production acceptance

Run as root on Linux with `ip`, `tc`, and Python 3. The runner creates uniquely
named client, router, and server network namespaces and veth links. All routes,
interfaces, and qdiscs exist inside those namespaces; it never changes host
routes, DNS, or proxy settings. A cleanup handler removes only resources
created by that invocation. Avoid running with a pre-existing PPP client if it
shares the process environment; the candidate smoke starts only the binary
explicitly selected for this run.

The `production-smoke` mode launches the candidate server and a primary
proxy-only client using `tools/compat` fixtures. The client has a policy-v2
`default proxy` rule and opens a SOCKS5 connection to the server namespace's
business IP, where an echo service listens. This sends the application stream
through the VMUX tunnel path and does not use FRP port mappings or virtual-peer
NAT. It repeats each path as a best-single baseline and then runs
multipath trials in the same isolated topology. Each trial checks an
exact-byte, half-closed stream and measures fresh-connection latency probes.
Before and after application traffic it samples per-path TBF byte counters;
each configured path must carry a minimum byte delta during successful
multipath transfers. Established sockets are only startup readiness, not path
participation evidence. Results include per-trial hashes and byte deltas,
median throughput, p50/p95/p99 latency, best-single ratios, and gates. It does
not cover outage recovery, RST semantics, or resource ceilings. Run it with:

```sh
sudo python3 tests/integration/multipath/run.py production-smoke \
  --binary ./bin/ppp --profile asymmetric-5-80-200 \
  --repeats 5 --payload-bytes 8388608 --output ./multipath-results/compare
```

The mode snapshots the selected executable into its mode-0700 result directory
and records its SHA-256 before starting processes. It creates one random active
transport-auth PSK there, uses it in mode-0600 configs, and deletes it after
stopping the candidate processes. `--binary` defaults to the repository's
`bin/ppp`.

The external scenario command can instead start the actual candidate client/server
or their test wrappers, exchange application traffic through the supplied
paths, and write a result JSON file. It receives `MP_TOPOLOGY_JSON`, a file
with namespace names and path endpoint addresses, plus `MP_RESULT_JSON`. Its
own fixture config must point the production binaries at those endpoints.

Example (scenario command is project-specific):

```sh
sudo python3 tests/integration/multipath/run.py scenario \
  --scenario ./path/to/production-multipath-scenario \
  --profile asymmetric-5-80-200 --output ./multipath-results/run-001
```

The command runs within the harness and inherits the environment variables
above. It is responsible for measuring the actual PPP application stream and
writing the schema documented below. It may use `ip netns exec` with namespace
names in the topology document to start its production components. A zero exit
code without a well-formed result is a failure. Scenario timeouts are enforced.

Available profiles include different RTT, jitter, independent packet loss,
and path-cap combinations. The
`shared-5-80-200` profile routes all paths through one bottleneck namespace and
one server-facing veth, then applies a single aggregate cap to that link in
both directions. Its topology records the shared interface and cap. Compare
it with `asymmetric-5-80-200`, which has independent per-path caps and no
common cap.

Run these timed fault plans while a production scenario is active:

```sh
python3 tests/integration/multipath/run.py fault-plan --topology <topology.json> \
  --case single-path-outage --path path2 --outage-ms 1500
python3 tests/integration/multipath/run.py fault-plan --topology <topology.json> \
  --case primary-path-outage --outage-ms 1500
python3 tests/integration/multipath/run.py fault-plan --topology <topology.json> \
  --case all-short-outage --outage-ms 1500 --grace-ms 30000
python3 tests/integration/multipath/run.py fault-plan --topology <topology.json> \
  --case all-grace-expired --outage-ms 31000 --grace-ms 30000
```

The final case leaves all links blackholed until namespace teardown so the
scenario can observe grace expiry and a subsequent new session. Interrupting
a fault plan restores modified paths. `impair` also supports manual `drop`,
`blackhole`, and `reset`. These replace only the child netem qdisc, preserving
the parent TBF path-rate cap.

## Scenario matrix and metrics

Run full production cases with at least 5, 80, and 200 ms path RTTs, differing path
rate caps, isolated-path and common-link capacity cases, then repeat with one
path blackholed and all paths interrupted/reconnected. For each case transfer
deterministic bytes in both directions, including half-close and reset cases.
The scenario must compare the received SHA-256 and exact byte count to the
sender, and record completion and recovery timeouts. This adapter does not
equate carrier establishment with usable throughput.

Result JSON required by the adapter:

```json
{
  "kind": "production-multipath",
  "scenario": "rtt-5-80-200",
  "integrity": {"sent_bytes": 1048576, "received_bytes": 1048576,
    "sent_sha256": "...", "received_sha256": "...", "duplicate_bytes": 0},
  "timing": {"completed": true, "completion_ms": 1234,
    "recovery_ms": 450, "reconnect_ms": 800},
  "latency_ms": {"p50": 22, "p95": 74, "p99": 110},
  "resources": {"peak_rss_kib": 12345, "peak_reorder_bytes": 4096,
    "retransmitted_bytes": 100},
  "throughput_mbps": 12.3,
  "baseline": {"best_single_mbps": 10.0}
}
```

The runner validates exact stream integrity, percentile ordering, finite
nonnegative metrics, completion/recovery/reconnect bounds, and reports
throughput relative to the measured best-single baseline. Use `--max-p95-ms`
and `--max-p99-ms` to enforce tail-latency ceilings. Numerical throughput
goals are intentionally scenario arguments because the design does not
promise simple capacity addition. The raw result and invocation metadata are
retained in the output directory for review. Baseline runs must use the same
host, payload, and impairment profile.

`python3 run.py infrastructure-check` checks only the fixture's namespace,
addressing, routes, qdiscs, connectivity, and cleanup mechanics. Its output
explicitly says it is not production acceptance. It must never be used as
evidence that PPP multi-path works.
