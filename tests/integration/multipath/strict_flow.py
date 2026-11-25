"""Integrity-preserving TCP workloads used by the strict multipath suite."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import socket
import struct
import sys
import threading
import time
from pathlib import Path

from payload import SOCKS_PORT, connect as connect_via_socks, exact, socks_handshake


CHUNK = 32768
IO_TIMEOUT = 120


def run_workers(items, work):
    """Run stream workers and unblock siblings when one worker fails."""
    pool = ThreadPoolExecutor(max_workers=len(items))
    futures = {pool.submit(work, item): index for index, item in enumerate(items)}
    results = [None] * len(items)
    try:
        for future in as_completed(futures):
            results[futures[future]] = future.result()
    except BaseException:
        for _, stream, *_ in items:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            stream.close()
        for future in futures:
            future.cancel()
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    pool.shutdown(wait=True)
    return results


def quick_ack(stream: socket.socket) -> None:
    option = getattr(socket, "TCP_QUICKACK", None)
    if option is not None:
        try:
            stream.setsockopt(socket.IPPROTO_TCP, option, 1)
        except OSError:
            pass


def connect(target_ip: str, target_port: int) -> socket.socket:
    stream = connect_via_socks(target_ip, target_port, IO_TIMEOUT)
    stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return stream


class StallMeter:
    def __init__(self, total_ref):
        self.total_ref = total_ref
        self.stop_event = threading.Event()
        self.max_stall_ms = 0
        self._thread = threading.Thread(target=self._sample, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self.stop_event.set()
        self._thread.join(timeout=1)

    def _sample(self):
        last = 0
        zero_ms = 0
        while not self.stop_event.wait(0.2):
            current = self.total_ref[0]
            if current == last:
                zero_ms += 200
                self.max_stall_ms = max(self.max_stall_ms, zero_ms)
            else:
                zero_ms = 0
                last = current


def payload_block(flow: int, offset: int, size: int) -> bytes:
    seed = hashlib.shake_256(f"strict/{flow}/{offset}".encode()).digest(size)
    return seed


def send_bulk(stream: socket.socket, flow: int, byte_count: int, duration: float, start_at: float) -> dict:
    digest = hashlib.sha256()
    sent = 0
    started = time.monotonic()
    body = hashlib.shake_256(f"strict-bulk/{flow}".encode()).digest(CHUNK - 8)
    timed = byte_count == 0
    deadline = start_at + duration
    burst_bytes = max(CHUNK, int(byte_count / max(duration, .2) * .2)) if duration else byte_count
    while (time.monotonic() < deadline if timed else sent < byte_count):
        if duration and not timed:
            release_at = started + (sent // burst_bytes) * .2
            time.sleep(max(0, release_at - time.monotonic()))
        size = CHUNK if timed else min(CHUNK, byte_count - sent)
        block = (struct.pack("!Q", sent) + body)[:size]
        stream.sendall(struct.pack("!H", size) + block)
        digest.update(block)
        sent += size
    stream.sendall(struct.pack("!H", 0))
    receipt = exact(stream, 40)
    expected = struct.pack("!Q", sent) + digest.digest()
    if receipt != expected:
        raise RuntimeError(f"bulk receipt mismatch for flow {flow}")
    stream.sendall(b"K")
    stream.shutdown(socket.SHUT_WR)
    eof_ok = not stream.recv(1)
    if not eof_ok:
        raise RuntimeError("bulk sender expected EOF")
    return {"flow": flow, "sent_bytes": sent, "sent_sha256": digest.hexdigest(),
            "sha256_ok": True, "bytes_match": True, "eof_ok": eof_ok,
            "send_start_monotonic_ms": int(started * 1000),
            "send_end_monotonic_ms": int(time.monotonic() * 1000),
            "max_stall_ms": 0}


def receive_bulk(stream: socket.socket, flow: int) -> dict:
    digest = hashlib.sha256()
    total_ref = [0]
    meter = StallMeter(total_ref)
    meter.start()
    try:
        while True:
            length = struct.unpack("!H", exact(stream, 2))[0]
            if length == 0:
                break
            if length > CHUNK:
                raise RuntimeError(f"invalid bulk frame {length} for flow {flow}")
            digest.update(exact(stream, length))
            total_ref[0] += length
    finally:
        meter.stop()
    stream.sendall(struct.pack("!Q", total_ref[0]) + digest.digest())
    if exact(stream, 1) != b"K":
        raise RuntimeError("bulk receiver expected confirmation")
    stream.shutdown(socket.SHUT_WR)
    eof_ok = not stream.recv(1)
    if not eof_ok:
        raise RuntimeError("bulk receiver expected EOF")
    return {"flow": flow, "received_bytes": total_ref[0],
            "received_sha256": digest.hexdigest(), "sha256_ok": True,
            "bytes_match": True, "eof_ok": eof_ok, "max_stall_ms": meter.max_stall_ms}


def web_server(stream: socket.socket) -> dict:
    requests = 0
    total = 0
    request_received_ns = []
    response_sent_ns = []
    while True:
        length = struct.unpack("!H", exact(stream, 2))[0]
        if length == 0:
            break
        request = exact(stream, length)
        request_received_ns.append(time.monotonic_ns())
        quick_ack(stream)
        stream.sendall(struct.pack("!H", length) + request)
        response_sent_ns.append(time.monotonic_ns())
        requests += 1
        total += length
    stream.sendall(b"K")
    return {"requests": requests, "bytes": total, "web_error_count": 0,
            "web_success_rate": 1.0, "request_received_monotonic_ns": request_received_ns,
            "response_sent_monotonic_ns": response_sent_ns}


def web_client(stream: socket.socket) -> dict:
    body = b"GET /resource HTTP/1.1\r\nHost: browse.test\r\n\r\n".ljust(768, b" ")
    started = time.monotonic()
    quick_ack(stream)
    stream.sendall(struct.pack("!H", len(body)) + body)
    length = struct.unpack("!H", exact(stream, 2))[0]
    if exact(stream, length) != body:
        raise RuntimeError("web response mismatch")
    rtt = (time.monotonic() - started) * 1000
    stream.sendall(struct.pack("!H", 0))
    if exact(stream, 1) != b"K":
        raise RuntimeError("web session did not finish")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("web client expected EOF")
    return {"requests": 1, "bytes": len(body), "web_success_rate": 1.0, "web_error_count": 0,
            "web_rtt_ms": [rtt], "web_start_monotonic_ms": [int(started * 1000)], "eof_ok": True}


def web_client_group(target_ip: str, port: int, workers: int, requests: int,
                     interval_ms: int, start_at: float) -> list[dict]:
    def lane(worker):
        results = []
        for index in range(worker, requests, workers):
            scheduled = start_at + (index // workers) * interval_ms / 1000
            time.sleep(max(0, scheduled - time.monotonic()))
            began_ns = time.monotonic_ns()
            row = {"flow": index, "connection_start_monotonic_ms": began_ns // 1_000_000,
                   "connection_start_monotonic_ns": began_ns}
            stage = "socket_connect"
            try:
                with socket.create_connection(("127.0.0.1", SOCKS_PORT), timeout=10) as stream:
                    row["peer_port"] = stream.getsockname()[1]
                    stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    stage = "socks_handshake"
                    socks_handshake(stream, target_ip, port)
                    stage = "origin_ready"
                    stream.sendall(b"W" + bytes([index]))
                    if exact(stream, 1) != b"R":
                        raise RuntimeError("web origin did not accept stream")
                    stage = "web_request"
                    row.update(web_client(stream))
            except (OSError, RuntimeError) as exc:
                row.update(web_error_count=1, web_success_rate=0.0, error=str(exc),
                           error_stage=stage, elapsed_ms=round((time.monotonic_ns() - began_ns) / 1_000_000, 3))
            row["connection_end_monotonic_ns"] = time.monotonic_ns()
            results.append(row)
        return results

    with ThreadPoolExecutor(max_workers=workers) as pool:
        return [row for rows in pool.map(lane, range(workers)) for row in rows]


def gray_client(stream: socket.socket, flow: int, requests: int, interval_ms: int) -> dict:
    body = payload_block(flow, 0, 8192)
    for index in range(requests):
        stream.sendall(struct.pack("!H", len(body)) + body)
        length = struct.unpack("!H", exact(stream, 2))[0]
        if exact(stream, length) != body:
            raise RuntimeError("grayzone response mismatch")
        if index + 1 < requests:
            time.sleep(interval_ms / 1000)
    stream.sendall(struct.pack("!H", 0))
    if exact(stream, 1) != b"K":
        raise RuntimeError("grayzone session did not finish")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("grayzone expected EOF")
    return {"flow": flow, "bytes": requests * len(body), "sha256_ok": True,
            "bytes_match": True, "eof_ok": True, "max_stall_ms": 0}


def phase_server(stream: socket.socket, flow: int, small_count: int, byte_count: int) -> dict:
    digest = hashlib.sha256()
    for _ in range(small_count):
        length = struct.unpack("!H", exact(stream, 2))[0]
        payload = exact(stream, length)
        stream.sendall(struct.pack("!H", length) + payload)
    sent = 0
    while sent < byte_count:
        size = min(CHUNK, byte_count - sent)
        block = payload_block(flow, sent, size)
        stream.sendall(struct.pack("!H", size) + block)
        digest.update(block)
        sent += size
    stream.sendall(struct.pack("!H", 0))
    receipt = exact(stream, 40)
    if receipt != struct.pack("!Q", sent) + digest.digest():
        raise RuntimeError("phase-shift receipt mismatch")
    stream.sendall(b"K")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("phase-shift expected EOF")
    return {"flow": flow, "sent_bytes": sent, "sent_sha256": digest.hexdigest(),
            "sha256_ok": True, "bytes_match": True, "eof_ok": True,
            "max_stall_ms": 0}


def phase_client(stream: socket.socket, flow: int, small_count: int, byte_count: int,
                 interval_ms: int) -> dict:
    for index in range(small_count):
        body = payload_block(flow, index, 256)
        stream.sendall(struct.pack("!H", len(body)) + body)
        length = struct.unpack("!H", exact(stream, 2))[0]
        if exact(stream, length) != body:
            raise RuntimeError("phase-shift small response mismatch")
        time.sleep(interval_ms / 1000)
    digest = hashlib.sha256()
    total_ref = [0]
    meter = StallMeter(total_ref)
    meter.start()
    try:
        while True:
            length = struct.unpack("!H", exact(stream, 2))[0]
            if length == 0:
                break
            block = exact(stream, length)
            digest.update(block)
            total_ref[0] += length
    finally:
        meter.stop()
    if total_ref[0] != byte_count:
        raise RuntimeError("phase-shift byte count mismatch")
    stream.sendall(struct.pack("!Q", total_ref[0]) + digest.digest())
    if exact(stream, 1) != b"K":
        raise RuntimeError("phase-shift receipt missing")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("phase-shift expected EOF")
    return {"flow": flow, "received_bytes": total_ref[0],
            "received_sha256": digest.hexdigest(), "sha256_ok": True,
            "bytes_match": True, "eof_ok": True, "max_stall_ms": meter.max_stall_ms}


def server(target_ip: str, port: int, mode: str, ready: Path, flows: int,
           byte_count: int, requests: int, interval_ms: int, small_count: int,
           start_at: float, duration: float) -> list[dict]:
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((target_ip, port))
    listener.listen(flows)
    listener.settimeout(IO_TIMEOUT)
    ready.touch()
    streams = []
    try:
        if mode == "web":
            def serve_web(stream):
                with stream:
                    stream.settimeout(15)
                    row = {}
                    try:
                        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        header = exact(stream, 2)
                        if header[:1] != b"W":
                            raise RuntimeError("unexpected web workload header")
                        row["flow"] = header[1]
                        stream.sendall(b"R")
                        row.update(web_server(stream))
                    except (OSError, RuntimeError) as exc:
                        row.update(web_error_count=1, web_success_rate=0.0, error=str(exc))
                    return row

            deadline = start_at + ((requests - 1) // flows) * interval_ms / 1000 + 15
            with ThreadPoolExecutor(max_workers=flows) as pool:
                futures = []
                for _ in range(requests):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    listener.settimeout(remaining)
                    try:
                        stream, _ = listener.accept()
                    except TimeoutError:
                        break
                    futures.append(pool.submit(serve_web, stream))
                listener.close()
                return [future.result() for future in futures]
        for _ in range(flows):
            stream, _ = listener.accept()
            stream.settimeout(IO_TIMEOUT)
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            header = exact(stream, 2)
            expected = {"bulk": b"B", "web": b"W", "gray": b"G", "phase": b"P"}[mode]
            if header[:1] != expected:
                raise RuntimeError(f"unexpected workload header {header!r}")
            streams.append((header[1], stream))
            stream.sendall(b"R")
        def work(item):
            flow, stream = item
            with stream:
                if mode == "bulk":
                    return send_bulk(stream, flow, byte_count, duration, start_at)
                if mode == "gray":
                    return {"flow": flow, **web_server(stream)}
                return phase_server(stream, flow, small_count, byte_count)
        return run_workers(streams, work)
    finally:
        listener.close()


def client(target_ip: str, port: int, mode: str, ready: Path, flows: int,
           byte_count: int, requests: int, interval_ms: int, small_count: int,
           start_at: float, duration: float) -> list[dict]:
    deadline = time.monotonic() + IO_TIMEOUT
    while not ready.exists() and time.monotonic() < deadline:
        time.sleep(.02)
    if not ready.exists():
        raise RuntimeError("workload server did not become ready")
    if mode == "web":
        return web_client_group(target_ip, port, flows, requests, interval_ms, start_at)
    time.sleep(max(0, start_at - time.monotonic()))
    streams = []
    for flow in range(flows):
        began_ns = time.monotonic_ns()
        stream = connect(target_ip, port)
        stream.sendall({"bulk": b"B", "web": b"W", "gray": b"G", "phase": b"P"}[mode] + bytes([flow]))
        if exact(stream, 1) != b"R":
            raise RuntimeError("workload origin did not accept stream")
        streams.append((flow, stream, began_ns))
    def work(item):
        flow, stream, began_ns = item
        with stream:
            peer_port = stream.getsockname()[1]
            if mode == "bulk":
                report = receive_bulk(stream, flow)
            elif mode == "gray":
                report = gray_client(stream, flow, requests, interval_ms)
            else:
                report = phase_client(stream, flow, small_count, byte_count, interval_ms)
        return {"peer_port": peer_port, "connection_start_monotonic_ns": began_ns,
                "connection_end_monotonic_ns": time.monotonic_ns(), **report}
    return run_workers(streams, work)


def main() -> int:
    role, target_ip, port, mode, ready, flows, byte_count, requests, interval_ms, small_count, start_at, duration = sys.argv[1:]
    if role not in ("server", "client") or mode not in ("bulk", "web", "gray", "phase"):
        raise SystemExit("usage: strict_flow.py server|client IP PORT MODE READY FLOWS BYTES REQUESTS INTERVAL_MS SMALL_COUNT START_AT DURATION")
    values = (int(port), mode, Path(ready), int(flows), int(byte_count),
              int(requests), int(interval_ms), int(small_count), float(start_at), float(duration))
    reports = server(target_ip, *values) if role == "server" else client(target_ip, *values)
    print(json.dumps(reports), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
