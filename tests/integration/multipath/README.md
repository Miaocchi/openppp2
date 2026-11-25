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

#### Three independent paths

#### TCP intent acceptance

`intent-50-80-155` creates three independent paths at 50 Mbit/s and 50 ms
RTT, 300 Mbit/s and 80 ms RTT, and 700 Mbit/s and 155 ms RTT. Netem uses
uniform 5% RTT jitter and the acceptance sampler varies each path's TBF rate
by a bounded random 5% at each sample. The intent gate keeps two authenticated
VMUX carriers on every path, enables flow-v2 intent routing, and runs four
concurrent download flows, a small bidirectional web-burst stream, and one
sustained streaming stream. It checks exact payload integrity, classifier
telemetry, download migration toward path 3, web preference for path 1, and
streaming preference for path 3.

Run it in a root Linux environment with network namespaces enabled:

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile intent-50-80-155 --fixed-pool \
  --carriers-per-path 2 --intent-acceptance --output ./build/private/intent-acceptance
```

The run writes `intent-acceptance.json`, per-phase path wire samples, endpoint
logs, and the existing candidate evidence files. The repository's ordinary
unit and compile checks do not provide these namespace capabilities.

`--intent-migration-acceptance` uses the same topology with two or three fixed
carriers per path. Four concurrent downloads, forty web exchanges, and a
paced video segment stream share one origin port and use behavior
classification. Each download and video connection receives its metadata on
the low-latency path and requests the body on the same TCP connection.

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile intent-50-80-155 --fixed-pool \
  --carriers-per-path 2 --intent-migration-acceptance \
  --output ./build/private/intent-migration
```

The `intent-migration.json` report correlates each SOCKS socket lifetime
with one VMUX connection ID, traces the delivered bytes on both endpoints,
and checks that both endpoints classify the connection. Each download and
video flow requires its first payload on Path1, at least 80% of body delivery
on Path3, valid byte and SHA-256 receipts, clean EOF, and at most 1000 ms to
first body delivery or between payload progress samples. The web flow must
complete all requests with p95 at most 85 ms and zero payload delivered on
Path3 in either direction.

`--suite intent-strict` runs the six concurrent intent profiles in their
declared order. Each profile gets fresh namespaces, a candidate snapshot,
and its own report directory. The fixed-pool profiles open two carriers
per path. The dynamic-tide profile enables the adaptive pool.

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --suite intent-strict --intent-acceptance \
  --output ./build/private/intent-strict
```

Use `--profile intent-strict-burst-kill` in place of `--suite intent-strict`
to run an individual profile with its configured workload and gates.

Each `intent-strict.json` records one-second path bytes by traffic group,
web RTT percentiles, the longest 200 ms sampled payload pause, intent
telemetry, and observed and required values for each assertion.
The client correlates VMUX deliveries to individual SOCKS connections so
concurrent traffic on the same path or destination port can be attributed
to its workload. The fault and carrier logs preserve injection timestamps,
the selected socket, and one-second carrier counts. The burst-kill profile
also records server-side VMUX deliveries and origin request timestamps
to locate delay within an exchange. The suite returns a
failure status when any profile fails and lists all results in
`intent-strict-summary.json`.

`independent-90-125-180-stable` shapes three paths to 100, 200, and 300 Mbit/s
at 90, 125, and 180 ms RTT. `independent-90-125-180` adds 25 ms of configured
jitter to path 2. Use `--fixed-pool --carriers-per-path 1` for three total
carrier sockets. The fixed-pool startup gate checks the configured carrier
count on each path. `--tcp-turbo` enables TCP_NODELAY on both endpoints.

Use `--direction upload` and `--direction download` for the one-way runs.
`--direction bidirectional` sends in both directions simultaneously over
the same six SOCKS streams and checks each direction's count, SHA-256,
receipt, and EOF. The default `sequential` retains the earlier workload.

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile independent-90-125-180-stable \
  --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
  --pool-metrics --output ./build/private/three-path-stable-01
```

Repeat with `--single-path path1`, `--single-path path2`, and
`--single-path path3` to measure the best-single baseline using the same
workload and profiling options. Use a fresh result directory for each trial.
For timed degradation, add `--impair-path path2 --impair-after 15
--impair-for 12` and one of `--impairment delay --impair-value 120`
(added RTT in ms), `--impairment rate --impair-value 20` (cap in Mbit/s),
or `--impairment blackhole`. The harness records the start and end of each
qdisc change in `impairment-events.ndjson`.

