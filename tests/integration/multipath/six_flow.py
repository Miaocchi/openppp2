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


def send(stream, direction, flow, duration):
    # Make a substitution across streams or frame positions visible in SHA-256.
    body = hashlib.shake_256(f"{direction}/{flow}".encode()).digest(CHUNK - 8)
    digest = hashlib.sha256()
    total = 0
    end = time.monotonic() + duration
    while time.monotonic() < end:
        block = struct.pack("!Q", total) + body
        stream.sendall(struct.pack("!H", CHUNK) + block)
        digest.update(block)
        total += CHUNK
    stream.sendall(b"\x00\x00")
    receipt = exact(stream, 40)
    if not total or receipt != struct.pack("!Q", total) + digest.digest():
        raise RuntimeError(f"bad receipt for {total} bytes, sha256={digest.hexdigest()}")
    stream.sendall(b"K")
    return {"sent_bytes": total, "sent_sha256": digest.hexdigest(), "sender_receipt": True}


def receive(stream):
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
    stream.sendall(struct.pack("!Q", total) + digest.digest())
    if exact(stream, 1) != b"K" or stream.recv(1):
        raise RuntimeError("expected sender confirmation and EOF")
    return {"received_bytes": total, "received_sha256": digest.hexdigest(), "sender_confirmed": True}


def phase(role, address, port, direction, ready_file, flows, duration):
    server = role == "server"
    tag = direction[:1].upper().encode()
    barrier = threading.Barrier(flows, timeout=IO_TIMEOUT)
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
                        barrier.wait()
                        stream.sendall(b"G")
                    sender = server == (direction == "download")
                    result = send(stream, direction, flow, duration) if sender else receive(stream)
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
    duration = int(options[1]) if len(options) > 1 else SECONDS[direction]
    if not 1 <= flows <= 64:
        raise SystemExit("flow count must be between 1 and 64")
    if not 12 <= duration <= 120:
        raise SystemExit("send duration must be between 12 and 120 seconds")
    raise SystemExit(0 if phase(role, address, int(port), direction, Path(ready), flows, duration) else 1)
