# Testing
> Status: Active
> Type: Development guide
> Last verified: 2026-07-24
>
> **Purpose:** Select and run the test entry point that actually covers the changed surface.
> **Audience:** Contributors and CI maintainers.
> **Parent index:** [Development](README.md) · **Chinese:** [测试](TESTING_CN.md)

## Choose the correct C++ test boundary

The repository has two distinct C++ test paths. Do not describe one as a shortcut for the other.

| Path | What it builds | When to use it |
|---|---|---|
| `tests/cpp` | Focused standalone executables registered with CTest | Fast unit/regression work without configuring the full native runtime. |
| Root CMake + `ENABLE_TESTS=ON` | `ppp`, `openppp2_lib`, GTest-based `openppp2_tests`, and `openppp2_dns_cache_ttl_tests` | Changes that need the root native dependency graph or root-linked test behavior. |

### Standalone C++ suite

The script configures `tests/cpp` with Ninja, builds it, and runs CTest:

```bash
scripts/run-cpp-tests.sh
```

That project requires CMake 3.16 or newer, a C++17-capable compiler, OpenSSL, and Ninja because the script selects it explicitly. It writes its build tree to `build/test`.

### Focused P2P V2 Isolation

Run the fixed suite from the repository root; each mode uses its own build
directory and also compiles the real application wiring:

```sh
sh scripts/run-p2p-v2-isolation.sh normal
sh scripts/run-p2p-v2-isolation.sh asan
sh scripts/run-p2p-v2-isolation.sh tsan
```

Set `OPENPPP2_P2P_BUILD_DIR` to a separate directory when existing build
artifacts must be preserved. `CXX` selects the compiler and
`OPENPPP2_BUILD_JOBS` controls build parallelism.

Configure `tests/cpp` in a separate build directory and build the `p2p_*`
targets. The v2 targets include `p2p_v2_offer_test`, `p2p_v2_channel_test`,
`p2p_v2_noise_test`, `p2p_v2_integration_test`, `p2p_probe_coordinator_test`,
`p2p_ingress_limiter_test`, `p2p_stun_gatherer_test` and
`p2p_native_socket_stun_test`. The fixed suite additionally includes
`p2p_information_message_test` and `p2p_v2_server_coordination_test`, the latter
testing the production coordination helper for concurrent renew, bilateral
activation, cancellation, stale callbacks and conservative predecessor recovery.
`p2p_v2_app_wiring_compile` compiles the actual
client, server and carrier wiring without linking or starting PPP.

Use separate `-DENABLE_SANITIZERS=ON` and `-DENABLE_TSAN=ON` trees. Native
socket tests use only unprivileged loopback UDP; a sandbox may require
permission to open those sockets. Never run PPP or privileged netns for this
suite. The integration test uses real offer/channel/codec and IPv4 parsing,
with simulated NAT/relay/clock, including 180 seconds of rotation, 100
connect/refresh/stop cycles, migration, nonce exhaustion and lost CommitACK.
That integration target does not instantiate full Exchanger automatic socket recovery.
The separate root-linked recovery target below uses the actual Exchanger. These
checks do not establish Android device behavior, real NAT traversal or Linux
SO_MARK bypass of TUN. Keep the production gate disabled.

On 2026-10-05 the ordinary P2P and existing Noise regression suite passed 30 targets, including
separate unprivileged loopback runs for socket-restricted tests. Both existing
Noise handshake/exporter targets also passed. The final focused suite
passed 10 targets and 80 cases under ASan/UBSan/LSan and separately under TSan;
the integration target contains 16 cases, including the 180-second and
100-cycle scenarios. The actual application wiring object target also compiled.
The final channel tests cover large authenticated numeric RX sequence advances
without wrap, duplicate delivery or receive-window mutation on failed AEAD.
The associated tooling suite passed 46 tests.
These focused results concern isolated Linux checks. The subsequent root
Linux Release build completed all 283 compilation/link actions and was
incrementally relinked after the recovery fix. The final production build has
no recovery-test macro and keeps the gate closed. This does not establish
Windows MSBuild, Android SDK/device tests or live network acceptance.

### Root CMake tests

For actual Exchanger recovery, prepare the root native dependency layout and
run the dedicated script. Pass the dependency root as a CMake option; no
dependencies are installed by this script:

```sh
sh scripts/run-p2p-v2-exchanger-recovery.sh normal -DTHIRD_PARTY_LIBRARY_DIR=/path/to/native-deps
sh scripts/run-p2p-v2-exchanger-recovery.sh asan -DTHIRD_PARTY_LIBRARY_DIR=/path/to/native-deps
sh scripts/run-p2p-v2-exchanger-recovery.sh tsan -DTHIRD_PARTY_LIBRARY_DIR=/path/to/native-deps
```

Each mode builds the actual `openppp2_lib` in a separate directory and runs only
`p2p_v2_exchanger_recovery_test`, without starting PPP. Override
`OPENPPP2_P2P_RECOVERY_BUILD_DIR`, `CC`, `CXX` and `OPENPPP2_BUILD_JOBS` as needed.
The root option `ENABLE_P2P_RECOVERY_TESTS` defaults to OFF. Its compile-gated
dependency hooks require explicit test capability; the production gate stays
closed. The test exercises socket failure, retry backoff, registration failure,
scheduling failure, stale callbacks and suspended coroutines, authenticated v2
reconnection, v1 rejection and FRP maintenance through the actual Update method.
Root ASan/UBSan and TSan builds must use separate directories.

