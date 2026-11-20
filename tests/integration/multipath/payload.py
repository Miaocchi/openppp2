"""Application endpoints for the private production multipath fixture."""

from contextlib import nullcontext
import hashlib
import json
import socket
import struct
import sys
import time
from pathlib import Path


SOCKS_PORT = 11080


def exact(stream, size):
    data = bytearray()
    while len(data) < size:
        block = stream.recv(min(65536, size - len(data)))
        if not block:
            raise RuntimeError(f"early EOF: received {len(data)} of {size} bytes")
        data.extend(block)
    return bytes(data)


def socks_handshake(stream, target_ip, target_port):
    stream.sendall(b"\x05\x01\x00")
    if exact(stream, 2) != b"\x05\x00":
        raise RuntimeError("SOCKS5 method negotiation failed")
    stream.sendall(b"\x05\x01\x00\x01" + socket.inet_aton(target_ip) + struct.pack("!H", target_port))
    response = exact(stream, 4)
    if response[:3] != b"\x05\x00\x00":
        raise RuntimeError(f"SOCKS5 CONNECT failed: {response!r}")
    atyp = response[3]
    if atyp == 3:
        length = exact(stream, 1)[0]
    elif atyp in (1, 4):
        length = 4 if atyp == 1 else 16
    else:
        raise RuntimeError(f"invalid SOCKS5 address type: {atyp}")
    exact(stream, length + 2)


def connect(target_ip, target_port, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        stream = None
        try:
            stream = socket.create_connection(("127.0.0.1", SOCKS_PORT), timeout=1)
            stream.settimeout(timeout)
            socks_handshake(stream, target_ip, target_port)
            return stream
        except (OSError, RuntimeError) as error:
            if stream is not None:
                stream.close()
            last = error
            time.sleep(.1)
    raise RuntimeError(f"SOCKS5 target did not become ready: {last}")


def benchmark(target_ip, port, size, timeout, trial, probes, output):
    data = hashlib.shake_256(("openppp2-multipath-" + trial).encode()).digest(size)
    started = time.monotonic()
    with connect(target_ip, port, timeout) as stream:
        stream.sendall(data)
        stream.shutdown(socket.SHUT_WR)
        received = exact(stream, size)
        if stream.recv(1):
            raise RuntimeError("echo returned more bytes than sent")
    elapsed = (time.monotonic() - started) * 1000
    if data != received:
        raise RuntimeError("bulk transfer integrity mismatch")
    latencies = []
    for index in range(int(probes)):
        message = hashlib.shake_256((trial + str(index)).encode()).digest(1024)
        started = time.monotonic()
        with connect(target_ip, port, timeout) as stream:
            stream.sendall(message)
            if exact(stream, len(message)) != message:
                raise RuntimeError("latency probe integrity mismatch")
        latencies.append((time.monotonic() - started) * 1000)
    result = {"sent_bytes": len(data), "received_bytes": len(received),
              "sent_sha256": hashlib.sha256(data).hexdigest(), "received_sha256": hashlib.sha256(received).hexdigest(),
              "bulk_elapsed_ms": elapsed, "throughput_mbps": size * 8 / elapsed / 1000,
              "latency_samples_ms": latencies}
    Path(output).write_text(json.dumps(result) + "\n")


def transfer(stream, direction, sender, size, trial):
    data = hashlib.shake_256(trial.encode() + direction).digest(size)
    if sender:
        if exact(stream, 1) != b"R":
            raise RuntimeError("receiver not ready")
        stream.sendall(data)
        stream.shutdown(socket.SHUT_WR)
        if exact(stream, 1) != b"K":
            raise RuntimeError("missing verified receipt after SHUT_WR")
        return {"sender_receipt": True, "sent_sha256": hashlib.sha256(data).hexdigest()}
    stream.sendall(b"R")
    received = exact(stream, size)
    if stream.recv(1) or received != data:
        raise RuntimeError("unexpected extra byte or payload mismatch")
    stream.sendall(b"K")  # Only after both the payload and exact EOF were verified.
    return {"bytes": size, "received_sha256": hashlib.sha256(received).hexdigest(), "exact_eof": True}


def check_receipts(role, target_ip, port, size, timeout, trial):
    results = {}
    with (socket.socket() if role == "server" else nullcontext()) as listener:
        if role == "server":
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((target_ip, port))
            listener.listen()
            listener.settimeout(timeout)
        for direction in (b"U", b"D"):
            if role == "server":
                stream, peer = listener.accept()
            else:
                stream = connect(target_ip, port, timeout)
            with stream:
                stream.settimeout(timeout)
                if role == "server":
                    if peer[0] != target_ip or exact(stream, 1) != direction:
                        raise RuntimeError("expected a server-local business connection for " + direction.decode())
                else:
                    stream.sendall(direction)
                sender_role = "client" if direction == b"U" else "server"
                results[direction.decode()] = transfer(stream, direction, role == sender_role, size, trial)
    print(json.dumps(results))


if __name__ == "__main__":
    role, target, port, size, timeout, trial, *extra = sys.argv[1:]
    if role == "benchmark":
        benchmark(target, int(port), int(size), float(timeout), trial, *extra)
    elif role in ("server", "client"):
        check_receipts(role, target, int(port), int(size), float(timeout), trial)
    else:
        raise SystemExit(f"unknown fixture role: {role}")