#### Executor concurrency

Both endpoint templates ship with top-level `"concurrent": 1`, which runs all
executor work on a single IO worker thread. `--concurrent N` (1..64) overrides
that field on both the server and client candidate configurations for the
trial, without touching `tools/compat/server.json` or `client_proxy.json`.
Keep the default for the compatibility baseline and raise it only to A/B the
executor. Use a fresh output directory per value:

```sh
for workers in 1 4; do
  sudo python3 tests/integration/multipath/run.py six-flow-integrity \
    --binary ./bin/ppp --profile independent-90-125-180-stable \
    --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
    --concurrent "$workers" --pool-metrics \
    --output "./build/private/three-path-stable-concurrency-$workers" || exit 1
done
```

The resolved value is recorded in `topology.json`
(`executor_concurrency`), `run-evidence.json` (`executor_concurrency`), the
successful `six-flow-integrity.json` (`executor_concurrency`), and the
effective per-trial `six-flow-*-server.json` / `six-flow-*-client.json`
configurations. Compare goodput and per-path telemetry between the two
directories; both runs must still pass the six-flow integrity gate.

`run-evidence.json` records the runner and candidate command arguments,
checkout commit and dirty-tree fingerprint when available, selected binary
SHA-256, topology reference, executor concurrency, startup carrier counts, and
runner exit code. `--wire-interval N` (1..6, default 6) sets the spacing in
seconds between root TBF wire samples written to `upload-wire.ndjson` /
`download-wire.ndjson` during each direction; the resolved value is recorded
as `wire_interval` in `run-evidence.json`. With `--wire-interval 1`, both
endpoints finish the stream handshakes and wait while the runner reads the
initial TBF counters. Releasing the workload starts the measurement clock.
A 36-second run records 36 consecutive windows per selected direction,
including startup. Intervals of 2..6 seconds use the historical baseline at
second 3 and take their first sample at second 9. Rates use the recorded
elapsed time between counter reads. TBF bytes include control traffic and
TCP acknowledgments for traffic in the opposite direction.

Use `--wire-interval 1 --min-path-utilization 0.95` to require 95% of
each selected path's configured rate in every second and direction. The
runner reads the selected TBF counters concurrently and records monotonic
timestamps immediately before and after each path's `tc` query. It also
records when it starts and finishes releasing both workload processes.
Each query must lie within 50 ms of its scheduled boundary, relative to
both ends of release. This 5% timing tolerance allows process scheduling
and `tc` overhead. The baseline must be captured before release. For
each path the gate divides the byte delta by the **longest possible**
elapsed time between the two counter reads. Counter discontinuities,
missing samples and queries outside the timing tolerance fail the gate.
Historical files with only batch timestamps still yield diagnostic Mbps
and window counts. Their lack of per-path timing evidence prevents a
verified pass.
A path missing its counter in any window also fails. The runner exits
with a nonzero status on failure. `six-flow-integrity.json` records
`integrity_passed`, the `performance` report, and overall `passed`.
The performance report identifies failed seconds per path, numbered from 1.
Each direction counts a second in `valid_windows` when every selected path
has a verified counter delta and timing interval, including seconds below target.
Omitting the utilization option selects the integrity-only check.

Use `--duration 60` to compare cold start with sustained utilization.
Each direction reports the number of seconds when all selected paths reached
their target together. In bidirectional mode, `performance.bidirectional`
counts seconds when all paths met the target in both directions at once.
For runs longer than 36 seconds, `last_36_seconds` repeats these counts
over the final 36 windows.
At 60 seconds the summary covers seconds 25 through 60 inclusive. The
overall acceptance result includes every window starting at second 1.

Saved output can be evaluated offline from the repository root.

```sh
PYTHONPATH=tests/integration/multipath python3 -c \
  'import json, sys; from pathlib import Path; from run import evaluate_path_utilization; print(json.dumps(evaluate_path_utilization(Path(sys.argv[1]), 0.95), indent=2))' \
  ./build/private/three-path-stable-01
```

