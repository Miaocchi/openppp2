# XTCP platform build notes

> Status: Active
> Type: Design
> Last verified: 2026-10-20 (against root `CMakeLists.txt`, `ppp.vcxproj`, `android/build.sh`, `tools/run_windows_xtcp_loopback_benchmark.ps1`)

Rebase validation (2026-10-18): the new pin passes Linux product, contracts
(188/188), assertion-enabled general tests (179/179), fault (20/20), and full
standalone tests (241/241). Windows x64 static Release builds with zero errors;
both offline tests pass (2/2), and all ten packaged Driver files match. No
Windows client or driver was started or installed.

Runtime acceptance is blocked. The isolated before/after stall campaign
qualifies 160/160 cells, but the three-round P16/P64 DL/UL, one/two-shard,
GSO-off/on matrix qualifies only 83/96. Thirteen cells contain zero-completion
flows: seven before and six after (107/104 individual flows). They occur in
P64/GSO-off uploads on both binaries and shard counts, plus one old-binary
download cell. Positive aggregate throughput and zero all-stream-zero intervals
do not satisfy per-flow completion. Read-only samples show kernel TUN TX drops
and BBR retransmission backoff on both versions; the complete recovery cause
and any rebase-specific contribution remain unverified. Original failures and
additional matched comparisons are retained separately in the local execution
report under `build/plan15/`; no CC, queue, default or feature gate is promoted.

Root cause found (2026-10-18): the zero-completion flows are an upstream xtcp
bug. `BuildSegmentPacket` allowed 4 SACK blocks next to the 12-byte TSopt, so
ACKs for flows with many out-of-order ranges had 48 bytes of options and a
68-byte TCP header; the 4-bit data offset wrapped (17 -> 1) and the kernel
dropped every such ACK, leaving the sender in RTO backoff. A header-only TUN
capture plus a diagnostic build (per-flow ingress/egress counters and the last
output headers; not committed) showed XTCP received every retransmission and
wrote an ACK to the TUN each time, each with IP total length 88 and data offset
1. Patch 0036 sizes SACK blocks to the remaining 40-byte option space (3 with
timestamps, as Linux; 2 with MD5); its `test_sack_option_budget` fails on
upstream main and passes with the fix, and it is upstreamed as
liulilittle/xtcp#2. Patch 0035 separately fixes ACK/window state being cleared
when ACK construction fails on buffer exhaustion; it alone did not remove the
P64 stalls.

P64 UL GSO-off A/B on the shared remote VM, diagnostics off, three alternating
rounds (median Mbps; zero-completion flows summed over rounds, of 64 per round):

| Cell | 0035 only | Zero flows | 0035 + 0036 | Zero flows |
| --- | ---: | ---: | ---: | ---: |
| One shard | 288.2 | 60 | 250.4 | 0 |
| Two shards | 769.2 | 36 | 707.2 | 1 |

The native control ran at 289.7 Mbps with no zero flows. Open items: one zero
flow remains in one two-shard round; aggregate throughput drops 8-13% because
previously stalled flows now compete (retransmissions rise from about 90-106k
to about 152k in the one-shard cell); P16/P64 DL, GSO-on and the stall
campaign have not been re-run with 0036. See XTCP_INTEGRATION_CN.md §11.186.

XTCP is opt-in on Windows and Android. Linux desktop retains its current
default. All platforms use the pinned source in `third-party/xtcp` and the
same production target settings from the root CMake build (revision
`f71548f3664740bf87b40bf0b226d2ee315109a8`). The verified archive SHA256 is
`ff0a0a6021abeeadc84337992d765a02eee57dfb3e3ed7ef7e8f3bb22e08656e`.

