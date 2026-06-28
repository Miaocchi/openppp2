# Testing

OpenPPP2 uses **GoogleTest** for unit tests and **LLVM source coverage** for proxy-only path instrumentation.

## Build tests

```bash
cmake -B build-test -DENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test
cd build-test && ctest --output-on-failure
```

On Linux CI or coverage builds, use Clang and point `THIRD_PARTY_LIBRARY_DIR` at the prebuilt dependency tree (default `/root/dev`):

```bash
cmake -B build-test \
  -DENABLE_TESTS=ON \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTHIRD_PARTY_LIBRARY_DIR=/root/dev
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

## Coverage (LLVM)

```bash
chmod +x scripts/coverage.sh
./scripts/coverage.sh
```

Or manually:

```bash
cmake -B build-cov \
  -DENABLE_TESTS=ON \
  -DENABLE_COVERAGE=ON \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov
cd build-cov
export LLVM_PROFILE_FILE=default.profraw
ctest --output-on-failure
llvm-profdata merge -sparse default.profraw -o default.profdata
llvm-cov report ./tests/openppp2_tests -instr-profile=default.profdata \
  ../ppp/app/ApplicationMode.cpp ../ppp/tap/TapStub.cpp
```

**Target:** line coverage ≥ 70% on proxy-only related sources (see `scripts/coverage.sh` file list).

## Smoke test

Requires a built `bin/ppp` binary and a reachable server in the config:

```bash
chmod +x tools/proxy_mode_smoke.sh
PPP_BIN=./bin/ppp CONFIG=./appsettings.json ./tools/proxy_mode_smoke.sh
```

## Test layout

```
tests/
  CMakeLists.txt
  unit/
    test_application_mode.cpp   # --mode parsing
    test_tap_stub.cpp           # TapStub lifecycle
    test_proxy_defaults.cpp     # listener defaults
```

## CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTS` | OFF | Build `openppp2_tests` and register CTest targets |
| `ENABLE_COVERAGE` | OFF | Add LLVM `-fprofile-instr-generate` flags (Clang/GCC only) |

Do not combine `ENABLE_COVERAGE` with `ENABLE_ASAN` in the same build.