Compare the `client-stats.ndjson` and `server-stats.ndjson` link snapshots
using the VMUX session ID and shared link ID. Each endpoint's local link
generation tracks changes on that endpoint. `path-telemetry.ndjson` pairs
the corresponding stats with raw TBF deltas from `upload-wire.ndjson` and
`download-wire.ndjson` using the host monotonic clock. It reports received
unique VMUX payload, original sender submissions, and sender retransmits
separately. A delta is null when matching counter snapshots are absent or a
link identity changes. The run manifest summarizes matched session IDs,
carrier counts, and paths with received payload in the available windows.
`--pool-metrics` keeps pool events in the candidate logs and extracts them
into `pool-events.ndjson` with an estimated monotonic timestamp. Verified
workload goodput in `six-flow-integrity.json` uses the application payload
bytes and the measured send interval.

`--qdisc-metrics` (default off) additionally reads
`tc -j -s qdisc show dev <interface>` at every `--wire-interval` sampling
offset for both path egresses already in the topology: the client-side
interface (`client_if`) and the router-side interface
(`router_client_if`). It appends one record per interface to
`qdisc-metrics.ndjson` in the run output directory. Each record contains
`path_id`, `role` (`client` or `router`), `namespace`, `interface`,
`directions`, `monotonic_ms`, and parsed `tbf` (`bytes`, `packets`,
`drops`, `overlimits`, `backlog`, `qlen`) and `netem` (`packets`, `drops`)
counters. Counters absent from `tc` output are omitted. A failed `tc` read
sets `error` on that record and does not fail the run. The timestamps use the
same host monotonic clock as `start_monotonic_ms` / `end_monotonic_ms` in the
wire and telemetry files, so qdisc queue growth, drops, and TBF shaping can be
aligned with VMUX payload deltas. This option is diagnostic only:
it adds no acceptance gate, and its absence leaves the default run outputs
unchanged. The resolved value is recorded as `qdisc_metrics` in
`run-evidence.json`.

`--socket-metrics` (default off) additionally runs `ss -tinm` inside the
client and server namespaces at every `--wire-interval` sampling offset and
appends records to `socket-metrics.ndjson` in the run output directory. In
one-second mode it also samples both endpoints before releasing the workload,
so the first window can be compared with the starting TCP congestion window. Each
record contains the `role` (`client` or `server`), `namespace`, `directions`,
`monotonic_ms`, the exact `ss` command, the full raw `ss` output (`raw`), and
parsed `sockets` for every established TCP connection.
Each socket record keeps the raw state line and detail lines and adds
structured `state`, `recv_q`, `send_q`, `local`, `peer`, `local_address`,
`local_port`, `peer_address`, `peer_port`, and a `fields` map of TCP values
parsed from the detail text (`cwnd`, `rtt`, `rto`, `ssthresh`, `snd_wnd`,
`bytes_sent`, `bytes_acked`, `retrans`, `pacing_rate`, and similar). Parsing
is best effort: the raw lines are authoritative, and a failed `ss` read or
parse sets `error` on that record without failing the run. The timestamps
share the host monotonic clock with the wire, telemetry, and qdisc files, so
TCP congestion-window, send-queue, and retransmit state can be aligned with
TBF backlog, drops, and VMUX payload deltas. This option is diagnostic only:
it adds no acceptance gate, and its absence leaves the default run outputs
unchanged. The resolved value is recorded as `socket_metrics` in
`run-evidence.json`.

#### Per-path window alignment

`path-telemetry.ndjson` keeps its existing top-level fields and additionally
aligns the diagnostic stages for each `path` in each six-second wire window.
The added object is additive, so older consumers and older result directories
remain readable:

- `tbf_start_bytes` / `tbf_end_bytes` / `tbf_bytes` are the absolute and delta
  root-TBF counters for that path. The direction's existing observation that
  upload counts the client egress and download counts the router egress is
  unchanged.
