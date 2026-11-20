#!/usr/bin/env python3
"""Persistent, non-reconnecting SOCKS workload for multipath fault scenarios."""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import struct
import sys
import time
from pathlib import Path

from payload import connect


HEADER = struct.Struct("!QQ")
DEFAULT_CHUNK_BYTES = 32 * 1024


def wall_ms() -> int:
    return round(time.time() * 1000)


def chunk(index: int, size: int) -> bytes:
    if size < HEADER.size:
        raise ValueError("chunk size must fit the sequence header")
    body = hashlib.shake_256(f"openppp2-recovery-chunk-{index}".encode()).digest(size - HEADER.size)
    return HEADER.pack(index, size) + body


def run_client(target_ip: str, target_port: int, duration: float, result_path: Path,
               ready_path: Path | None, chunk_bytes: int, connect_timeout: float,
               io_timeout: float) -> int:
    started_at = wall_ms()
    sent_hash = hashlib.sha256()
    received_hash = hashlib.sha256()
    sent_bytes = 0
    received_bytes = 0
    connection_errors: list[str] = []
    connected_at = None
    ended_at = None
    eof = False
    try:
        if duration <= 0 or chunk_bytes < HEADER.size or connect_timeout <= 0 or io_timeout <= 0:
            raise ValueError("duration, timeouts, and chunk size must be positive")
        stream = connect(target_ip, target_port, connect_timeout)
        connected_at = wall_ms()
        stream.settimeout(io_timeout)
        if ready_path is not None:
            ready_path.parent.mkdir(parents=True, exist_ok=True)
            ready_path.write_text(json.dumps({"connected_at_ms": connected_at}) + "\n")
        deadline = time.monotonic() + duration
        index = 0
        with stream:
            while time.monotonic() < deadline:
                data = chunk(index, chunk_bytes)
                stream.sendall(data)
                sent_hash.update(data)
                sent_bytes += len(data)
                echoed = bytearray()
                while len(echoed) < len(data):
                    block = stream.recv(min(65536, len(data) - len(echoed)))
                    if not block:
                        raise RuntimeError(f"early EOF after {received_bytes} of {sent_bytes} bytes")
                    echoed.extend(block)
                if bytes(echoed) != data:
                    raise RuntimeError(f"echo mismatch at chunk {index}")
                received_hash.update(echoed)
                received_bytes += len(echoed)
                index += 1
            stream.shutdown(socket.SHUT_WR)
            while True:
                block = stream.recv(65536)
                if not block:
                    eof = True
                    break
                received_hash.update(block)
                received_bytes += len(block)
        ended_at = wall_ms()
        if not sent_bytes:
            raise RuntimeError("workload sent no application bytes")
        if received_bytes != sent_bytes or received_hash.digest() != sent_hash.digest():
            raise RuntimeError("application byte or SHA-256 mismatch")
        result = {
            "kind": "multipath-recovery-workload",
            "passed": True,
            "started_at_ms": started_at,
            "connected_at_ms": connected_at,
            "ended_at_ms": ended_at,
            "sent_bytes": sent_bytes,
            "received_bytes": received_bytes,
            "sent_sha256": sent_hash.hexdigest(),
            "received_sha256": received_hash.hexdigest(),
            "eof": eof,
            "connection_errors": connection_errors,
        }
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        ended_at = wall_ms()
        connection_errors.append(str(exc))
        failure = {
            "kind": "multipath-recovery-workload",
            "passed": False,
            "started_at_ms": started_at,
            "connected_at_ms": connected_at,
            "ended_at_ms": ended_at,
            "sent_bytes": sent_bytes,
            "received_bytes": received_bytes,
            "sent_sha256": sent_hash.hexdigest(),
            "received_sha256": received_hash.hexdigest(),
            "eof": eof,
            "connection_errors": connection_errors,
        }
        try:
            result_path.write_text(json.dumps(failure, indent=2) + "\n")
        except OSError:
            pass
        print(json.dumps(failure, sort_keys=True), file=sys.stderr)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("client",))
    parser.add_argument("target_ip")
    parser.add_argument("target_port", type=int)
    parser.add_argument("duration", type=float)
    parser.add_argument("result_json", type=Path)
    parser.add_argument("--ready-file", type=Path)
    parser.add_argument("--chunk-bytes", type=int, default=DEFAULT_CHUNK_BYTES)
    parser.add_argument("--connect-timeout", type=float, default=10)
    parser.add_argument("--io-timeout", type=float, default=20)
    args = parser.parse_args()
    return run_client(args.target_ip, args.target_port, args.duration, args.result_json,
                      args.ready_file, args.chunk_bytes, args.connect_timeout, args.io_timeout)


if __name__ == "__main__":
    raise SystemExit(main())