Rebase onto upstream f71548f (2026-10-25): upstream added KCC v2
(kernel-faithful delivery/recovery), CA state-machine, RACK/SACK/undo/RTO
fixes, O(1) pipe queries and SIMD copy+checksum, and merged former local 0036
(liulilittle/xtcp#2), whose patch file is removed. The remaining 26 patches
keep their IDs. Adapted during the rebase: 0004 (keeps upstream's
app-limited marking at the send entry), 0012 (rate-change telemetry is now
sampled after the ACK flush, which upstream moved behind the CC hook), 0023
(offset-aware pending-send accessors in the new upstream app-limited check,
ACK flush and fast-open paths), 0026 (KCC v2 no longer aggregates per-round
samples; `round_sample_*` telemetry fields stay and report 0) and 0034 (new
upstream tests follow the timestamp-aware payload cap and burst pacing).
Offline ctest (WSL GCC 12, Release, checksum validation, -j1): upstream main
238/239, rebased 241/243. Both fail `test_ts_ooo_drain` (also failing on the
old pin). The rebased tree initially failed the KCC ECE assertion in
`test_ecn_cc`. Investigation showed that upstream passed only by coincidence. Under
the frozen virtual clock, per-segment pacing sends one segment per round after
warm-up. KCC's max-bandwidth filter then ages out and cwnd falls to the floor
(90 -> 4) on the same ACK *without* ECE as well. With 0005 burst pacing, the
bandwidth model stays fresh and cwnd stays at the BDP target. Kernel BBR/KCC has
no ECN window response beyond `tcp_enter_cwr` running the pass-through
`ssthresh` hook (`tcp_cong_control` returns early for `cong_control`), so the
port's only effect is the clamp to that threshold (178 > 90 here). 0034 now
asserts that contract with seeded twin runs: the threshold is real, cwnd <=
threshold after the ECE ACK, and the ECE window is <= the plain-ACK window. The
wire-level CUBIC case still proves that a CE mark cuts cwnd under burst pacing.
New patch 0037 (`0037-rcv-mss-immediate-ack.patch`) makes the receiver ACK at
once when one in-order segment carries more than the advertised MSS (1460).
This matches Linux `__tcp_ack_snd_check`, whose `rcv_nxt - rcv_wup > rcv_mss`
test counts bytes. Before 0037, a GRO/LRO super-segment counted as one segment
and waited for the 40 ms delayed ACK. `test_rcv_mss_ack` fails without the fix
and passes with it. This is an upstream receiver behavior: 0005 itself emits
only segments no larger than the MSS. Final stack (27 patches): 243/244, with
only `test_ts_ooo_drain` failing. See XTCP_INTEGRATION_CN.md §11.187.

The current local series has 27 patches with stable, sparse IDs: 0001-0015,
0023, 0025-0029, 0031-0035 and 0037 (0035: retry ACK/window construction after
allocation failure; 0037: byte-based immediate ACK for super-segments; former
0036, SACK blocks fit the option space, is upstream as liulilittle/xtcp#2 and
its ID is not reused). Upstream absorbs former local 0016 (PSH tail),
0017 (loss-test helpers), 0018-0022 (KCC), 0024 (pool counts), and 0030
(duplicate-ACK classification). Their local patch files are removed; this
rebase introduces no new KCC policy, pool sizing, bridge or GSO defaults.

Upstream owns OOO frontier reclamation. Residual 0013 retains
`drained_gap |= TrimOooToFrontier()` for the local ACK integration and the
rejected receive-callback/resume scenario. Patches 0014/0015 preserve
backpressured OOO and in-order frontier data. Patch 0031 preserves both
upstream CLOSE-WAIT notifications while restoring FIN-first ordering after
0005; its focused regression covers 16 callback/close/wrap scenarios.
Patch 0034 retains nine local-semantic test adaptations (including the
`test_ecn_cc` KCC ECE contract). Patch 0027 only
needs a context repair during this rebase; its behavior is unchanged.
Historical revisions, binary hashes and measurements below retain their
original provenance. See the separately dated migration record in
[the Chinese integration notes](XTCP_INTEGRATION_CN.md), section 11.185.

## Direct Bridge Policy (2026-10-18)

For XTCP sessions on Linux desktop/server builds (`__linux__`, excluding
Android), an unset or empty `OPENPPP2_XTCP_MEMORY_BRIDGE` now tries the direct
memory bridge. Exact `1` enables it on every platform; `0` or any other
non-empty value selects the legacy socket pump. Windows, Android, macOS and
other platforms retain their opt-in default: unset/empty disables the bridge.
The process caches this choice on first use; set the variable before startup.

The existing carrier capability gate still applies: carriers without real
half-close support, including WebSocket/TLS, fall back to the socket pump.
This does not enable the XTCP stack itself. The controlled datapath acceptance
contract continues to pin `=1`; the Linux matrix runner's default leaves the
variable unset, and its explicit bridge flags set `1` or `0`.
The synthetic `xtcp_runtime_bridge_test` throughput switch documented below
remains explicit and is separate from this production policy.

### Experimental Runtime Execution Context

On Linux, set `OPENPPP2_XTCP_PRIVATE_IO=1` before startup to run even a single XTCP
shard on its own `io_context` and one `xtcp-io` worker. Unset, empty, and other
values preserve the supplied context for one shard. Multiple shards already
use private workers. This experimental switch changes execution placement,
not shard count, output lanes, carrier encryption, or gather policy. Its one
queue effect: with one shard, TAP GSO active, and
`OPENPPP2_XTCP_GLOBAL_QUEUE_BYTES` unset, the shared upload budget defaults to
2 MiB instead of 32 MiB. The worker otherwise
ACKs upload data that the main thread has not yet encrypted, so the sender's
window outruns carrier progress and overflows the TUN queue (P64 upload: 10k-40k
TUN drops, 100k-220k retransmits). With 2 MiB, P64 GSO-on upload measured
+10-15% with TUN drops two orders lower and P16 unchanged; 1 MiB slowed P16.
With TAP GSO off the main thread drains 1500-byte packets more slowly, and
2 MiB cut P64 upload from about 720 to 340 Mbit/s, so GSO-off keeps 32 MiB.
An explicit `OPENPPP2_XTCP_GLOBAL_QUEUE_BYTES` always wins.
It adds a CPU execution domain and does not establish single-core performance.
Keep the setting fixed for the lifetime of a runtime instance. No default
change or throughput qualification is implied.
Other platforms ignore this switch. This experimental execution context and
its queue-budget default, together with the direct bridge test stability fix,
first ship with release v2.1.9.0.

## Windows

Prepare the pinned source, then build with the repository's CMake/Ninja
wrapper from a Visual Studio developer shell:

```bat
bash tools/prepare_xtcp.sh
build_windows.bat Release x64 xtcp
```

For a Visual Studio project build, build the dependency separately and pass its
library path to MSBuild:

```bat
cmake -B build-xtcp-win -S third-party/xtcp -G "Visual Studio 17 2022" -A x64 "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>" -DXTCP_BUILD_TESTS=OFF -DXTCP_BUILD_BENCH=OFF -DXTCP_BUILD_SAMPLES=OFF -DXTCP_BUILD_PLUGINS=OFF -DXTCP_BUILD_LWIP=OFF -DXTCP_BUILD_CC_PLUGINS=ON -DXTCP_CHECKSUM_VALIDATE=ON
cmake --build build-xtcp-win --config Release --target xtcp_static
MSBuild ppp.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:VcpkgTriplet=x64-windows-static /p:PppEnableXtcp=true /p:PppXtcpLibrary=build-xtcp-win\Release\xtcp_static.lib /p:OutDir=build\msbuild-xtcp\ /p:IntDir=build\msbuild-xtcp-int\ /m:2
```

`PppXtcpLibrary` can be an absolute path when the MSBuild working directory is
not the repository root. Windows XTCP currently requires the Wintun L3 adapter;
the legacy TapWindows L2 adapter is rejected with a logged explanation.

The MSVC CMake path explicitly uses `/utf-8` and `NOMINMAX`, matching the
vcxproj. These prevent local code pages from misreading Chinese comments and
Windows min/max macros from corrupting Boost.UUID declarations.

### Windows data path and offline validation (2026-10-17)

Windows XTCP is x64-only and uses Wintun L3 plus an internal loopback TCP
bridge. Wintun supplies raw IP packets, without Ethernet headers. The current
XTCP ingress accepts only unfragmented IPv4 TCP; IPv6, UDP and fragmented IPv4
are outside XTCP support. Wintun does not advertise TX GSO, so neither TSO nor
partial-checksum output can be enabled by the diagnostic environment gates;
XTCP emits normal MSS-sized packets with checksums through `Output`.

The bridge binds `127.0.0.1:0`, obtains its actual ephemeral source port and
registers that port with the external-client table before connecting to
`127.0.0.1:<local-listener-port>`. Windows rejects a non-IPv4/non-loopback
listener. This local bridge does not need physical-interface binding or remote
socket protection: such binding would interfere with loopback. Remote carrier
and bypass connections still use the existing forwarding/protection paths.
This is a source-level conclusion; route interaction has not been tested live.

With more than one shard, XTCP owns its I/O workers. Windows uses the existing
thread-name helper; no CPU affinity is imposed. Teardown runs on each owning
strand, cancels timers and closes flows before releasing the pool lease. The
workers must drain cancelled handlers before exit, and destruction on a worker
must avoid joining itself. Stop checks running state under the startup mutex
so repeated Stop/destruction cannot post teardown into an exited pool. These lifecycle rules also correct the shared
implementation on Linux without changing its packet or bridge selection.
`fcntl`, `socketpair`, POSIX `close`, and pthread naming remain behind platform
guards. The remaining `getenv` calls use the portable CRT API with exact value
checks; set diagnostics before startup and do not mutate the process environment
concurrently with runtime startup.

The Windows lifecycle regression is opt-in and links the production library:

```bat
rem In an x64 VS developer shell, with the provisioned x64-windows-static triplet:
cmake -S . -B build\ninja-xtcp-validation -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static "-DPPP_OUTPUT_DIR=%CD%/build/ninja-xtcp-validation/bin" -DENABLE_XTCP=ON -DENABLE_XTCP_WINDOWS_TESTS=ON
cmake --build build\ninja-xtcp-validation --parallel 2
ctest --test-dir build\ninja-xtcp-validation -R xtcp_windows_lifecycle_test --output-on-failure
```

Replace dependency/output paths with local paths; keep output separate from any
active installation. This is equivalent to the wrapper's Release/x64/XTCP
configuration, while retaining an isolated build directory. The test performs
16 two-shard startup/stop/release cycles, alternating immediate destruction
with delayed repeated Stop, without adapters or network traffic.
Production publishes a runtime only after Start succeeds and creates a fresh
instance on reconnect. This change does not extend the existing boundaries for
concurrent Start/Stop on one instance or changing shard count between restarts.
The token checks Impl release; it does not claim that the detached current
worker has synchronously exited, since that worker still owns its context.
The standalone `tests/cpp` XTCP suite is still Linux-only; this focused target
does not claim to port that suite.

The missing `pwsh.exe` message comes from vcpkg's imported
`AppLocalFromInstalled` target (`scripts/buildsystems/msbuild/vcpkg.targets`),
not `CopyDriverRuntime`. The installed target already retries the system
Windows PowerShell when pwsh exits with 9009. No repository fallback or external
vcpkg modification is needed. Local duplicate-vcpkg-import warnings are separate
from XTCP compiler warnings.

Live validation still requires explicit authorization: Wintun packet/MTU/checksum
handling, bidirectional bridge traffic and half-close/RST, loopback under tunnel
routes, reconnect and multi-shard exit. No PPP application, route/DNS/adapter,
driver or proxy changes are part of the offline validation.

Validation on 2026-10-17: MSBuild Release/x64 with XTCP enabled and the default
XTCP-disabled build both passed (exit 0). The equivalent isolated VS x64
CMake/Ninja build passed all 342 steps (exit 0). The production-linked Windows
lifecycle test passed 1/1 (16 internal cycles, 0.49 seconds). Include boundaries,
vcxproj source-list matching (259/259), and `git diff --check` passed. No new
XTCP compiler warnings remain, including C4018; existing C4005/C4099,
local MSB4011 and CMake compatibility/policy warnings remain. Linux runtime
regressions were not run in this Windows-focused pass. No live VPN session
was started or validated.

### Windows loopback regression (2026-10-17)

The opt-in `ENABLE_XTCP_WINDOWS_TESTS` build also includes
`xtcp_runtime_bridge_test`, linked to the production library. It injects
synthetic IPv4/TCP packets into XtcpRuntime and uses a real ephemeral-port
`127.0.0.1` TCP echo server; no PPP application or Wintun adapter is started.
Windows runs the TCP bridge and excludes only the POSIX descriptor-ownership
case. Its environment helpers and Asio work guard use Windows-compatible APIs;
the POSIX relay and Linux execution behavior are preserved.

```bat
cmake --build build\ninja-xtcp-validation --target xtcp_runtime_bridge_test xtcp_windows_lifecycle_test --parallel 2
ctest --test-dir build\ninja-xtcp-validation -R "^xtcp_(runtime_bridge|windows_lifecycle)_test$" --output-on-failure
```

MSVC compilation/linking passed (exit 0). CTest passed 2/2: lifecycle 0.48 s,
bridge 12.84 s, total 13.32 s. Bridge coverage includes endpoint byte order,
handshake, byte-exact bidirectional data, half-close, RST before readiness,
direct-read reservations, backpressure/resume, stale callbacks, restart/global
upload budget, FIN idempotence, queue accounting and 256 connect/close cycles.
These are functional test durations, not throughput measurements.

The user authorized isolated tests only, preserving existing connections. No
usable Windows Sandbox or Windows VM tooling/inventory was found; none was
installed or enabled. Live PPP/Wintun native-versus-XTCP performance remains
unmeasured. Comparing this synthetic packet harness directly with ordinary
Winsock loopback would use different workloads and cannot establish VPN speed.

### Windows loopback throughput (2026-10-17)

Run the optional benchmark after building `xtcp_runtime_bridge_test`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run_windows_xtcp_loopback_benchmark.ps1 -Seconds 3 -Rounds 3
```

The script launches only the test executable, alternates reference/XTCP order,
clears XTCP overrides in each child process and saves samples, stderr, binary
SHA-256 and medians. Each connection has a 1-second warmup followed by a
3-second measurement window. Setup and final integrity drain are excluded.
Both paths use a 32 KiB application flight limit, 1360-byte write chunks and
the same deterministic payload; reported goodput counts echoed payload once,
not the sum of upload and download. XTCP uses KCC, one shard, no GSO, and an
explicit synthetic peer MSS of 1460. Winsock negotiates its own loopback MSS.

| Component path | Three runs, Mbps | Median, Mbps | Median process CPU ns/B |
| --- | --- | --- | --- |
| Winsock TCP loopback reference | 779.381 / 1023.176 / 967.785 | 967.785 | 14.208 |
| XTCP Runtime + synthetic IPv4/TCP + loopback echo | 22.634 / 20.480 / 19.486 | 20.480 | 417.082 |

All six samples passed byte-exact echo and full-drain checks, with zero XTCP
entry-admission drops. XTCP emitted 536/532/559 complete old payload packets
per connection (including warmup/drain), which were counted as duplicates and
excluded from goodput. Their cause is unproven. The earlier no-MSS diagnostic
(default peer MSS 536) measured 34.607 Mbps XTCP / 1018.449 Mbps Winsock medians;
that is a different profile and is not pooled into this result.

**XTCP is substantially slower in this harness.** The reference is ordinary
Winsock TCP, not OpenPPP2 native mode. XTCP additionally performs synthetic
packet construction/checksums, queueing, output copies, parsing and manual
ACK driving; process CPU includes those costs, the echo server and polling.
The small application window also constrains the closed-loop test. These
measurements cannot establish a PPP/Wintun speed ratio or identify whether
pacing, timer scheduling, retransmissions or harness work dominates. No PPP,
Wintun, routes, DNS or adapter settings were changed.

Results: `build/xtcp-validation/throughput-mss1460/results.json` (binary SHA-256
`7c021c03bacb4ff3d4e7b0b862336781cafe916818155ff30197267f4c1943bc`).
The subsequent functional regression passed 2/2 in 13.49 seconds. The Windows
harness now waits for runtime teardown before stopping its external context,
preventing the test itself from retaining pending Stop handlers.

### Windows throughput follow-up: root causes and parity (2026-10-17)

The 20 Mbps result above had four compounding causes, found with an
in-process stack sampler (`XTCP_BENCH_PROFILE=io|main`) and per-thread CPU
accounting (`XTCP_BENCH_THREADS=1`) added to the benchmark:

1. **`getenv` on the hot path.** `ConnectorWriteCap()`, `ConnectorBatchBytes()`,
   `GlobalQueueBudget()`, `SendRetryDelay()` and the GRO size helper called
   `getenv` per write/packet. The Windows CRT locks and scans the environment
   block, which cost about 36% of the runtime I/O thread. These values are now
   read once when the runtime is constructed (a new runtime still picks up
   changed variables). This helps every platform.
2. **Data segments counted as duplicate ACKs** (upstream `0255dae`, formerly
   local `0030-dupack-excludes-data-segments.patch`). On a bidirectional flow every
   peer data segment repeats the cumulative ACK; xtcp counted them as dupacks,
   triggering spurious fast retransmits (the "duplicate packets") and cwnd
   collapse. Data/SYN/FIN segments are no longer counted (RFC 5681 section 2,
   Linux `FLAG_DATA`). A window change alone is intentionally not excluded,
   because xtcp's own receiver shrinks its window per buffered out-of-order
   segment.
3. **KCC on coarse Windows timers.** KCC send paths are timer driven; Windows
   waits round up to about 15.6 ms. Windows now defaults to `cubic`;
   `OPENPPP2_XTCP_CC=kcc` restores KCC. Patch
   `0029-pacing-late-wake-catch-up.patch` credits late pacing wakes (bounded to
   20 ms); it showed no measurable change by itself in this harness.
4. **Harness confound.** The echo server shared the runtime I/O thread, while the
   Winsock reference gives it its own thread. The benchmark now runs the echo
   server on a separate thread.

Benchmark script, 3 interleaved rounds, default CC (cubic):

| Component path | Median Mbps | Range Mbps | Median CPU ns/B | Duplicates |
| --- | --- | --- | --- | --- |
| Winsock TCP loopback reference | 1061.767 | 1005.985-1076.121 | 13.343 | 0 |
| XTCP Runtime + synthetic IPv4/TCP + loopback echo | 1096.883 | 1074.658-1110.883 | 18.791 | 0 |

With `XTCP_BENCH_CC=kcc` the same build measured 309-400 Mbps. XTCP throughput
now matches the reference in this closed-loop harness; it still spends about
40% more process CPU per byte. This remains a component benchmark, not a
PPP/Wintun VPN measurement.

### Linux loopback throughput and direct bridge (2026-10-17)

`xtcp_runtime_bridge_test --throughput xtcp|native <seconds>` now also builds
on Linux (`ENABLE_XTCP_TESTS=ON`). `native` is the kernel TCP loopback there.
`OPENPPP2_XTCP_MEMORY_BRIDGE=1` (or `XTCP_BENCH_DIRECT=1`) replaces the
loopback-TCP echo with an in-process direct second leg that echoes through
`SendToPeer` / `OnSecondLegPayload` from its own pump thread.
`XTCP_BENCH_WINDOW` changes the 32 KiB flight limit and `XTCP_BENCH_PROFILE`
also works on Linux (signal sampler plus `addr2line`; build with `-g`).

Runtime changes from this pass: connector writes and poll-timer kicks are
deferred to the end of each ingress batch (one gather-write and one timer
re-arm per batch), and `XtcpSecondLegHooks` gained an opt-in
`BatchesPeerSend()` / `FlushPeerSend()` pair so a second leg can wake its
writer once per batch instead of once per segment. The production
`VEthernetNetworkTcpipConnection` second leg has not opted in yet.

WSL2 Debian, 32 KiB window, single connection, 3 runs each:

| Path | Mbps |
| --- | --- |
| Kernel TCP loopback reference | 7186-9561 |
| XTCP, loopback TCP bridge (before batching) | 951-1199 |
| XTCP, loopback TCP bridge (batched writes) | 1305-1389 |
| XTCP, direct bridge, per-segment wake | 1496-1543 |
| XTCP, direct bridge, `FlushPeerSend` batching | 1878-1987 |

On Windows the same build measured about 1000 Mbps Winsock, 1130 Mbps TCP
bridge and 1240-1250 Mbps direct bridge. On Linux XTCP reaches about 22% of
the kernel loopback. The runtime I/O thread is saturated; the remaining cost
is cross-thread wake-ups on every small batch plus per-1360-byte userspace
segment processing on one thread. Closing the gap needs larger units of work
(GSO/TSO-sized segments) or a multi-thread pipeline, not further local fixes.
With a 64 KB window XTCP reaches about 1.84 Gbps over the TCP bridge; larger
windows are capped because the synthetic peer does not negotiate window
scaling.

### Linux per-shard GSO output lanes and default segment cap (2026-10-18)

With `OPENPPP2_XTCP_SHARDS` above 1, every shard used to push its TUN output
through the single `TunGsoCoalescer` in `TapLinux`. That coalescer sits behind
one mutex and writes one queue fd. Shards serialized on the lock, and flows
from different shards interleaved in one merge buffer, which forced early
flushes. Two shards with GSO on reached only about 900 Mbps at P64 DL.

Changes:

- `ppp/tap/TapOutputLane.h` adds a thread-local lane hint. When there is
  more than one shard, `XtcpRuntime` wraps shard i's output handlers in
  `TapOutputLaneScope(i)`.
- On first use of a lane, `TapLinux` opens a dedicated `IFF_MULTI_QUEUE` queue
  fd for it, with its own coalescer, hold timer and read loop. Lanes share no
  lock. Each queue fd needs a reader because the kernel's tun automq steers a
  flow's return packets to the queue that last wrote that flow.
- If a lane cannot be created, only that shard falls back to the shared path.
  When the TAP switches to SSMT or fails, every lane is flushed and merging
  stops. An existing lane then writes unmerged frames on its own fd, so a flow
  never changes queue.
- `OPENPPP2_TAP_GSO_LANES_DISABLE=1` forces the shared path for comparison.
- M3 opt-in switches (default off; unset keeps the behaviour above):
  `OPENPPP2_XTCP_DIRECT_SPSC=1` moves the direct-upload data queue to a
  fixed 4096-slot SPSC handoff (same caps and gather; ring allocation
  failure keeps the relay path). `OPENPPP2_XTCP_AFFINITY=queues` with
  `OPENPPP2_XTCP_SHARDS>1` and an exact `OPENPPP2_XTCP_IO_CPUS` map gives each
  shard its own io_context and pinned worker, binds GSO lane i to shard i,
  and stages lane reads (1024 items / 8 MiB) back to the TAP context for the
  unchanged `OnInput`; it is rejected with SSMT and reported as unsupported
  for one shard. `OPENPPP2_XTCP_AFFINITY_STRICT=1` refuses to start when a
  pin fails.
- The default `TunGsoCoalescer::kSegmentCap` is now 32 instead of 4.
  `OPENPPP2_TAP_GSO_SEGMENTS` (1-48) still overrides it, and
  `OPENPPP2_XTCP_DL_GSO_PERF_PROFILE=1` still selects 48 when no explicit cap
  is set. The production-builder contract in `tun_gso_output_probe` pins
  cap 4.

WSL2 Debian, three isolated netns, XTCP, 2 rounds of 15 s plus 2 s per cell,
median Mbps:

| P / direction | 1 shard GSO off | 1 shard cap 4 | 1 shard cap 32 | 1 shard cap 48 | 2 shards GSO off | 2 shards cap 4 | 2 shards cap 32 | 2 shards cap 48 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| P1 DL | 488 | 497 | 497 | 494 | 389 | 316 | 390 | 402 |
| P1 UL | 724 | 1610 | 1630 | 1533 | 1433 | 1370 | 1350 | 1352 |
| P16 DL | 1761 | 2112 | 3129 | 3074 | 3292 | 1755 | 4919 | 4725 |
| P16 UL | 988 | 2124 | 2022 | 1718 | 1567 | 2094 | 2011 | 2010 |
| P64 DL | 1713 | 2472 | 3005 | 2957 | 3766 | 1963 | 4571 | 4630 |
| P64 UL | 697 | 1786 | 1852 | 1823 | 1467 | 1918 | 1905 | 1871 |

- Same binary, 2 shards, cap 4, P64 DL: 915 Mbps with lanes disabled, 1983
  Mbps with lanes.
- Caps 32 and 48 perform alike on downloads, and both let two shards with GSO
  on beat GSO off at P16 and P64 DL. Cap 48 was lower for one shard at P16 UL
  (1718 vs 2022), so the default is 32. Most of the gain from cap 4 to 32 is
  on downloads; uploads are about flat.
- Upload retransmits are far lower with GSO on at every cap than with GSO off
  (for example, one shard at P64 UL averaged 47790 with GSO off and 4219 at
  cap 32). The download iperf results carry no retransmit count.
- `XTCP-GSO-STALL-001` (P16 DL, GSO on, cap 48, 10 s plus 2 s, 42 s watchdog)
  passed 20 of 20 rounds with one shard and with two shards (median 2928 and
  4775 Mbps), with no watchdog trips. That run was not repeated at cap 32.
- C++ unit tests: 174/174 pass.
- Not verified: lanes together with the `--stall-diagnostics` ledger, lanes
  with NDI TSO, and latency distribution. These are directional results from
  a shared WSL2 host with only 2 rounds per cell.

### Future AF_UNIX option (assessment only)

Windows 10 1803+ AF_UNIX stream sockets are a possible follow-up, but cannot
reuse the POSIX `socketpair` branch directly: Winsock has no `socketpair`, and
Asio's Windows `local::connect_pair` is unsupported. A Windows implementation
would need bind/listen/connect/accept, filesystem-name/permission cleanup,
Winsock `SOCKET` ownership without truncation to `int`, and an appropriate Asio
local/generic stream socket rather than assigning AF_UNIX to a TCP socket.
Retain loopback TCP and runtime fallback until those paths are tested. No
performance improvement has been measured. See [Microsoft's AF_UNIX notes](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/)
and [Asio local-socket documentation](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/posix/local.html).

## Android

Set `PPP_ENABLE_XTCP=ON` when invoking the Android build script. It prepares the
pinned source before configuring the NDK build:

```sh
cd android
PPP_ENABLE_XTCP=ON NDK_ROOT=/path/to/android/ndk ./build.sh arm64
```

XTCP ingress uses Android's Linux `TapLinux` packet path. Its bridge socket is
only a loopback connection to the in-process TCP stack. Remote TCP connections
are established by the existing client forwarding layer, which calls the
`VpnService.protect` bridge before connecting.

Both Windows and Android default to XTCP disabled. Linux desktop behavior and
its existing default are unchanged.
