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
multipath trials in the same isolated topology. On a fresh multipath session,
the first authenticated record must be CreateRequest, not a recovery query.
The inner session handshake uses the committed multipath binding, without a
second plain transport-auth INFO exchange. Both peers must report VMUX link
open before the application transfer. The business target is a server address
without a client route, not one of the routable carrier endpoints. Each trial
times the exact-byte echo and fresh-connection latency probes. Outside that
measurement window, `payload.py` checks upload and download on separate SOCKS
streams. Each sender half-closes, then must read a receipt issued only after
its receiver verified the payload and EOF. Both endpoint processes must succeed;
a receiver digest or a negotiated VMUX link alone does not pass this check.
Before and after the timed application traffic it samples per-path TBF byte counters;
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

### Persistent application fault matrix

Use `production-fault-matrix` for outages against one persistent SOCKS TCP
stream. The command starts one real candidate client/server session for each
case, waits until the business stream is established, samples path counters,
injects the outage inside the private namespaces, restores the qdisc, and
waits for the same application stream to finish. The workload never reconnects;
continuity must come from the multipath session.

The default cases are:

| Case | Fault | Required result |
|---|---|---|
| `single-path-outage` | Blackhole the last configured path | Existing stream completes; surviving paths carry post-fault bytes |
| `primary-path-outage` | Blackhole path 1 | Existing stream completes on the remaining paths |
| `all-short-outage` | Blackhole every path for less than `resume_grace_ms` | Existing stream completes after restoration |
| `all-grace-expired` | Blackhole every path longer than `resume_grace_ms` | Existing stream fails; a false successful stream is a failure |

Run it with a fresh result directory:

```sh
sudo python3 tests/integration/multipath/run.py production-fault-matrix \
  --binary ./bin/ppp --profile asymmetric-5-80-200 \
  --grace-ms 30000 --outage-ms 1500 \
  --output ./multipath-results/fault-matrix
```

Each successful recovery case requires exact sent/received byte counts,
matching SHA-256, clean half-close/EOF, and no workload reconnect. The JSON
records counter snapshots and deltas for every configured path:
`pre_fault_bytes`, `during_fault_bytes`, `post_fault_bytes`, and
`total_bytes`. A path must meet `--minimum-path-bytes`; for single-path
outages every surviving path must also carry the minimum post-fault delta.
These TBF deltas include control traffic, retransmissions, and encapsulation.
The client runtime statistics are saved as `*-client-stats.ndjson`. They record
path state and carrier/frame counters. Release acceptance also needs
path-attributed application payload during the outage, recovery commit or
expiry tied to the original session generation, and evidence of old-generation
isolation on the same server. A workload socket timeout fails the
transport-termination check.

`fault-plan` and `impair` remain low-level qdisc controls. Running either one
without the persistent candidate session and its result JSON is not a fault
recovery acceptance result. On failure, retain the per-case JSON, any
`*-observation.json` written before the checks, workload result, candidate
logs, runtime statistics, and topology until review.

### Six concurrent streams per direction

Use `six-flow-integrity` to check the timed workload without running the echo,
best-single, or latency benchmarks. Run **both** profiles for independent and
shared bottleneck acceptance. Give each invocation a new output directory:

```sh
for profile in independent-12ms-24mbit shared-12ms-24mbit; do
  sudo python3 tests/integration/multipath/run.py six-flow-integrity \
    --binary ./bin/ppp --profile "$profile" \
    --output "./multipath-results/six-flow/$profile" || exit 1
done
```

Each profile uses two adaptive TCP carriers at 12 ms RTT and 24 Mbit/s per
path; the shared profile also caps the common link at 24 Mbit/s. After VMUX
readiness the flow/turbo session idles for 6 s, runs six concurrent uploads
for 25 s, idles for 12 s, runs six concurrent downloads for 12 s, and idles
for 6 s. The two directions use separate sets of SOCKS connections. All six
connections must reach the server-local business target before data starts.
Senders stream 32 KiB frames until their time window ends, not to a fixed
byte target. Connections have a 4 s connect timeout, no retry, 128 KiB send
buffers, and a 20 s I/O timeout. A phase has one outer send-window-plus-20 s
deadline for the entire group, not six successive waits.

For each flow the receiver returns its exact byte count and SHA-256. The
sender must verify both and send confirmation; the receiver must read it.
Both endpoint processes must exit successfully, and their six numbered
records must match for **each** direction. Missing, empty, truncated, or
mismatched streams fail the run; one completed stream cannot stand in for
another. `six-flow-integrity.json` is written only on success. On failure
inspect the per-role `upload-*.ndjson` / `download-*.ndjson` records and
candidate and endpoint logs. This gate measures integrity under concurrent
load, not throughput scaling, path participation, or fault recovery.

For a matched throughput comparison, run the same profile with
`--single-path path1`, `--single-path path2`, and without `--single-path`.
All three trials use the authenticated multipath carrier and the same traffic
workload. Use `--pool-metrics` in all trials to capture pool decisions and
retransmission counters in the PPP logs. Use a separate output directory for
each trial.

### External production scenarios

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