- `vmux` describes the sending endpoint's carrier links for that path:
  `selection_count`, `probe_selection_count`, and
  `candidate_observation_count` are window deltas of
  `scheduler_selection_count`, `scheduler_probe_selection_count`, and
  `scheduler_candidate_observation_count`.
  `send_credit_bytes` / `wire_inflight_bytes` are the final carrier totals,
  `selected_send_credit_bytes` / `selected_wire_inflight_bytes` are the
  scheduler's last selected values, while `observed_send_credit_bytes` /
  `observed_wire_inflight_bytes` are its last candidate-observation values.
  `write_submitted_bytes` is the `tx_bytes` delta (VMUX frame bytes submitted
  to carrier writes, including retries); `queued_bytes` is the final
  outstanding local write bytes; `write_completed_bytes` is the submitted
  delta minus the queued-byte change. `stable` is true only when the path's
  link-id/generation set is identical at both window boundaries; deltas are
  null otherwise.
- `socket` describes the sending endpoint's carrier sockets matched to that
  path by the topology's client/server addresses. It reports final
  `notsent_bytes`, `snd_wnd_bytes`, `cwnd_segments`, `send_q_bytes`,
  `recv_q_bytes`, and `tcp_acked_bytes`. `tcp_acked_delta_bytes` and
  `tcp_sent_delta_bytes` are window deltas, null when the socket set is absent
  at either boundary or its local/peer identity changes. Application or proxy
  sockets (for example loopback business streams) are not path-attributed.
  `sample_ms` is the monotonic timestamp of the selected `ss` sample, within
  2.5 s of the window boundary; `socket_count` is the number of matched
  carrier sockets.
- `ack_delivery` repeats the receiver-confirmed
  `vmux_unique_payload_bytes` and the sender's TCP
  `tcp_acked_delta_bytes`, so VMUX payload delivery and TCP ACK delivery can
  be compared directly in one row.

`socket-metrics.ndjson` gains a `path_id` on each socket record that matches a
configured path endpoint. When reading an older file without that field,
`record_path_windows` falls back to address matching using `topology.json`.
These additions are diagnostic only and add no acceptance gates.

Window boundary samples are selected by nearest timestamp within 1 s; an older
sample is not silently reused for a shortened interval. Wire sampling includes
the send-window endpoint, and partial intervals are excluded from six-second
acceptance aggregation. Server VMUX links without a client `path_id` use the
fixture's stable `link_id == path number` fallback for diagnostics; this does
not alter production scheduling or the wire gate.

#### Harness TBF burst

`--tbf-burst-kb` (128, 256, 512, or 1024; default 128) sets the root TBF
burst in KiB on every per-path egress qdisc for an infrastructure A/B. The
shared router/server aggregate link uses twice the selected value, so the
default reproduces the historical 128 KiB per-path and 256 KiB aggregate
bursts. Use this only to separate harness shaping behavior from candidate
scheduler behavior; the workload, readiness gates, and candidate
configuration are unchanged. The resolved value is recorded as
`tbf_burst_kb` in `topology.json`, `run-evidence.json`, and successful
`six-flow-integrity.json`.

#### TCP congestion control A/B

`--congestion-control {bbr,cubic}` selects the TCP congestion control
algorithm used by the candidate inside an otherwise identical fixture. It is
available on `six-flow-integrity` only. The default leaves every namespace
value untouched, so the kernel default still applies and no sysctl runs. This
isolates the transport algorithm from scheduler, qdisc, and workload
differences when comparing runs.

The harness writes the per-namespace sysctl with

```sh
ip netns exec <ns> sysctl -q -w net.ipv4.tcp_congestion_control=<value>
```

for the fixture's `client`, `router`, and `server` namespaces. The write
happens after the fixture setup/verification step and before either candidate
endpoint starts, so every transport socket the endpoints create is subject to
the selected algorithm. `net.ipv4.tcp_congestion_control` is a network-namespace
sysctl: the host and any other namespace are unaffected, and the bottleneck
relay namespace is not touched. There is no in-run switching; the selection is
fixed for the invocation and disappears with the namespaces.

Each namespace is read back with `sysctl -n` after the write. An unavailable
algorithm (for example `bbr` without its module) fails the write, and a
readback mismatch fails the readback; both abort the run with a
`multipath harness error` naming the namespace, before the endpoints start.
An unknown value is rejected by the parser with exit code 2.

Run the paired A/B with identical binary, profile, carrier count, direction,
and duration; only the algorithm differs:

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile independent-90-125-180-stable \
  --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
  --wire-interval 1 --qdisc-metrics --socket-metrics \
  --output ./build/private/cc-default

sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile independent-90-125-180-stable \
  --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
  --wire-interval 1 --qdisc-metrics --socket-metrics --congestion-control cubic \
  --output ./build/private/cc-cubic
```

Use `--socket-metrics` to confirm the change took effect: the endpoint sockets
in `socket-metrics.ndjson` should show congestion-window and pacing behavior
consistent with the selected algorithm rather than the previous one. The
resolved value is recorded as `congestion_control` in `run-evidence.json`
(`null` when the option is absent) and in successful `six-flow-integrity.json`;
`topology.json` gains `"congestion_control": "<value>"` only when the option is
used, matching the `ack_priority` convention.

#### VMUX frame timing

For per-frame VMUX timing, set `OPENPPP2_VMUX_PACKET_TIMING_PREFIX` in the
environment inherited by both endpoints. For example, set it to an absolute
prefix such as `/tmp/issue156/frames` in an existing private directory.
The endpoints append NDJSON to `frames.client.ndjson` and `frames.server.ndjson`.
Use a fresh prefix for each diagnostic run. The trace contains PUSH metadata
for sequences 1, 33, 65, etc., and all VMUX ACK frames. It records no
application payload or network addresses. Synchronous diagnostic writes can
change timing, so measure acceptance throughput with the trace disabled.

Each row has a local monotonic timestamp `us`, `session`, endpoint `role`,
frame `kind`, and local numeric `carrier`. Sampled PUSH frames have a
connection ID `cid` and sequence `seq`. A retransmission is marked `retry`
on the sending side. The transmit events are `tx_post`, `write_start`, and
`write_complete` (or `post_failed` / `write_failed`). Receive events are
`rx_read` and `rx_dispatch`. The carrier write completion reports local
transport completion. The peer reports VMUX feedback separately.

Session-level `ack_generated` and `credit_release` events use `carrier=0`
and include `rows` of `{cid, ranges}` with inclusive sequence ranges.
`credit_release.bytes` is the session's decrease in retained RTX bytes
while processing that ACK. Other `bytes` fields, where populated, give
VMUX frame size. Match ACK transport events using `ack_id`, a 64-bit
fingerprint of the encoded ranges. Repeated identical cumulative feedback
has the same fingerprint. Correlate the same data frame across endpoints by
session, sending role, CID and sequence. Compat ordering uses ACK CID 0 for
its session-wide sequence space. Subtract timestamps only within one endpoint.

#### ACK FIFO A/B (diagnostic)

`--ack-priority` is an opt-in, reversible qdisc A/B for the hypothesis that
download ACKs wait behind upload bulk in the same per-path FIFO. It is
available on `six-flow-integrity`, `production-smoke`,
`production-fault-matrix`, `scenario`, and `infrastructure-check`; the default
is off, so the default layout and every default output stay unchanged.

The default layout is root TBF (`1:`) with one netem child (`10:`) at parent
`1:1`. The A/B layout keeps the identical root TBF rate/burst/latency and
inserts a size-classified `prio` child:

```text
root TBF 1: rate/burst/latency (unchanged)
└── prio 20: bands 3
    ├── 20:1  ACK/control band  ← netem 30:, same delay/jitter/loss
    └── 20:2  bulk band         ← netem 40:, same delay/jitter/loss
```

Two `tc` filters classify IPv4 total length `< 256` bytes (pure ACK / small
control) into band 0 and everything else into band 1. Each band has an
identical netem, so the configured one-way delay, jitter, and loss still apply
to every packet; only the service order changes. The TBF still owns the path
rate cap. `fq_codel` is not used here because upload data and download ACKs of
one VMUX carrier share the same 5-tuple, so a flow-hash qdisc places them in
the same flow queue and cannot separate them. Size-classified strict priority
is the direct test for head-of-line blocking.

Run the paired A/B with a bidirectional workload; an upload-only or
download-only phase has no upload bulk and download ACKs sharing the same
client egress, so it is a control, not the test:

```sh
sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile independent-90-125-180-stable \
  --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
  --wire-interval 1 --qdisc-metrics --socket-metrics \
  --output ./build/private/ack-fifo-baseline

