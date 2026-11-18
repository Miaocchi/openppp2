#!/usr/bin/env python3
"""Private Linux namespace fixture for production multi-path scenarios."""

from __future__ import annotations

import argparse
import hashlib
import contextlib
import json
import io
import math
import os
import secrets
import signal
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
import copy
import re
from pathlib import Path


PROFILES = {
    "asymmetric-5-80-200": {"paths": [(5, 100, 0, 0), (80, 40, 8, 0), (200, 20, 20, 0)], "shared_mbps": None},
    "asymmetric-5-80": {"paths": [(5, 100, 0, 0), (80, 40, 8, 0)], "shared_mbps": None},
    "equal-rtt": {"paths": [(30, 50, 0, 0), (30, 50, 0, 0), (30, 50, 0, 0)], "shared_mbps": None},
    "shared-5-80-200": {"paths": [(5, 100, 2, 0), (80, 100, 12, 0), (200, 100, 25, 0)], "shared_mbps": 60},
    "lossy-20-80-200": {"paths": [(20, 80, 3, 0.1), (80, 40, 12, 0.5), (200, 20, 25, 1.0)], "shared_mbps": None},
}


class HarnessError(RuntimeError):
    pass


def block_interrupt_signals():
    return signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})


def spawn_owned(children: list[subprocess.Popen], argv: list[str], **kwargs) -> subprocess.Popen:
    previous = block_interrupt_signals()
    try:
        kwargs["start_new_session"] = True
        child = subprocess.Popen(argv, **kwargs)
        children.append(child)
        return child
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def run(argv: list[str], *, check: bool = True, capture: bool = False) -> str:
    result = subprocess.run(argv, text=True,
                            stdout=subprocess.PIPE if capture or not check else None,
                            stderr=subprocess.PIPE if capture or not check else None)
    if check and result.returncode:
        detail = (result.stderr or result.stdout or "").strip()
        raise HarnessError(f"command failed ({result.returncode}): {' '.join(argv)}: {detail}")
    return result.stdout if capture else ""


def terminate_process_group(process: subprocess.Popen, grace_seconds: float = 3.0) -> None:
    group_exists = True
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        group_exists = False
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            group_exists = False
            break
        time.sleep(0.05)
    if group_exists:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass


def established_path_endpoints(namespace: str) -> set[tuple[str, int]]:
    output = run(["ip", "netns", "exec", namespace, "ss", "-Htn", "state", "established"], capture=True)
    endpoints: set[tuple[str, int]] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        host, port = fields[-1].rsplit(":", 1)
        try:
            endpoints.add((host.strip("[]"), int(port)))
        except ValueError:
            continue
    return endpoints


