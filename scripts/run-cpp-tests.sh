#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$ROOT/tests/cpp" -B "$ROOT/build/test" -G Ninja
cmake --build "$ROOT/build/test" --target p2p_replay_window_test dns_buffer_test base64_test
ctest --test-dir "$ROOT/build/test" --output-on-failure
