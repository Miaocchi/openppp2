# XTCP platform build notes

> Status: Active
> Type: Design
> Last verified: 2026-10-17 (against root `CMakeLists.txt`, `ppp.vcxproj`, `android/build.sh`, `tools/run_windows_xtcp_loopback_benchmark.ps1`)

XTCP is opt-in on Windows and Android. Linux desktop retains its current
default. All platforms use the pinned source in `third-party/xtcp` and the
same production target settings from the root CMake build (revision `e79db8fd`).

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
2. **Data segments counted as duplicate ACKs** (XTCP patch
   `0030-dupack-excludes-data-segments.patch`). On a bidirectional flow every
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