class Lab:
    def __init__(self, profile: str):
        self.profile_name = profile
        self.profile = PROFILES[profile]
        self.specs = self.profile["paths"]
        self.suffix = uuid.uuid4().hex[:8]
        self.names = {key: f"mp{key[0]}{self.suffix}" for key in ("client", "router", "bottleneck", "server")}
        self.created_ns: list[str] = []
        self.root_links: dict[str, tuple[int, str, bool]] = {}
        self.paths: list[dict] = []

    def setup(self) -> None:
        if os.geteuid() != 0:
            raise HarnessError("network namespace fixture requires root/CAP_NET_ADMIN")
        for tool in ("ip", "tc", "ping", "ss"):
            if not shutil.which(tool):
                raise HarnessError(f"required command is unavailable: {tool}")
        for ns in self.names.values():
            previous = block_interrupt_signals()
            try:
                run(["ip", "netns", "add", ns])
                self.created_ns.append(ns)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous)
            run(["ip", "-n", ns, "link", "set", "lo", "up"])
        for key in ("router", "bottleneck"):
            run(["ip", "netns", "exec", self.names[key], "sysctl", "-q", "-w", "net.ipv4.ip_forward=1"])
        self._veth("bs0", "s0", self.names["bottleneck"], self.names["server"])
        self._addr(self.names["bottleneck"], "bs0", "10.70.0.1", 24)
        self._addr(self.names["server"], "s0", "10.70.0.2", 24)
        for index, (rtt, rate, jitter, loss) in enumerate(self.specs, 1):
            client = f"c{index}"
            router_c = f"rc{index}"
            router_b = f"rb{index}"
            bottle_r = f"br{index}"
            self._veth(client, router_c, self.names["client"], self.names["router"])
            self._veth(router_b, bottle_r, self.names["router"], self.names["bottleneck"])
            cnet = f"10.{40 + index}.0"
            bnet = f"10.{60 + index}.0"
            cip, crip = f"{cnet}.1", f"{cnet}.2"
            rbip, brip = f"{bnet}.1", f"{bnet}.2"
            sip = f"10.70.0.{10 + index}"
            self._addr(self.names["client"], client, cip, 30)
            self._addr(self.names["router"], router_c, crip, 30)
            self._addr(self.names["router"], router_b, rbip, 30)
            self._addr(self.names["bottleneck"], bottle_r, brip, 30)
            run(["ip", "-n", self.names["client"], "route", "add", sip + "/32", "via", crip, "dev", client])
            run(["ip", "-n", self.names["router"], "route", "add", sip + "/32", "via", brip, "dev", router_b])
            run(["ip", "-n", self.names["bottleneck"], "route", "add", cip + "/32", "via", rbip, "dev", bottle_r])
            # Delay and caps are per path before traffic reaches the aggregate link.
            for ns, dev in ((self.names["client"], client), (self.names["router"], router_c),
                            (self.names["router"], router_b), (self.names["bottleneck"], bottle_r)):
                self._shape(ns, dev, rtt, rate, jitter, loss)
            self.paths.append({"id": f"path{index}", "rtt_ms": rtt, "jitter_ms": jitter,
                               "loss_percent": loss, "rate_mbps": rate,
                               "client_ip": cip, "server_ip": sip,
                               "client_ns": self.names["client"], "server_ns": self.names["server"],
                               "router_ns": self.names["router"], "bottleneck_ns": self.names["bottleneck"],
                               "client_if": client, "router_client_if": router_c,
                               "router_bottleneck_if": router_b, "bottleneck_router_if": bottle_r,
                               "router_client_ip": crip, "router_bottleneck_ip": rbip,
                               "bottleneck_router_ip": brip})
        # All paths share this one server-facing link and its aggregate capacity.
        for index in range(1, len(self.paths) + 1):
            run(["ip", "-n", self.names["server"], "addr", "add", f"10.70.0.{10 + index}/32", "dev", "lo"])
            path = self.paths[index - 1]
            run(["ip", "-n", self.names["server"], "route", "add", path["client_ip"] + "/32",
                 "via", "10.70.0.1", "dev", "s0"])
        shared = self.profile["shared_mbps"]
        if shared:
            self._rate(self.names["bottleneck"], "bs0", shared)
            self._rate(self.names["server"], "s0", shared)

    def _veth(self, left: str, right: str, left_ns: str, right_ns: str) -> None:
        # Temporary host names are random and never reused for namespace names.
        root_left = f"m{uuid.uuid4().hex[:12]}a"
        root_right = f"m{uuid.uuid4().hex[:12]}b"
        alias = f"openppp2-mp-{uuid.uuid4().hex}"
        previous = block_interrupt_signals()
        try:
            run(["ip", "link", "add", root_left, "type", "veth", "peer", "name", root_right])
            self._track_root_link(root_left, alias)
            self._track_root_link(root_right, alias)
            run(["ip", "link", "set", root_left, "alias", alias])
            self._mark_root_link_aliased(root_left)
            run(["ip", "link", "set", root_right, "alias", alias])
            self._mark_root_link_aliased(root_right)
            run(["ip", "link", "set", root_left, "netns", left_ns])
            self.root_links.pop(root_left, None)
            run(["ip", "-n", left_ns, "link", "set", root_left, "name", left])
            run(["ip", "link", "set", root_right, "netns", right_ns])
            self.root_links.pop(root_right, None)
            run(["ip", "-n", right_ns, "link", "set", root_right, "name", right])
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)

    def _track_root_link(self, name: str, alias: str) -> None:
        info = json.loads(run(["ip", "-j", "link", "show", "dev", name], capture=True))
        if len(info) != 1 or not isinstance(info[0].get("ifindex"), int):
            raise HarnessError(f"could not identify newly created temporary link {name}")
        self.root_links[name] = (info[0]["ifindex"], alias, False)

    def _mark_root_link_aliased(self, name: str) -> None:
        if name in self.root_links:
            ifindex, alias, _ = self.root_links[name]
            self.root_links[name] = (ifindex, alias, True)

    @staticmethod
    def _addr(ns: str, dev: str, address: str, prefix: int) -> None:
        run(["ip", "-n", ns, "addr", "add", f"{address}/{prefix}", "dev", dev])
        run(["ip", "-n", ns, "link", "set", dev, "up"])

    @staticmethod
    def _shape(ns: str, dev: str, rtt: int, rate: int, jitter: float = 0,
               loss_percent: float = 0) -> None:
        # The round trip crosses four shaped path egresses.
        run(["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev,
             "root", "handle", "1:", "tbf", "rate", f"{rate}mbit", "burst", "128kb", "latency", "500ms"])
        Lab._netem(ns, dev, rtt, jitter, loss_percent)

    @staticmethod
    def _netem(ns: str, dev: str, rtt: float, jitter: float = 0,
               loss_percent: float = 0) -> None:
        command = ["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev,
                   "parent", "1:1", "handle", "10:", "netem", "delay", f"{rtt / 4:g}ms"]
        if jitter:
            command.extend((f"{jitter / 4:g}ms", "distribution", "normal"))
        if loss_percent:
            command.extend(("loss", f"{loss_percent:g}%"))
        command.extend(("limit", "32768"))
        run(command)

    @staticmethod
    def _rate(ns: str, dev: str, rate: int) -> None:
        run(["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev,
             "root", "tbf", "rate", f"{rate}mbit", "burst", "256kb", "latency", "500ms"])

    def topology(self) -> dict:
        return {"schema": 1, "profile": self.profile_name, "namespaces": self.names,
                "shared_bottleneck_mbps": self.profile["shared_mbps"],
                "shared_bottleneck_interfaces": ["bottleneck/bs0", "server/s0"] if self.profile["shared_mbps"] else [],
                "paths": self.paths,
                "note": "Paths converge on one server-facing link; shared profile caps this aggregate link."}

    def cleanup(self) -> None:
        previous = block_interrupt_signals()
        try:
            for ns in reversed(self.created_ns):
                run(["ip", "netns", "del", ns], check=False)
            self.created_ns.clear()
            for link, (expected_ifindex, expected_alias, alias_set) in list(self.root_links.items()):
                try:
                    records = json.loads(run(["ip", "-j", "link", "show", "dev", link], capture=True))
                except (HarnessError, json.JSONDecodeError):
                    self.root_links.pop(link, None)
                    continue
                if len(records) != 1 or records[0].get("ifindex") != expected_ifindex:
                    self.root_links.pop(link, None)
                    continue
                current_alias = records[0].get("ifalias", "")
                ownership_matches = current_alias == expected_alias if alias_set else not current_alias
                if ownership_matches:
                    run(["ip", "link", "del", link], check=False)
                self.root_links.pop(link, None)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)

    def verify(self) -> None:
        for path in self.paths:
            for ns, dev in ((path["client_ns"], path["client_if"]),
                            (path["router_ns"], path["router_client_if"]),
                            (path["router_ns"], path["router_bottleneck_if"]),
                            (path["bottleneck_ns"], path["bottleneck_router_if"])):
                try:
                    records = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-s", "-d", "qdisc", "show", "dev", dev], capture=True))
                except json.JSONDecodeError as exc:
                    raise HarnessError(f"invalid qdisc response for {ns}/{dev}") from exc
                if not any(item.get("kind") == "tbf" for item in records) or not any(item.get("kind") == "netem" for item in records):
                    raise HarnessError(f"expected tbf and netem qdiscs on {ns}/{dev}")
                tbf = next(item for item in records if item.get("kind") == "tbf")
                netem = next(item for item in records if item.get("kind") == "netem")
                expected_rate = path["rate_mbps"] * 1_000_000 / 8
                if abs(tbf.get("options", {}).get("rate", -1) - expected_rate) > 1:
                    raise HarnessError(f"path rate cap readback mismatch on {ns}/{dev}")
                options = netem.get("options", {})
                delay = options.get("delay", {})
                if abs(delay.get("delay", -1) - path["rtt_ms"] / 4000) > 1e-6:
                    raise HarnessError(f"netem base delay readback mismatch on {ns}/{dev}")
                if abs(delay.get("jitter", -1) - path["jitter_ms"] / 4000) > 1e-6:
                    raise HarnessError(f"netem jitter readback mismatch on {ns}/{dev}")
                loss = options.get("loss-random", {}).get("loss", 0) * 100
                if abs(loss - path["loss_percent"]) > 1e-6:
                    raise HarnessError(f"netem loss readback mismatch on {ns}/{dev}")
        if self.profile["shared_mbps"]:
            for ns, dev in ((self.names["bottleneck"], "bs0"), (self.names["server"], "s0")):
                records = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-s", "-d", "qdisc", "show", "dev", dev], capture=True))
                tbf = next((item for item in records if item.get("kind") == "tbf"), None)
                if not tbf:
                    raise HarnessError(f"missing aggregate bottleneck qdisc on {ns}/{dev}")
                expected_rate = self.profile["shared_mbps"] * 1_000_000 / 8
                if abs(tbf.get("options", {}).get("rate", -1) - expected_rate) > 1:
                    raise HarnessError(f"shared bottleneck rate readback mismatch on {ns}/{dev}")
        for path in self.paths:
            output = run(["ip", "netns", "exec", path["client_ns"], "ping", "-n", "-i", "0.1", "-c", "5", "-W", "3", path["server_ip"]], capture=True)
            samples = [float(item) for item in re.findall(r"time=([0-9.]+)\s*ms", output)]
            if len(samples) < 3:
                raise HarnessError(f"ping reported fewer than three RTT samples for {path['id']}")
            observed = statistics.median(samples)
            if not path["rtt_ms"] * 0.5 <= observed <= path["rtt_ms"] * 1.5:
                raise HarnessError(f"{path['id']} RTT {observed:.1f}ms is outside target {path['rtt_ms']}ms +/-50%")
        self.path_egress_bytes()

    @staticmethod
    def _root_tbf_bytes(namespace: str, interface: str) -> int:
        records = json.loads(run(["ip", "netns", "exec", namespace, "tc", "-j", "-s", "qdisc",
                                  "show", "dev", interface], capture=True))
        for record in records:
            if record.get("kind") != "tbf":
                continue
            for key in ("bytes", "stats64", "stats"):
                value = record.get(key)
                if isinstance(value, int) and value >= 0:
                    return value
                if isinstance(value, dict) and isinstance(value.get("bytes"), int):
                    return value["bytes"]
        raise HarnessError(f"tc did not report root TBF bytes for {namespace}/{interface}")

    def path_egress_bytes(self) -> dict[str, int]:
        counters = {}
        for path in self.paths:
            counters[path["id"]] = sum((
                self._root_tbf_bytes(path["client_ns"], path["client_if"]),
                self._root_tbf_bytes(path["router_ns"], path["router_client_if"]),
            ))
        return counters