sudo python3 tests/integration/multipath/run.py six-flow-integrity \
  --binary ./bin/ppp --profile independent-90-125-180-stable \
  --fixed-pool --carriers-per-path 1 --direction bidirectional --duration 48 \
  --wire-interval 1 --qdisc-metrics --socket-metrics --ack-priority \
  --output ./build/private/ack-fifo-priority
```

Use the same binary, profile, carrier count, direction, and duration in both
runs; only `--ack-priority` differs. A download-only run on the same profile
is a useful control: its client egress carries ACKs without upload bulk, so
even the baseline should show a much smaller TBF backlog.

The A/B confirms the mechanism only when all of the following hold:

1. **Baseline queue:** during bidirectional sending, `qdisc-metrics.ndjson`
   records for `role=client` show a sustained `tbf.backlog` (for example a
   median above 64 KiB) and `tbf.qlen` above zero on the loaded paths. A
   download-only baseline shows a near-zero client-egress backlog.
2. **ACK band separation:** in the `--ack-priority` run, each client record has
   `prio` and `netem_bands`. The band with `parent=20:1` stays nearly empty
   (`queue.backlog` median below 8 KiB, `queue.qlen` p95 at most 2) while
   `parent=20:2` carries the bulk backlog. This is the direct proof that small
   frames bypass the bulk FIFO; it should hold without changing the TBF cap.
3. **Reverse ACK RTT:** in `socket-metrics.ndjson`, the server-side carrier
   sockets (exclude the workload ports 19002/19003) show a lower smoothed
   `rtt` in the ACK-priority run. Expect the baseline median to be well above
   the configured path RTT and the variant median to approach it. A robust
   test is a variant/baseline ratio at or below 0.75 on at least two of the
   three paths while the upload direction is active.
4. **Directional outcome:** in `six-flow-integrity.json`, the variant download
   `verified_payload_mbps` improves (for example at least 10%), while upload
   goodput stays within run-to-run noise. Exact-byte/SHA-256/EOF gates must
   still pass in both runs.

If 1 and 2 hold but 3 or 4 does not, the FIFO is confirmed at the qdisc but the
remaining download limit is above it (scheduler credit, payload-to-wire
pacing, or app-limited sending), not evidence against the FIFO mechanism.

Reversibility is inherent: the mode is per-invocation, all qdiscs live in
private namespaces that are deleted by the cleanup handler, and the default
command without `--ack-priority` restores the original TBF + single-netem
layout. When a topology was recorded with `ack_priority`, `impair` and
`fault-plan` update both band netems and their restore path keeps the same
band layout; `fault-plan`/`impair` never edit the root TBF. In the A/B mode
`qdisc-metrics.ndjson` additionally carries `prio` and `netem_bands`
(band `parent`, counters, `qlen`, `backlog`); baseline records are unchanged.
The resolved flag is recorded as `ack_priority` in `run-evidence.json` and
successful `six-flow-integrity.json`, and `topology.json` gains
`"ack_priority": true` only in the A/B mode.

Useful checks after the pair:

```sh
# TBF backlog and per-band queue, client egress only
jq -c 'select(.role=="client") | {path_id, tbf_backlog:.tbf.backlog,
  prio_backlog:.prio.backlog, bands:.netem_bands}' \
  build/private/ack-fifo-priority/qdisc-metrics.ndjson | head

# Median server-observed smoothed RTT per carrier peer
python3 - ack-fifo-baseline ack-fifo-priority <<'PY'
import json, statistics, sys
for directory in sys.argv[1:]:
    samples = {}
    with open(f"build/private/{directory}/socket-metrics.ndjson") as source:
        for line in source:
            record = json.loads(line)
            if record.get("role") != "server":
                continue
            for sock in record.get("sockets", []):
                rtt = (sock.get("fields") or {}).get("rtt")
                if not rtt or sock.get("local_port") in ("19002", "19003"):
                    continue
                samples.setdefault(sock.get("peer_address"), []).append(
                    float(rtt.split("/")[0]))
    print(directory, {peer: round(statistics.median(values), 2)
                      for peer, values in sorted(samples.items()) if values})
PY
```

`--tbf-burst-kb` remains a separate, weaker control: it changes token-bucket
depth but not service order, so it cannot separate ACKs from bulk. Do not
combine it with `--ack-priority` when attributing the result.

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
