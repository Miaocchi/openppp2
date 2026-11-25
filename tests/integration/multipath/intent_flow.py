"""Small TCP workloads for the VMUX intent acceptance fixture."""

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
METADATA = b'{"mime":"application/octet-stream","segment_bytes":262144}'
METADATA_RESPONSE = (f"HTTP/1.1 200 OK\r\nContent-Length: {len(METADATA)}\r\n\r\n".encode()
                     + METADATA)
METADATA_REQUEST = b"GET /metadata HTTP/1.1\r\nHost: intent.test\r\n\r\n"
CONTENT_REQUEST = b"GET /content HTTP/1.1\r\nHost: intent.test\r\n\r\n"


def run_workers(items, work):
    """Run stream workers and unblock siblings when one worker fails."""
    pool = ThreadPoolExecutor(max_workers=len(items))
    futures = {pool.submit(work, item): index for index, item in enumerate(items)}
    results = [None] * len(items)
    try:
        for future in as_completed(futures):
            results[futures[future]] = future.result()
    except BaseException:
        for _, stream in items:
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


def connect(target_ip: str, target_port: int) -> socket.socket:
    return connect_via_socks(target_ip, target_port, IO_TIMEOUT)


def bulk_sender(stream: socket.socket, flow: int, duration: int, paced: bool = False) -> dict:
    body = hashlib.shake_256(f"intent-download/{flow}".encode()).digest(CHUNK - 8)
    digest = hashlib.sha256()
    total = 0
    started = time.monotonic()
    while time.monotonic() < started + duration:
        block = struct.pack("!Q", total) + body
        stream.sendall(struct.pack("!H", CHUNK) + block)
        digest.update(block)
        total += CHUNK
        if paced and total % (8 * CHUNK) == 0:
            time.sleep(.1)
    stream.sendall(b"\x00\x00")
    receipt = exact(stream, 40)
    expected = struct.pack("!Q", total) + digest.digest()
    if receipt != expected:
        raise RuntimeError(f"bulk receipt mismatch for flow {flow}")
    stream.sendall(b"K")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("bulk sender expected EOF")
    return {"flow": flow, "sent_bytes": total, "sent_sha256": digest.hexdigest(),
            "send_start_monotonic_ms": int(started * 1000),
            "send_end_monotonic_ms": int(time.monotonic() * 1000)}


def bulk_receiver(stream: socket.socket, flow: int) -> dict:
    digest = hashlib.sha256()
    total = 0
    observed = [0]
    stopped = threading.Event()
    max_stall_ms = [0]

    def sample_stall():
        last = 0
        zero_ms = 0
        while not stopped.wait(0.2):
            if observed[0] == last and observed[0] > 0:
                zero_ms += 200
                max_stall_ms[0] = max(max_stall_ms[0], zero_ms)
            else:
                zero_ms = 0
                last = observed[0]

    sampler = threading.Thread(target=sample_stall, daemon=True)
    sampler.start()
    try:
        while True:
            length, = struct.unpack("!H", exact(stream, 2))
            if length == 0:
                break
            if length != CHUNK:
                raise RuntimeError(f"invalid bulk frame {length} for flow {flow}")
            digest.update(exact(stream, length))
            total += length
            observed[0] = total
    finally:
        stopped.set()
        sampler.join(timeout=1)
    stream.sendall(struct.pack("!Q", total) + digest.digest())
    if exact(stream, 1) != b"K":
        raise RuntimeError("bulk receiver expected confirmation")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("bulk receiver expected EOF")
    return {"flow": flow, "received_bytes": total, "received_sha256": digest.hexdigest(),
            "exact_eof": True, "max_stall_ms": max_stall_ms[0]}


def web_server(stream: socket.socket) -> dict:
    requests = 0
    total = 0
    while True:
        length, = struct.unpack("!H", exact(stream, 2))
        if length == 0:
            break
        request = exact(stream, length)
        stream.sendall(struct.pack("!H", len(request)) + request)
        requests += 1
        total += length
    stream.sendall(b"K")
    return {"requests": requests, "bytes": total}