def validate_result(path: Path, scenario: str, *, min_ratio: float,
                    completion_timeout_ms: int, recovery_timeout_ms: int,
                    reconnect_timeout_ms: int, max_p95_ms: float | None,
                    max_p99_ms: float | None) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise HarnessError(f"scenario result is missing or invalid JSON: {exc}") from exc
    if data.get("kind") != "production-multipath":
        raise HarnessError("result kind must be production-multipath")
    if data.get("scenario") != scenario:
        raise HarnessError("result scenario does not match requested scenario")
    integrity = data.get("integrity", {})
    sent_bytes, received_bytes = integrity.get("sent_bytes"), integrity.get("received_bytes")
    if type(sent_bytes) is not int or sent_bytes <= 0 or type(received_bytes) is not int or sent_bytes != received_bytes:
        raise HarnessError("sent and received byte counts must be equal and nonzero")
    sent_hash, received_hash = integrity.get("sent_sha256"), integrity.get("received_sha256")
    if not isinstance(sent_hash, str) or len(sent_hash) != 64 or sent_hash != received_hash:
        raise HarnessError("sent and received SHA-256 must match")
    try:
        int(sent_hash, 16)
    except ValueError as exc:
        raise HarnessError("SHA-256 must be hexadecimal") from exc
    if type(integrity.get("duplicate_bytes")) is not int or integrity["duplicate_bytes"] != 0:
        raise HarnessError("duplicate_bytes must be zero")
    timing = data.get("timing", {})
    if timing.get("completed") is not True:
        raise HarnessError("scenario did not complete")
    completion = finite_nonnegative(timing.get("completion_ms"), "completion_ms")
    if completion > completion_timeout_ms:
        raise HarnessError("completion exceeded configured timeout")
    recovery = finite_nonnegative(timing.get("recovery_ms"), "recovery_ms")
    if recovery > recovery_timeout_ms:
        raise HarnessError("recovery exceeded configured timeout")
    reconnect = finite_nonnegative(timing.get("reconnect_ms"), "reconnect_ms")
    if reconnect > reconnect_timeout_ms:
        raise HarnessError("full reconnect exceeded configured timeout")
    latency = data.get("latency_ms", {})
    p50 = finite_nonnegative(latency.get("p50"), "latency_ms.p50")
    p95 = finite_nonnegative(latency.get("p95"), "latency_ms.p95")
    p99 = finite_nonnegative(latency.get("p99"), "latency_ms.p99")
    if not p50 <= p95 <= p99:
        raise HarnessError("latency percentiles must satisfy p50 <= p95 <= p99")
    if max_p95_ms is not None and p95 > max_p95_ms:
        raise HarnessError("p95 latency exceeded configured ceiling")
    if max_p99_ms is not None and p99 > max_p99_ms:
        raise HarnessError("p99 latency exceeded configured ceiling")
    resources = data.get("resources", {})
    for key in ("peak_rss_kib", "peak_reorder_bytes", "retransmitted_bytes"):
        finite_nonnegative(resources.get(key), key)
    throughput = finite_nonnegative(data.get("throughput_mbps"), "throughput_mbps")
    baseline = finite_nonnegative(data.get("baseline", {}).get("best_single_mbps"), "best_single_mbps")
    if baseline <= 0:
        raise HarnessError("best_single_mbps must be positive")
    ratio = throughput / baseline
    if ratio < min_ratio:
        raise HarnessError(f"throughput ratio {ratio:.3f} is below required {min_ratio:.3f}")
    data["acceptance"] = {"throughput_ratio_to_best_single": ratio,
                          "min_ratio": min_ratio, "passed": True}
    return data


