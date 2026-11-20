#!/bin/bash

# Copyright  : Copyright (C) 2017 ~ 2035 SupersocksR ORG. All rights reserved.
# Build the seven non-baseline Linux amd64 packages with the root CMake project.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
third_party=${1:-${THIRD_PARTY_LIBRARY_DIR:-/root/dev}}
if [[ $(uname -sm) != 'Linux x86_64' ]]; then
    echo 'This script builds native Linux amd64 packages.' >&2
    exit 1
fi
third_party=$(cd -- "$third_party" && pwd)

mkdir -p "$root/bin"
work=$(mktemp -d "$root/bin/.linux-amd64.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

for suffix in io-uring io-uring-simd simd tc tc-io-uring tc-io-uring-simd tc-simd; do
    sysnat=OFF uring=OFF simd=OFF
    if [[ $suffix == tc* ]]; then sysnat=ON; fi
    if [[ $suffix == *io-uring* ]]; then uring=ON; fi
    if [[ $suffix == *simd ]]; then simd=ON; fi

    build="$work/$suffix"
    archive="openppp2-linux-amd64-$suffix.zip"
    echo "Building $archive"
    # The old local -simd packages enabled AVX2 as well as AES-NI.
    cmake -S "$root" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTHIRD_PARTY_LIBRARY_DIR="$third_party" \
        -DPPP_OUTPUT_DIR="$build/bin" \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE="$build/bin" \
        -DENABLE_SYSNAT="$sysnat" -DENABLE_IO_URING="$uring" \
        -DENABLE_SIMD="$simd" -DENABLE_SSEA_SIMD="$simd"
    cmake --build "$build" --config Release --target ppp --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
    zip -qj "$work/$archive" "$build/bin/ppp"
    mv -f -- "$work/$archive" "$root/bin/$archive"
    rm -rf -- "$build"
done
