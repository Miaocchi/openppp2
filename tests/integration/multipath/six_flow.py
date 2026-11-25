"""One endpoint of a timed concurrent-stream phase in the production namespace fixture."""

from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import nullcontext
import hashlib
import json
import socket
import struct
import sys
import threading
import time
from pathlib import Path

from payload import SOCKS_PORT, exact, socks_handshake


FLOWS = 6
SECONDS = {"upload": 25, "download": 12}
CONNECT_TIMEOUT = 4
IO_TIMEOUT = 20
CHUNK = 32768


def write_payload(stream, direction, flow, duration):
    # Make a substitution across streams or frame positions visible in SHA-256.
    body = hashlib.shake_256(f"{direction}/{flow}".encode()).digest(CHUNK - 8)
    digest = hashlib.sha256()
    total = 0
    started = time.monotonic()
    end = started + duration
    while time.monotonic() < end:
        block = struct.pack("!Q", total) + body
        stream.sendall(struct.pack("!H", CHUNK) + block)
        digest.update(block)
        total += CHUNK
    finished = time.monotonic()
    stream.sendall(b"\x00\x00")
    return total, digest.digest(), started, finished


def read_payload(stream):
    digest = hashlib.sha256()
    total = 0
    while True:
        length, = struct.unpack("!H", exact(stream, 2))
        if length == 0:
            break
        if length != CHUNK:
            raise RuntimeError(f"invalid frame length {length} after {total} bytes")
        digest.update(exact(stream, length))
        total += length
    return total, digest.digest()


def verify_receipt(stream, sent):
    total, digest, started, finished = sent
    receipt = exact(stream, 40)
    if not total or receipt != struct.pack("!Q", total) + digest:
        raise RuntimeError(f"bad receipt for {total} bytes, sha256={digest.hex()}")
    return {"sent_bytes": total, "sent_sha256": digest.hex(), "sender_receipt": True,
            "send_start_monotonic_ms": int(started * 1000),
            "send_end_monotonic_ms": int(finished * 1000)}


def verify_confirmation(stream, received):
    total, digest = received
    if exact(stream, 1) != b"K":
        raise RuntimeError("expected sender confirmation")
    return {"received_bytes": total, "received_sha256": digest.hex(), "sender_confirmed": True}


def transfer(stream, server, direction, flow, duration):
    if direction == "bidirectional":
        send_direction, receive_direction = ("download", "upload") if server else ("upload", "download")
        with ThreadPoolExecutor(max_workers=1) as sender:
            sending = sender.submit(write_payload, stream, send_direction, flow, duration)
            received = read_payload(stream)
            sent = sending.result()
        total, digest = received
        stream.sendall(struct.pack("!Q", total) + digest)
        send_report = verify_receipt(stream, sent)
        stream.sendall(b"K")
        receive_report = verify_confirmation(stream, received)
        stream.shutdown(socket.SHUT_WR)
        if stream.recv(1):
            raise RuntimeError("expected exact EOF")
        return {send_direction: send_report, receive_direction: {**receive_report, "exact_eof": True}}

    if server == (direction == "download"):
        report = verify_receipt(stream, write_payload(stream, direction, flow, duration))
        stream.sendall(b"K")
        return report

    received = read_payload(stream)
    total, digest = received
    stream.sendall(struct.pack("!Q", total) + digest)
    report = verify_confirmation(stream, received)
    if stream.recv(1):
        raise RuntimeError("expected sender confirmation and EOF")
    return {**report, "exact_eof": True}


def phase(role, address, port, direction, ready_file, flows, duration, wait_for_start=False):
    server = role == "server"
    tag = direction[:1].upper().encode()

    def await_sampler():
        if server:
            ready_file.with_suffix(".armed").touch()
        if sys.stdin.buffer.read(1) != b"G":
            raise RuntimeError("sampler exited before releasing workload")

    barrier = threading.Barrier(flows, action=await_sampler if wait_for_start else None, timeout=IO_TIMEOUT)
    with (socket.socket() if server else nullcontext()) as listener:
        if server:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((address, port))
            listener.listen(flows)
            listener.settimeout(IO_TIMEOUT)
            ready_file.touch()

        def worker(slot):
            flow = None if server else slot
            try:
                if server:
                    stream, peer = listener.accept()
                else:
                    stream = socket.create_connection(("127.0.0.1", SOCKS_PORT), CONNECT_TIMEOUT)
                with stream:
                    stream.settimeout(IO_TIMEOUT)
                    stream.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 131072)
                    if server:
                        header = exact(stream, 2)
                        flow = header[1]
                        if peer[0] != address or header[:1] != tag or flow >= flows:
                            raise RuntimeError(f"unexpected business connection: {peer}, {header!r}")
                        stream.sendall(b"R")
                        if exact(stream, 1) != b"G":
                            raise RuntimeError("missing phase start")
                    else:
                        socks_handshake(stream, address, port)
                        stream.sendall(tag + bytes([flow]))
                        if exact(stream, 1) != b"R":
                            raise RuntimeError("origin not ready")
                        # All SOCKS streams must open before sending payload.
                        if not wait_for_start:
                            barrier.wait()
                        stream.sendall(b"G")
                    if wait_for_start:
                        barrier.wait()
                    result = transfer(stream, server, direction, flow, duration)
                    return {"flow": flow, **result}
            except Exception as error:
                barrier.abort()
                return {"flow": flow, "error": repr(error)}

        with ThreadPoolExecutor(max_workers=flows) as pool:
            futures = [pool.submit(worker, slot) for slot in range(flows)]
            reports = []
            for future in as_completed(futures):
                report = future.result()
                print(json.dumps(report), flush=True)
                reports.append(report)
    return (all("error" not in r for r in reports) and
            {r["flow"] for r in reports} == set(range(flows)))


if __name__ == "__main__":
    role, address, port, direction, ready, *options = sys.argv[1:]
    flows = int(options[0]) if options else FLOWS
    duration = int(options[1]) if len(options) > 1 else SECONDS.get(direction, 25)
    if not 1 <= flows <= 64:
        raise SystemExit("flow count must be between 1 and 64")
    if not 12 <= duration <= 120:
        raise SystemExit("send duration must be between 12 and 120 seconds")
    passed = phase(role, address, int(port), direction, Path(ready), flows, duration,
                   wait_for_start="--wait-for-start" in options[2:])
    raise SystemExit(0 if passed else 1)