def finite_nonnegative(value, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise HarnessError(f"{name} must be a finite nonnegative number")
    return float(value)


def scenario(args: argparse.Namespace) -> int:
    if args.timeout <= 0 or args.min_ratio <= 0 or args.completion_timeout_ms <= 0:
        raise HarnessError("scenario timeouts and minimum throughput ratio must be positive")
    if args.recovery_timeout_ms < 0 or args.reconnect_timeout_ms < 0:
        raise HarnessError("recovery and reconnect timeouts must be nonnegative")
    if args.max_p95_ms is not None and args.max_p95_ms < 0:
        raise HarnessError("p95 ceiling must be nonnegative")
    if args.max_p99_ms is not None and args.max_p99_ms < 0:
        raise HarnessError("p99 ceiling must be nonnegative")
    lab = Lab(args.profile)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    output.chmod(0o700)
    topology_file = output / "topology.json"
    raw_result = output / "scenario-result.json"
    accepted_file = output / "acceptance.json"
    children: list[subprocess.Popen] = []
    try:
        lab.setup()
        lab.verify()
        topology_file.write_text(json.dumps(lab.topology(), indent=2) + "\n")
        env = os.environ.copy()
        env.update(MP_TOPOLOGY_JSON=str(topology_file), MP_RESULT_JSON=str(raw_result))
        process = spawn_owned(children, args.scenario, env=env)
        try:
            status = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired as exc:
            raise HarnessError(f"production scenario exceeded {args.timeout}s timeout") from exc
        if status:
            terminate_process_group(process)
            raise HarnessError(f"production scenario exited with status {status}")
        accepted = validate_result(raw_result, args.name, min_ratio=args.min_ratio,
                                   completion_timeout_ms=args.completion_timeout_ms,
                                   recovery_timeout_ms=args.recovery_timeout_ms,
                                   reconnect_timeout_ms=args.reconnect_timeout_ms,
                                   max_p95_ms=args.max_p95_ms, max_p99_ms=args.max_p99_ms)
        accepted_file.write_text(json.dumps(accepted, indent=2) + "\n")
        print(f"PASS: production scenario {args.name}; result={accepted_file}")
        return 0
    finally:
        previous = block_interrupt_signals()
        try:
            for child in reversed(children):
                terminate_process_group(child)
            lab.cleanup()
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def _percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _transfer_driver() -> str:
    return '''
import hashlib, json, socket, struct, sys, time
proxy_port, target_ip, target_port, size, timeout, probes, output, trial = sys.argv[1:]
proxy_port, target_port, size, timeout, probes = int(proxy_port), int(target_port), int(size), float(timeout), int(probes)
payload = hashlib.shake_256(("openppp2-multipath-" + trial).encode()).digest(size)
def recv_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        block = sock.recv(length - len(result))
        if not block: raise RuntimeError("truncated SOCKS5 response")
        result.extend(block)
    return bytes(result)
def connect():
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        stream = None
        try:
            stream = socket.create_connection(("127.0.0.1", proxy_port), timeout=1)
            stream.settimeout(timeout)
            stream.sendall(b"\\x05\\x01\\x00")
            if recv_exact(stream, 2) != b"\\x05\\x00": raise RuntimeError("SOCKS5 method negotiation failed")
            address = socket.inet_aton(target_ip)
            stream.sendall(b"\\x05\\x01\\x00\\x01" + address + struct.pack("!H", target_port))
            response = recv_exact(stream, 4)
            if len(response) != 4 or response[:2] != b"\\x05\\x00":
                raise RuntimeError("SOCKS5 CONNECT failed: " + repr(response))
            atyp = response[3]
            if atyp == 1: recv_exact(stream, 4)
            elif atyp == 4: recv_exact(stream, 16)
            elif atyp == 3: recv_exact(stream, recv_exact(stream, 1)[0])
            else: raise RuntimeError("invalid SOCKS5 address type")
            recv_exact(stream, 2)
            return stream
        except (OSError, RuntimeError) as error:
            if stream is not None: stream.close()
            last = error
            time.sleep(.1)
    raise RuntimeError("SOCKS5 target did not become ready: " + str(last))
started = time.monotonic()
with connect() as stream:
    stream.settimeout(timeout); stream.sendall(payload); stream.shutdown(socket.SHUT_WR)
    received = bytearray()
    while len(received) < len(payload):
        block = stream.recv(min(65536, len(payload) - len(received)))
        if not block: break
        received.extend(block)
    if stream.recv(1): raise RuntimeError("echo returned more bytes than sent")
bulk_ms = (time.monotonic() - started) * 1000
received = bytes(received)
latencies = []
for index in range(probes):
    message = hashlib.shake_256((trial + str(index)).encode()).digest(1024)
    started = time.monotonic()
    with connect() as stream:
        stream.sendall(message)
        if recv_exact(stream, len(message)) != message: raise RuntimeError("latency probe integrity mismatch")
    latencies.append((time.monotonic() - started) * 1000)
result = {"sent_bytes": len(payload), "received_bytes": len(received),
          "sent_sha256": hashlib.sha256(payload).hexdigest(),
          "received_sha256": hashlib.sha256(received).hexdigest(),
          "bulk_elapsed_ms": bulk_ms, "throughput_mbps": len(payload) * 8 / bulk_ms / 1000,
          "latency_samples_ms": latencies}
open(output, "w").write(json.dumps(result) + "\\n")
if payload != received: raise SystemExit("bulk transfer integrity mismatch")
'''


def production_smoke(args: argparse.Namespace) -> int:
    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise HarnessError(f"candidate PPP binary is not executable: {binary}")
    if (args.repeats < 2 or args.repeats > 50 or args.payload_bytes < 1024 or
            args.latency_probes < 5 or args.timeout <= 0 or args.startup_timeout <= 0 or
            args.startup_wait < 0 or args.min_throughput_ratio <= 0 or args.max_p99_ratio <= 0):
        raise HarnessError("repeats must be 2..50, payload at least 1024 bytes, and at least 5 latency probes")
    root = Path(__file__).resolve().parents[3]
    compat = root / "tools" / "compat"
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    output.chmod(0o700)
    run_binary = output / "ppp-candidate"
    shutil.copy2(binary, run_binary)
    binary_sha256 = _sha256_file(run_binary)
    candidate_file = output / "candidate.json"
    candidate_file.write_text(json.dumps({"source_binary": str(binary),
                                         "run_binary": str(run_binary),
                                         "sha256": binary_sha256}, indent=2) + "\n")
    candidate_file.chmod(0o600)
    lab = Lab(args.profile)
    children: list[subprocess.Popen] = []
    handles = []
    secret_path: Path | None = None
    try:
        lab.setup()
        lab.verify()
        topology_path = output / "topology.json"
        topology_path.write_text(json.dumps(lab.topology(), indent=2) + "\n")
        server_template = json.loads((compat / "server.json").read_text())
        client_template = json.loads((compat / "client_proxy.json").read_text())
        for template in (server_template, client_template):
            template["telemetry"].update({"enabled": True, "level": 2, "console-log": True})
        echo_port = 19002
        policy_rules = output / "multipath-policy.rules"
        policy_rules.write_text("default proxy\ndns direct direct\ndns proxy proxy\n")
        policy_rules.chmod(0o600)
        secret_path = output / "multipath-test.psk"
        secret_path.write_text(secrets.token_hex(32))
        secret_path.chmod(0o600)
        auth = {"handshake-timeout-ms": 5000,
                "keys": [{"id": "multipath-test", "state": "active", "secret-file": str(secret_path)}]}
        server_template["transport-auth"] = copy.deepcopy(auth)
        server_template["server"]["transport-auth"] = {"enabled": True}
        client_template["transport-auth"] = copy.deepcopy(auth)
        client_template["client"]["transport-auth"] = {"enabled": True}
        client_template["client"]["socks-proxy"]["port"] = 11080
        client_template["client"]["http-proxy"]["port"] = 18080
        echo = spawn_owned(children, ["ip", "netns", "exec", lab.names["server"], sys.executable, "-u", "-c",
            "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); "
            f"s.bind(('0.0.0.0',{echo_port})); s.listen(); "
            "\nwhile True:\n c,_=s.accept()\n with c:\n  while True:\n   b=c.recv(65536)\n   if not b: break\n   c.sendall(b)"],
            stdout=(output / "echo.log").open("w"), stderr=subprocess.STDOUT)
        time.sleep(0.1)
        if echo.poll() is not None:
            raise HarnessError(f"echo fixture exited; inspect {output / 'echo.log'}")

        trial_index = 0
        def measure(label: str, selected_paths: list[dict], multipath: bool) -> dict:
            nonlocal trial_index
            trials = []
            for repeat in range(args.repeats):
                trial_index += 1
                server_port = 20000 + trial_index
                server = copy.deepcopy(server_template)
                server["tcp"]["listen"]["port"] = server_port
                server["udp"]["listen"]["port"] = server_port
                if multipath:
                    server["server"]["multipath"] = {"enabled": True}
                else:
                    server["server"].pop("multipath", None)
                client = copy.deepcopy(client_template)
                client["client"]["server"] = f"ppp://{selected_paths[0]['server_ip']}:{server_port}/"
                client["client"].pop("mappings", None)
                client["client"]["policy"] = {
                    "version": 2,
                    "rules": {"path": str(policy_rules)},
                    "dns": {"mode": "real", "resolvers": {
                        "direct": {"via": "direct", "servers": ["udp://192.0.2.53:53"]},
                        "proxy": {"via": "proxy", "servers": ["udp://192.0.2.53:53"]},
                    }},
                }
                if multipath:
                    client["client"]["multipath"] = {
                        "enabled": True, "policy": "adaptive", "max_connections": len(selected_paths),
                        "max_concurrent_connects": len(selected_paths), "resume_grace_ms": 30000,
                        "paths": [{"id": p["id"], "transport": "tcp", "connect_host": p["server_ip"],
                                   "port": server_port, "connections": 1} for p in selected_paths],
                    }
                else:
                    client["client"].pop("multipath", None)
                server_config, client_config = output / f"{label}-{repeat}-server.json", output / f"{label}-{repeat}-client.json"
                server_config.write_text(json.dumps(server, indent=2) + "\n")
                client_config.write_text(json.dumps(client, indent=2) + "\n")
                server_config.chmod(0o600); client_config.chmod(0o600)
                server_log = (output / f"{label}-{repeat}-server.log").open("w")
                client_log = (output / f"{label}-{repeat}-client.log").open("w")
                handles.extend((server_log, client_log))
                baseline_mux_args = [] if multipath else ["--tun-mux=1"]
                server_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["server"], str(run_binary),
                    "--mode=server", f"--config={server_config}", *baseline_mux_args], stdout=server_log,
                    stderr=subprocess.STDOUT)
                time.sleep(args.startup_wait)
                if server_process.poll() is not None:
                    raise HarnessError(f"candidate server exited; inspect {output / f'{label}-{repeat}-server.log'}")
                client_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], str(run_binary),
                    "--mode=proxy", f"--config={client_config}", "--tun-host=no", "--tun-vnet=no",
                    *baseline_mux_args, "--proxy-subnet=yes", "--proxy-internet=yes",
                    "--tun-protect=no", "--tun-route=no"], stdout=client_log,
                    stderr=subprocess.STDOUT)
                endpoints = {(p["server_ip"], server_port) for p in selected_paths}
                deadline = time.monotonic() + args.startup_timeout
                def mux_ready() -> bool:
                    for log_path in (output / f"{label}-{repeat}-client.log",
                                     output / f"{label}-{repeat}-server.log"):
                        try:
                            log_text = log_path.read_text(errors="replace")
                            if "[mux]" in log_text and "link open" in log_text:
                                return True
                        except OSError:
                            pass
                    return False
                while time.monotonic() < deadline:
                    carriers_ready = endpoints <= established_path_endpoints(lab.names["client"])
                    if carriers_ready and mux_ready():
                        break
                    if server_process.poll() is not None or client_process.poll() is not None:
                        raise HarnessError(f"candidate PPP process exited; inspect {output}/*.log")
                    time.sleep(0.1)
                if not endpoints <= established_path_endpoints(lab.names["client"]):
                    raise HarnessError(f"carrier readiness timed out for {label} trial {repeat}")
                if not mux_ready():
                    raise HarnessError(f"VMUX link-open readiness timed out for {label} trial {repeat}")
                before = lab.path_egress_bytes()
                transfer_path = output / f"{label}-{repeat}-transfer.json"
                transfer_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], sys.executable,
                    "-c", _transfer_driver(), "11080", selected_paths[0]["server_ip"], str(echo_port), str(args.payload_bytes), str(args.timeout),
                    str(args.latency_probes), str(transfer_path), f"{label}-{repeat}"],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    _, transfer_error = transfer_process.communicate(timeout=args.timeout + 10)
                except subprocess.TimeoutExpired as exc:
                    terminate_process_group(transfer_process)
                    children.remove(transfer_process)
                    raise HarnessError(f"{label} application transfer exceeded timeout") from exc
                children.remove(transfer_process)
                if transfer_process.returncode:
                    raise HarnessError(f"{label} application transfer failed: {transfer_error.strip()}")
                after = lab.path_egress_bytes()
                measurement = json.loads(transfer_path.read_text())
                if measurement["sent_bytes"] != measurement["received_bytes"] or measurement["sent_sha256"] != measurement["received_sha256"]:
                    raise HarnessError(f"{label} exact-byte or SHA-256 check failed")
                path_bytes = {p["id"]: max(0, after[p["id"]] - before[p["id"]]) for p in lab.paths}
                floor = max(16384, args.payload_bytes // 1000)
                if any(path_bytes[p["id"]] < floor for p in selected_paths):
                    raise HarnessError(f"{label} path did not carry bulk application traffic: {path_bytes}")
                trials.append({**measurement, "path_egress_bytes": path_bytes})
                terminate_process_group(client_process)
                children.remove(client_process)
                terminate_process_group(server_process)
                children.remove(server_process)
            latencies = [value for trial in trials for value in trial["latency_samples_ms"]]
            rates = [trial["throughput_mbps"] for trial in trials]
            return {"trials": trials, "median_throughput_mbps": statistics.median(rates),
                    "latency_ms": {"p50": _percentile(latencies, .50), "p95": _percentile(latencies, .95),
                                   "p99": _percentile(latencies, .99)}}

        baseline = {path["id"]: measure(f"single-{path['id']}", [path], False) for path in lab.paths}
        best_id = max(baseline, key=lambda path_id: baseline[path_id]["median_throughput_mbps"])
        multi = measure("multipath", lab.paths, True)
        best_single = baseline[best_id]
        ratio = multi["median_throughput_mbps"] / best_single["median_throughput_mbps"]
        p99_ratio = multi["latency_ms"]["p99"] / max(best_single["latency_ms"]["p99"], 0.001)
        result = {"kind": "production-multipath-comparison", "profile": args.profile,
                  "payload_bytes_per_trial": args.payload_bytes, "repeats": args.repeats,
                  "latency_probes_per_trial": args.latency_probes,
                  "baseline_single_path": baseline, "best_single_path_id": best_id,
                  "best_single_median_mbps": best_single["median_throughput_mbps"],
                  "multipath": multi, "throughput_ratio_to_best_single": ratio,
                  "p99_latency_ratio_to_best_single": p99_ratio,
                  "throughput_gate": {"minimum_ratio": args.min_throughput_ratio,
                                      "passed": ratio >= args.min_throughput_ratio},
                  "latency_gate": {"maximum_p99_ratio": args.max_p99_ratio,
                                   "passed": p99_ratio <= args.max_p99_ratio},
                  "topology": str(topology_path),
                  "candidate": {"path": str(run_binary), "sha256": binary_sha256},
                  "evidence_note": "SOCKS5 CONNECT targets the server namespace business IP under client policy-v2 default proxy; this follows the VMUX tunnel path. All configured paths must carry qdisc byte deltas during successful application transfers; established sockets are startup readiness only",
                  "scope_note": "does not exercise outage recovery, full reconnect, RST, or resource ceilings"}
        result["passed"] = result["throughput_gate"]["passed"] and result["latency_gate"]["passed"]
        result_path = output / "production-comparison.json"
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        print(f"{'PASS' if result['passed'] else 'FAIL'}: repeated candidate PPP comparison; result={result_path}")
        return 0 if result["passed"] else 1
    finally:
        previous = block_interrupt_signals()
        try:
            for child in reversed(children):
                terminate_process_group(child)
            for handle in handles:
                handle.close()
            if secret_path is not None:
                secret_path.unlink(missing_ok=True)
            lab.cleanup()
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def impairment(args: argparse.Namespace) -> int:
    topology = load_owned_topology(Path(args.topology))
    matches = [p for p in topology["paths"] if p["id"] == args.path]
    if len(matches) != 1:
        raise HarnessError(f"unknown path: {args.path}")
    path = matches[0]
    apply_path_impairment(path, args.action,
                          float(args.loss.rstrip("%")) if isinstance(args.loss, str) else args.loss)
    print(f"applied {args.action} to {args.path}; the path TBF rate cap remains configured")
    return 0


def apply_path_impairment(path: dict, action: str, loss_percent: float = 5) -> None:
    if not 0 <= loss_percent <= 100:
        raise HarnessError("loss percentage must be between 0 and 100")
    for ns, dev in ((path["client_ns"], path["client_if"]),
                    (path["router_ns"], path["router_client_if"])):
        if action == "reset":
            Lab._netem(ns, dev, path["rtt_ms"], path["jitter_ms"], path["loss_percent"])
        elif action == "blackhole":
            Lab._netem(ns, dev, path["rtt_ms"], path["jitter_ms"], 100)
        elif action == "drop":
            Lab._netem(ns, dev, path["rtt_ms"], path["jitter_ms"], loss_percent)
        else:
            raise HarnessError(f"unknown impairment action: {action}")


def load_owned_topology(path: Path) -> dict:
    try:
        topology = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise HarnessError(f"cannot read topology: {exc}") from exc
    if not isinstance(topology, dict) or topology.get("schema") != 1 or topology.get("profile") not in PROFILES:
        raise HarnessError("unsupported topology schema or profile")
    namespaces = topology.get("namespaces")
    if not isinstance(namespaces, dict):
        raise HarnessError("topology namespaces are missing")
    suffixes = set()
    for role in ("client", "router", "bottleneck", "server"):
        name = namespaces.get(role)
        match = re.fullmatch(rf"mp{role[0]}([0-9a-f]{{8}})", name or "")
        if not match:
            raise HarnessError("fault tools accept only harness-named private namespaces")
        suffixes.add(match.group(1))
    if len(suffixes) != 1:
        raise HarnessError("topology namespace names do not share one run id")
    paths = topology.get("paths")
    expected_paths = PROFILES[topology["profile"]]["paths"]
    if not isinstance(paths, list) or len(paths) != len(expected_paths):
        raise HarnessError("topology path list does not match its profile")
    for index, entry in enumerate(paths, 1):
        expected = {"id": f"path{index}", "client_ns": namespaces["client"],
                    "router_ns": namespaces["router"], "bottleneck_ns": namespaces["bottleneck"],
                    "server_ns": namespaces["server"], "client_if": f"c{index}",
                    "router_client_if": f"rc{index}"}
        if not isinstance(entry, dict) or any(entry.get(key) != value for key, value in expected.items()):
            raise HarnessError("topology path interface ownership does not match the harness layout")
    return topology


def verify_fault_rate_preservation(lab: Lab) -> None:
    path = lab.paths[0]
    interfaces = ((path["client_ns"], path["client_if"]),
                  (path["router_ns"], path["router_client_if"]))
    # Verify qdisc configuration rate, not traffic counters, across replacement.
    def configured_rate(ns: str, dev: str) -> int:
        records = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-d", "qdisc", "show", "dev", dev], capture=True))
        record = next(item for item in records if item.get("kind") == "tbf")
        return int(record["options"]["rate"])
    expected = [configured_rate(ns, dev) for ns, dev in interfaces]
    try:
        apply_path_impairment(path, "blackhole")
        for (ns, dev), rate in zip(interfaces, expected):
            records = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-d", "qdisc", "show", "dev", dev], capture=True))
            tbf = next((item for item in records if item.get("kind") == "tbf"), None)
            netem = next((item for item in records if item.get("kind") == "netem"), None)
            if not tbf or int(tbf["options"]["rate"]) != rate:
                raise HarnessError(f"blackhole replacement changed TBF path cap on {ns}/{dev}")
            if not netem or netem.get("options", {}).get("loss-random", {}).get("loss") != 1:
                raise HarnessError(f"blackhole replacement did not install 100% netem loss on {ns}/{dev}")
    finally:
        apply_path_impairment(path, "reset")
    lab.verify()


def verify_fault_plan_controls(lab: Lab) -> None:
    with tempfile.TemporaryDirectory(prefix="openppp2-multipath-fault.") as directory:
        topology_path = Path(directory) / "topology.json"
        topology_path.write_text(json.dumps(lab.topology()))
        topology_path.chmod(0o600)
        cases = (
            ("single-path-outage", "path2", 50, 1000),
            ("primary-path-outage", None, 50, 1000),
            ("all-short-outage", None, 50, 1000),
            ("all-grace-expired", None, 1001, 1000),
        )
        for case, path, outage, grace in cases:
            args = argparse.Namespace(topology=str(topology_path), case=case,
                                      path=path, outage_ms=outage, grace_ms=grace)
            with contextlib.redirect_stdout(io.StringIO()):
                if fault_plan(args) != 0:
                    raise HarnessError(f"fault-plan infrastructure case failed: {case}")
        # Grace-expired intentionally leaves links blocked; fixture teardown follows later.
        for path in lab.paths:
            apply_path_impairment(path, "reset")
        lab.verify()


def fault_plan(args: argparse.Namespace) -> int:
    topology = load_owned_topology(Path(args.topology))
    paths = topology["paths"]
    by_id = {path["id"]: path for path in paths}
    if args.case == "single-path-outage":
        selected = [args.path or paths[-1]["id"]]
    elif args.case == "primary-path-outage":
        selected = [args.path or paths[0]["id"]]
    else:
        selected = list(by_id)
    unknown = set(selected) - set(by_id)
    if unknown:
        raise HarnessError(f"unknown fault-plan path ids: {sorted(unknown)}")
    if args.outage_ms <= 0 or args.grace_ms <= 0:
        raise HarnessError("outage and grace durations must be positive")
    expires = args.case == "all-grace-expired"
    if args.case == "all-short-outage" and args.outage_ms >= args.grace_ms:
        raise HarnessError("all-short-outage duration must be less than session grace")
    if expires and args.outage_ms <= args.grace_ms:
        raise HarnessError("all-grace-expired duration must exceed session grace")
    started = time.monotonic()
    try:
        for path_id in selected:
            apply_path_impairment(by_id[path_id], "blackhole")
        print(json.dumps({"event": "blackhole", "paths": selected, "elapsed_ms": 0}))
        time.sleep(args.outage_ms / 1000)
    except BaseException:
        for path_id in selected:
            apply_path_impairment(by_id[path_id], "reset")
        raise
    if not expires:
        for path_id in selected:
            apply_path_impairment(by_id[path_id], "reset")
        print(json.dumps({"event": "reset", "paths": selected,
                          "elapsed_ms": round((time.monotonic() - started) * 1000)}))
    else:
        print(json.dumps({"event": "grace-expired", "paths": selected,
                          "elapsed_ms": round((time.monotonic() - started) * 1000),
                          "note": "all paths remain blackholed until namespace cleanup"}))
    return 0


def main() -> int:
    def interrupt(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("scenario")
    run_parser.add_argument("--scenario", nargs="+", required=True)
    run_parser.add_argument("--name", default="rtt-5-80-200")
    run_parser.add_argument("--profile", choices=PROFILES, default="asymmetric-5-80-200")
    run_parser.add_argument("--output", required=True)
    run_parser.add_argument("--timeout", type=float, default=300)
    run_parser.add_argument("--completion-timeout-ms", type=int, default=120000)
    run_parser.add_argument("--recovery-timeout-ms", type=int, default=30000)
    run_parser.add_argument("--reconnect-timeout-ms", type=int, default=30000)
    run_parser.add_argument("--min-ratio", type=float, default=0.8)
    run_parser.add_argument("--max-p95-ms", type=float)
    run_parser.add_argument("--max-p99-ms", type=float)
    run_parser.set_defaults(func=scenario)
    fault_parser = sub.add_parser("impair")
    fault_parser.add_argument("--topology", required=True)
    fault_parser.add_argument("--path", required=True)
    fault_parser.add_argument("--action", choices=("blackhole", "drop", "reset"), required=True)
    fault_parser.add_argument("--loss", default="5%")
    fault_parser.set_defaults(func=impairment)
    plan_parser = sub.add_parser("fault-plan", help="schedule outages while a scenario is active")
    plan_parser.add_argument("--topology", required=True)
    plan_parser.add_argument("--case", choices=("single-path-outage", "primary-path-outage",
                           "all-short-outage", "all-grace-expired"), required=True)
    plan_parser.add_argument("--path")
    plan_parser.add_argument("--outage-ms", type=int, default=1500)
    plan_parser.add_argument("--grace-ms", type=int, default=30000)
    plan_parser.set_defaults(func=fault_plan)
    smoke_parser = sub.add_parser("production-smoke", help="run candidate PPP inside private namespaces")
    smoke_parser.add_argument("--binary", default=str(Path(__file__).resolve().parents[3] / "bin/ppp"))
    smoke_parser.add_argument("--profile", choices=PROFILES, default="asymmetric-5-80-200")
    smoke_parser.add_argument("--output", required=True)
    smoke_parser.add_argument("--payload-bytes", type=int, default=4 * 1024 * 1024)
    smoke_parser.add_argument("--timeout", type=float, default=60)
    smoke_parser.add_argument("--startup-timeout", type=float, default=15)
    smoke_parser.add_argument("--startup-wait", type=float, default=2)
    smoke_parser.add_argument("--repeats", type=int, default=3)
    smoke_parser.add_argument("--latency-probes", type=int, default=10)
    smoke_parser.add_argument("--min-throughput-ratio", type=float, default=0.8)
    smoke_parser.add_argument("--max-p99-ratio", type=float, default=2.0)
    smoke_parser.set_defaults(func=production_smoke)
    infra_parser = sub.add_parser("infrastructure-check", help="fixture capability check; not production acceptance")
    infra_parser.add_argument("--profile", choices=PROFILES, default="shared-5-80-200")
    args = parser.parse_args()
    try:
        if args.command == "infrastructure-check":
            lab = Lab(args.profile)
            try:
                lab.setup()
                lab.verify()
                verify_fault_rate_preservation(lab)
                verify_fault_plan_controls(lab)
                print("PASS: isolated fixture setup and impairment capabilities verified; not production acceptance")
            finally:
                lab.cleanup()
            return 0
        return args.func(args)
    except (HarnessError, OSError, json.JSONDecodeError) as exc:
        print(f"multipath harness error: {exc}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as exc:
        print(f"multipath harness error: timed out running {exc.cmd}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("multipath harness interrupted; owned process groups and namespaces were cleaned", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