def web_client(stream: socket.socket) -> dict:
    samples = []
    body = b"GET /resource HTTP/1.1\r\nHost: browse.test\r\n\r\n"
    body = body.ljust(768, b" ")
    for _ in range(40):
        started = time.monotonic()
        stream.sendall(struct.pack("!H", len(body)) + body)
        length, = struct.unpack("!H", exact(stream, 2))
        if exact(stream, length) != body:
            raise RuntimeError("web response mismatch")
        samples.append((time.monotonic() - started) * 1000)
        time.sleep(0.1)
    stream.sendall(b"\x00\x00")
    if exact(stream, 1) != b"K":
        raise RuntimeError("web session did not finish")
    stream.shutdown(socket.SHUT_WR)
    if stream.recv(1):
        raise RuntimeError("web client expected EOF")
    ordered = sorted(samples)
    return {"requests": len(samples), "bytes": len(body) * len(samples),
            "rtt_p95_ms": ordered[max(0, int(len(ordered) * .95) - 1)],
            "rtt_p99_ms": ordered[max(0, int(len(ordered) * .99) - 1)],
            "rtt_max_ms": max(ordered)}


def server(role: str, target_ip: str, port: int, mode: str, ready: Path,
           flows: int, duration: int, scenario: str = "") -> list[dict]:
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((target_ip, port))
    listener.listen(flows)
    listener.settimeout(IO_TIMEOUT)
    ready.touch()
    streams = []
    try:
        for _ in range(flows):
            stream, peer = listener.accept()
            stream.settimeout(IO_TIMEOUT)
            header = exact(stream, 2)
            if header[:1] != (b"B" if mode == "bulk" else b"W"):
                raise RuntimeError(f"unexpected workload header {header!r}")
            streams.append((header[1], stream))
            if scenario and mode == "bulk" and exact(stream, len(METADATA_REQUEST)) != METADATA_REQUEST:
                raise RuntimeError("metadata request mismatch")
            stream.sendall(METADATA_RESPONSE if scenario and mode == "bulk" else b"R")
        def work(item):
            flow, stream = item
            with stream:
                if mode == "bulk":
                    if scenario and exact(stream, len(CONTENT_REQUEST)) != CONTENT_REQUEST:
                        raise RuntimeError(f"content request mismatch for flow {flow}")
                    return bulk_sender(stream, flow, duration, paced=scenario == "video")
                return web_server(stream)
        return run_workers(streams, work)
    finally:
        listener.close()


def client(target_ip: str, port: int, mode: str, ready: Path,
           flows: int, duration: int, scenario: str = "") -> list[dict]:
    deadline = time.monotonic() + IO_TIMEOUT
    while not ready.exists() and time.monotonic() < deadline:
        time.sleep(.02)
    if not ready.exists():
        raise RuntimeError("workload server did not become ready")
    streams = []
    connections = {}
    for flow in range(flows):
        began = time.monotonic_ns()
        if scenario:
            stream = socket.create_connection(("127.0.0.1", SOCKS_PORT), timeout=IO_TIMEOUT)
            try:
                stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                socks_handshake(stream, target_ip, port)
            except BaseException:
                stream.close()
                raise
            connections[flow] = {"connection_start_monotonic_ns": began, "peer_port": stream.getsockname()[1]}
        else:
            stream = connect(target_ip, port)
        request = (b"B" if mode == "bulk" else b"W") + bytes([flow])
        stream.sendall(request + METADATA_REQUEST if scenario and mode == "bulk" else request)
        response = METADATA_RESPONSE if scenario and mode == "bulk" else b"R"
        if exact(stream, len(response)) != response:
            raise RuntimeError("workload origin did not accept stream")
        streams.append((flow, stream))
    def work(item):
        flow, stream = item
        with stream:
            body_started = time.monotonic_ns()
            if mode == "bulk":
                if scenario:
                    stream.sendall(CONTENT_REQUEST)
                report = bulk_receiver(stream, flow)
            else:
                report = web_client(stream)
            if scenario:
                report.update(connections[flow], flow=flow, body_start_monotonic_ns=body_started,
                              connection_end_monotonic_ns=time.monotonic_ns())
            return report
    return run_workers(streams, work)


def main() -> int:
    role, target_ip, port, mode, ready, flows, duration, *options = sys.argv[1:]
    scenario = options[0] if options else ""
    if role not in ("server", "client") or mode not in ("bulk", "web"):
        raise SystemExit("usage: intent_flow.py server|client IP PORT bulk|web READY FLOWS DURATION")
    if role == "server":
        reports = server(role, target_ip, int(port), mode, Path(ready), int(flows), int(duration), scenario)
    else:
        reports = client(target_ip, int(port), mode, Path(ready), int(flows), int(duration), scenario)
    print(json.dumps(reports), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