On 2026-10-05 all nine actual Exchanger cases passed in normal,
ASan/UBSan/LSan and separate TSan builds of the full library. The script was
repeated successfully in each mode. LSan required permission for isolated
process inspection in this sandbox. Registration, transport and relay output
are simulated dependencies; native socket protection, real INFO delivery,
NAT traversal and device behavior remain unverified. Recovery failure without
a ready transport now schedules its retry without sending a renewal.

Prepare the root dependency layout first, then configure the root project with `ENABLE_TESTS`:

```bash
cmake -S . -B build/root-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DTHIRD_PARTY_LIBRARY_DIR=third-party
cmake --build build/root-tests
ctest --test-dir build/root-tests --output-on-failure
```

`ENABLE_TESTS` defaults to `OFF`. When enabled, `tests/CMakeLists.txt` fetches GoogleTest v1.14.0, so a first configuration may need network access in addition to the root native dependencies. This is separate from `tests/cpp`.

## Coverage and sanitizers

### Focused LLVM coverage

```bash
scripts/run-cpp-coverage.sh
```

The script configures `tests/cpp` with coverage enabled, builds and runs only `p2p_replay_window_test`, `dns_buffer_test`, and `base64_test`, then writes `build/coverage/summary.txt`. It requires `llvm-profdata` and `llvm-cov` in addition to the standalone-suite prerequisites.

### Root runtime coverage

```bash
THIRD_PARTY_LIBRARY_DIR=third-party scripts/coverage.sh
```

This script configures the root project with Clang, `ENABLE_TESTS=ON`, and `ENABLE_COVERAGE=ON`; it therefore requires the full native dependency layout. Root coverage instrumentation is not supported with MSVC.

### Lifecycle sanitizer targets

```bash
scripts/run-lifecycle-sanitizers.sh
```

The script configures the standalone C++ project with `ENABLE_SANITIZERS=ON`, defaults to `clang++`, and builds/runs five named lifecycle, route, and DNS targets. Use it for lifecycle-sensitive changes; it is not a build of the complete native executable.

### ThreadSanitizer

```bash
CXX=clang++ scripts/run-cpp-tsan-tests.sh
```

The script configures the complete standalone C++ suite in `build/test-tsan` with `ENABLE_TSAN=ON`, then builds and runs every registered CTest target. TSan is mutually exclusive with `ENABLE_SANITIZERS` (ASan/UBSan), requires a compiler with a working ThreadSanitizer runtime, and does not build the root native executable. Use `CXX=g++` when the local Clang TSan runtime is unavailable. Every standalone test registered through `openppp2_add_cpp_test()` has a 120-second watchdog so a deadlock fails instead of hanging CI.

Key deterministic concurrency tests in this batch are `yield_context_test`, `spinlock_test`, `asynchronous_write_io_queue_test`, `dns_udp_relay_test`, `client_datagram_port_manager_test`, `protector_network_request_test`, `transmission_qos_concurrency_test`, `vdns_request_lifecycle_test`, `mapping_port_connect_reentrancy_test`, and `dns_controller_test`.

## Runtime contract prerequisite

`bash scripts/test-runtime-contract.sh cpp` assumes that `build/test` already exists. Configure the standalone project first; the script then builds and runs only `runtime_snapshot_test` after checking the shared fixture hashes.

```bash
cmake -S tests/cpp -B build/test -G Ninja
bash scripts/test-runtime-contract.sh cpp
```

## Other checked-in component tests

Run these from the stated directories when changing those components:

```bash
# Guardian Go package
( cd go/guardian && go test ./... )

# Go manager/backend package checks used by CI
( cd go && go vet ./ppp/... && go test ./ppp/... )

# Android Flutter tests
( cd android && flutter pub get && flutter test )

# iOS logic tests
( cd ios/App && ./run-tests.sh )

# Experimental Desktop Client frontend and Rust shell
( cd desktop/client && npm ci && npm test && npm run build \
  && cargo test --manifest-path src-tauri/Cargo.toml )
```

Some commands require their platform SDK/toolchain; do not treat a missing local SDK as a runtime failure.

## Root CMake test-related options

All of these options default to `OFF`:

| Option | Effect |
|---|---|
| `ENABLE_TESTS` | Enables CTest and adds the root GTest/DNS test subdirectory. |
| `ENABLE_COVERAGE` | Adds LLVM coverage instrumentation on non-MSVC toolchains. |
| `ENABLE_VMUX_CHURN_TEST` | Builds and registers the root-linked VMUX carrier-churn integration test. |
| `ENABLE_VMUX_RECEIVE_SEMANTICS_TEST` | Builds and registers the root-linked VMUX receive-semantics test. |
| `ENABLE_ASAN`, `ENABLE_UBSAN` | Enable diagnostic sanitizer flags in the root project; not production build modes. |

## CI coverage today

`.github/workflows/test.yml` is the primary unit-test workflow. It runs the standalone C++/coverage path, a separate full-suite ThreadSanitizer job, lifecycle sanitizers, Guardian and Go checks, Flutter tests, and iOS logic tests. It does **not** run the experimental Desktop Client's npm or Cargo test commands.

Native build workflows are separate from that unit workflow. Read the relevant workflow before calling a platform or feature combination CI-covered.
