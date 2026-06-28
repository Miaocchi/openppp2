# Testing

OpenPPP2 has unit tests for C++, Guardian Go logic, Flutter UI helpers, and iOS shared logic. GitHub Actions workflow **Test - Unit** runs these suites on pull requests to `main`.

## RED/GREEN Policy

1. Write or preserve a failing regression test first.
2. Implement or fix until it passes.
3. Record the break recipe in `tests/red-manifest/` for new regression suites.

At least 40% of cases per suite should be negative, boundary, or regression tests when adding new coverage.

## C++

Top-level project tests:

```bash
cmake -B build-test -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Standalone regression tests:

```bash
scripts/run-cpp-tests.sh
scripts/run-cpp-coverage.sh
```

## Go Guardian

```bash
cd go/guardian
go test ./...
```

## Flutter

```bash
cd android
flutter test
```

## iOS Logic

```bash
cd ios/App
./run-tests.sh
```

## Coverage

Top-level LLVM coverage:

```bash
chmod +x scripts/coverage.sh
./scripts/coverage.sh
```

Standalone C++ coverage writes `build/coverage/summary.txt`:

```bash
scripts/run-cpp-coverage.sh
```

## Smoke Test

Requires a built `bin/ppp` binary and a reachable server in the config:

```bash
chmod +x tools/proxy_mode_smoke.sh
PPP_BIN=./bin/ppp CONFIG=./appsettings.json ./tools/proxy_mode_smoke.sh
```

## Test Layout

```text
tests/
  CMakeLists.txt
  unit/
    test_application_mode.cpp
    test_tap_stub.cpp
    test_proxy_defaults.cpp
  cpp/
    base64_test.cpp
    dns_buffer_test.cpp
    p2p_replay_window_test.cpp
  red-manifest/
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTS` | OFF | Build top-level `openppp2_tests` and register CTest targets |
| `ENABLE_COVERAGE` | OFF | Add LLVM coverage flags where supported |

Do not combine `ENABLE_COVERAGE` with `ENABLE_ASAN` in the same build.
