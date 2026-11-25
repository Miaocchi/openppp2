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
import threading
import time
import uuid
import copy
import re
import random
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


TBF_BURST_KB_CHOICES = (128, 256, 512, 1024)
DEFAULT_TBF_BURST_KB = 128
CONGESTION_CONTROL_CHOICES = ("bbr", "cubic")
# IPv4 total length below this is a pure ACK or a small control frame. Bulk
# data frames are near the 1500-byte veth MTU, so 256 bytes separates them
# even with TCP timestamps/SACK options.
ACK_PRIORITY_MAX_IP_BYTES = 256

PROFILES = {
    "independent-12ms-24mbit": {"paths": [(12, 24, 0, 0)] * 2, "shared_mbps": None},
    "shared-12ms-24mbit": {"paths": [(12, 24, 0, 0)] * 2, "shared_mbps": 24},
    "independent-90-125-180": {
        "paths": [(90, 100, 0, 0), (125, 200, 25, 0), (180, 300, 0, 0)],
        "shared_mbps": None,
        "jitter_distribution": "uniform",
    },
    "independent-90-125-180-stable": {
        "paths": [(90, 100, 0, 0), (125, 200, 0, 0), (180, 300, 0, 0)],
        "shared_mbps": None,
    },
    "intent-50-80-155": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None,
        "jitter_distribution": "uniform",
        "rate_jitter_percent": 5,
    },
    "intent-strict-narrow-p1": {
        "paths": [(35, 15, 1.75, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "uniform", "rate_jitter_percent": 5,
    },
    "intent-strict-role-flip": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "uniform", "rate_jitter_percent": 5,
    },
    "intent-strict-credit-blackhole": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "uniform", "rate_jitter_percent": 5,
    },
    "intent-strict-grayzone": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "uniform", "rate_jitter_percent": 5,
    },
    "intent-strict-burst-kill": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "normal", "rate_jitter_percent": 0,
        "netem": {"loss_model": "gemodel", "gemodel": (2, 20, 70, 0.1),
                   "reorder": (2, 50)},
    },
    "intent-strict-dynamic-tide": {
        "paths": [(50, 50, 2.5, 0), (80, 300, 4.0, 0), (155, 700, 7.75, 0)],
        "shared_mbps": None, "jitter_distribution": "uniform", "rate_jitter_percent": 5,
    },
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


def established_path_endpoints(namespace: str) -> Counter[tuple[str, int]]:
    output = run(["ip", "netns", "exec", namespace, "ss", "-Htn", "state", "established"], capture=True)
    endpoints: Counter[tuple[str, int]] = Counter()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        host, port = fields[-1].rsplit(":", 1)
        try:
            endpoints[host.strip("[]"), int(port)] += 1
        except ValueError:
            continue
    return endpoints


class Lab:
    def __init__(self, profile: str, tbf_burst_kb: int = DEFAULT_TBF_BURST_KB,
                 ack_priority: bool = False, congestion_control: str | None = None):
        if tbf_burst_kb not in TBF_BURST_KB_CHOICES:
            raise HarnessError(f"TBF burst must be one of {TBF_BURST_KB_CHOICES} KiB")
        if congestion_control is not None and congestion_control not in CONGESTION_CONTROL_CHOICES:
            raise HarnessError(f"congestion control must be one of {CONGESTION_CONTROL_CHOICES}")
        self.profile_name = profile
        self.profile = PROFILES[profile]
        self.rate_kind = "htb" if profile.startswith("intent-strict-") else "tbf"
        self.tbf_burst_kb = tbf_burst_kb
        self.ack_priority = bool(ack_priority)
        self.congestion_control = congestion_control
        self.specs = self.profile["paths"]
        self.rate_jitter_percent = float(self.profile.get("rate_jitter_percent", 0))
        self.rate_jitter = random.Random(0x4D5558)
        self.suffix = uuid.uuid4().hex[:8]
        self.names = {key: f"mp{key[0]}{self.suffix}" for key in ("client", "router", "bottleneck", "server")}
        self.created_ns: list[str] = []
        self.root_links: dict[str, tuple[int, str, bool]] = {}
        self.paths: list[dict] = []
        self.firewall_rules: list[tuple[str, list[str]]] = []

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
                self._shape(ns, dev, rtt, rate, jitter, loss,
                            self.profile.get("jitter_distribution", "normal"),
                            self.tbf_burst_kb, self.ack_priority, self.rate_kind)
            self.paths.append({"id": f"path{index}", "rtt_ms": rtt, "jitter_ms": jitter,
                               "jitter_distribution": self.profile.get("jitter_distribution", "normal"),
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
            self._rate(self.names["bottleneck"], "bs0", shared, 2 * self.tbf_burst_kb)
            self._rate(self.names["server"], "s0", shared, 2 * self.tbf_burst_kb)

    def apply_congestion_control(self) -> None:
        """Select the TCP congestion control algorithm inside the fixture.

        net.ipv4.tcp_congestion_control is a per-network-namespace sysctl, so
        these writes never reach the host. The fixture starts the candidate
        endpoints after this call, so every transport socket they create uses
        the selected algorithm. A no-op unless the A/B option was requested.
        """
        if self.congestion_control is None:
            return
        for role in ("client", "router", "server"):
            namespace = self.names[role]
            run(["ip", "netns", "exec", namespace, "sysctl", "-q", "-w",
                 f"net.ipv4.tcp_congestion_control={self.congestion_control}"])
            observed = run(["ip", "netns", "exec", namespace, "sysctl", "-n",
                            "net.ipv4.tcp_congestion_control"], capture=True).strip()
            if observed != self.congestion_control:
                raise HarnessError(f"{namespace} tcp_congestion_control readback is {observed!r}, "
                                   f"expected {self.congestion_control!r}")

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
               loss_percent: float = 0, jitter_distribution: str = "normal",
               tbf_burst_kb: int = DEFAULT_TBF_BURST_KB,
               ack_priority: bool = False, rate_kind: str = "tbf") -> None:
        # The round trip crosses four shaped path egresses.
        Lab._path_rate(ns, dev, rate, tbf_burst_kb=tbf_burst_kb, rate_kind=rate_kind)
        if ack_priority:
            Lab._ack_priority_shape(ns, dev, rtt, jitter, loss_percent, jitter_distribution)
        else:
            Lab._netem(ns, dev, rtt, jitter, loss_percent, jitter_distribution)

    @staticmethod
    def _path_rate(ns: str, dev: str, rate: float, action: str = "replace",
                   tbf_burst_kb: int = DEFAULT_TBF_BURST_KB, rate_kind: str = "tbf") -> None:
        tc = ["ip", "netns", "exec", ns, "tc"]
        if rate_kind == "htb":
            if action == "replace":
                run([*tc, "qdisc", "replace", "dev", dev, "root", "handle", "1:", "htb", "default", "1"])
            run([*tc, "class", action, "dev", dev, "parent", "1:", "classid", "1:1", "htb",
                 "rate", f"{rate:g}mbit", "ceil", f"{rate:g}mbit", "quantum", "1514",
                 "burst", f"{tbf_burst_kb}kb", "cburst", f"{tbf_burst_kb}kb"])
            return
        run([*tc, "qdisc", action, "dev", dev, "root", "handle", "1:", "tbf",
             "rate", f"{rate:g}mbit", "burst", f"{tbf_burst_kb}kb", "latency", "500ms"])

    @staticmethod
    def _ack_priority_shape(ns: str, dev: str, rtt: float, jitter: float = 0,
                            loss_percent: float = 0,
                            jitter_distribution: str = "normal") -> None:
        """Keep the TBF cap and path delay, but service small frames first.

        The root TBF still owns the aggregate rate. Its child is a three-band
        prio qdisc; band 0 holds pure ACKs/small control frames and band 1
        holds bulk data. Each band has an identical netem so the configured
        delay/loss stays on every packet. A pure ACK therefore does not wait
        behind bulk frames that are waiting for TBF tokens on the same egress.
        Diagnostic A/B only: the default layout remains TBF + one netem.
        """
        run(["ip", "netns", "exec", ns, "tc", "qdisc", "add", "dev", dev,
             "parent", "1:1", "handle", "20:", "prio", "bands", "3",
             "priomap", *(["1"] * 16)])
        Lab._netem(ns, dev, rtt, jitter, loss_percent, jitter_distribution,
                   parent="20:1", handle="30:")
        Lab._netem(ns, dev, rtt, jitter, loss_percent, jitter_distribution,
                   parent="20:2", handle="40:")
        run(["ip", "netns", "exec", ns, "tc", "filter", "add", "dev", dev,
             "parent", "20:", "protocol", "ip", "prio", "1", "basic", "match",
             f"cmp(u16 at 2 layer network mask 0xffff lt {ACK_PRIORITY_MAX_IP_BYTES})",
             "flowid", "20:1"])
        run(["ip", "netns", "exec", ns, "tc", "filter", "add", "dev", dev,
             "parent", "20:", "protocol", "ip", "prio", "2", "u32",
             "match", "u32", "0", "0", "flowid", "20:2"])

    @staticmethod
    def _netem(ns: str, dev: str, rtt: float, jitter: float = 0,
               loss_percent: float = 0, jitter_distribution: str = "normal",
               parent: str = "1:1", handle: str = "10:", action: str = "replace",
               extra_options: dict | None = None) -> None:
        command = ["ip", "netns", "exec", ns, "tc", "qdisc", action, "dev", dev,
                   "parent", parent, "handle", handle, "netem", "delay", f"{rtt / 4:g}ms"]
        if jitter:
            command.append(f"{jitter / 4:g}ms")
            # netem uses uniform jitter when no distribution table is supplied.
            if jitter_distribution == "normal":
                command.extend(("distribution", "normal"))
        if loss_percent:
            command.extend(("loss", f"{loss_percent:g}%"))
        if extra_options:
            model = extra_options.get("loss_model")
            gemodel = extra_options.get("gemodel")
            reorder = extra_options.get("reorder")
            if model == "gemodel" and gemodel:
                command.extend(("loss", "gemodel", *(f"{value:g}%" for value in gemodel)))
            if reorder:
                command.extend(("reorder", f"{reorder[0]:g}%", f"{reorder[1]:g}%"))
        command.extend(("limit", "32768"))
        run(command)

    @staticmethod
    def _apply_path_delay(ns: str, dev: str, rtt: float, jitter: float = 0,
                          loss_percent: float = 0, jitter_distribution: str = "normal",
                          ack_priority: bool = False) -> None:
        """Update the delay/loss qdiscs without replacing the root TBF cap."""
        if ack_priority:
            for parent, handle in (("20:1", "30:"), ("20:2", "40:")):
                Lab._netem(ns, dev, rtt, jitter, loss_percent, jitter_distribution,
                           parent=parent, handle=handle, action="change")
        else:
            Lab._netem(ns, dev, rtt, jitter, loss_percent, jitter_distribution, action="change")

    @staticmethod
    def _rate(ns: str, dev: str, rate: int, burst_kb: int = 2 * DEFAULT_TBF_BURST_KB) -> None:
        run(["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev,
             "root", "tbf", "rate", f"{rate}mbit", "burst", f"{burst_kb}kb", "latency", "500ms"])

    def topology(self) -> dict:
        topology = {"schema": 1, "profile": self.profile_name, "namespaces": self.names,
                    "shared_bottleneck_mbps": self.profile["shared_mbps"],
                    "shared_bottleneck_interfaces": ["bottleneck/bs0", "server/s0"] if self.profile["shared_mbps"] else [],
                    "paths": self.paths,
                    "note": "Paths converge on one server-facing link; shared profile caps this aggregate link."}
        if self.rate_jitter_percent:
            topology["rate_jitter_percent"] = self.rate_jitter_percent
        if self.ack_priority:
            # Only the non-default mode adds a key, so baseline topology files
            # stay byte-identical.
            topology["ack_priority"] = True
        if self.congestion_control is not None:
            topology["congestion_control"] = self.congestion_control
        if self.rate_kind != "tbf":
            topology["rate_kind"] = self.rate_kind
        if "netem" in self.profile:
            topology["netem"] = self.profile["netem"]
        return topology

    def apply_rate_jitter(self, excluded_paths=()) -> None:
        """Apply one bounded rate sample while keeping each path's delay qdisc."""
        if self.rate_jitter_percent <= 0:
            return
        for path, spec in zip(self.paths, self.specs):
            if path["id"] in excluded_paths:
                continue
            base_rate = float(spec[1])
            variation = self.rate_jitter.uniform(-self.rate_jitter_percent,
                                                  self.rate_jitter_percent) / 100.0
            rate = max(1.0, base_rate * (1.0 + variation))
            for ns, dev in ((path["client_ns"], path["client_if"]),
                            (path["router_ns"], path["router_client_if"]),
                            (path["router_ns"], path["router_bottleneck_if"]),
                            (path["bottleneck_ns"], path["bottleneck_router_if"])):
                self._path_rate(ns, dev, round(rate, 3), "change", self.tbf_burst_kb, self.rate_kind)

    def enable_baseline_loss(self) -> None:
        options = self.profile.get("netem")
        if not options:
            return
        for path in self.paths:
            for ns, interface in ((path["client_ns"], path["client_if"]),
                                  (path["router_ns"], path["router_client_if"])):
                handles = (("20:1", "30:"), ("20:2", "40:")) if self.ack_priority else (("1:1", "10:"),)
                for parent, handle in handles:
                    self._netem(ns, interface, path["rtt_ms"], path["jitter_ms"],
                                jitter_distribution=path["jitter_distribution"],
                                parent=parent, handle=handle, extra_options=options)

    def change_path_action(self, path: dict, action: str, *, restore: bool = False,
                           value: int | None = None) -> None:
        """Apply a live impairment while retaining the root 1: TBF and child 10: handle."""
        if action == "drop_carrier":
            raise HarnessError("drop_carrier is applied by the socket-aware scheduler")
        egresses = ((path["client_ns"], path["client_if"]),
                    (path["router_ns"], path["router_client_if"]))
        if action == "throttle":
            rate = path["rate_mbps"] if restore else value
            if rate is None or rate <= 0:
                raise HarnessError("throttle requires a positive rate")
            for namespace, interface in egresses:
                self._path_rate(namespace, interface, rate, "change", self.tbf_burst_kb, self.rate_kind)
            return
        jitter = path["jitter_ms"]
        distribution = path["jitter_distribution"]
        if action == "blackhole":
            target_rtt = path["rtt_ms"]
            target_loss = path["loss_percent"] if restore else 100
        elif action == "degrade":
            target_rtt = path["rtt_ms"] if restore else 4 * int(value or 140)
            target_loss = path["loss_percent"] if restore else 3
            if not restore:
                jitter = 28
                distribution = "normal"
        elif action == "restore":
            target_rtt = path["rtt_ms"]
            target_loss = path["loss_percent"]
        else:
            raise HarnessError(f"unsupported dynamic action {action}")
        for namespace, interface in egresses:
            self._apply_path_delay(namespace, interface, target_rtt, jitter,
                                   target_loss, distribution, self.ack_priority)

    def cleanup(self) -> None:
        previous = block_interrupt_signals()
        try:
            for namespace, rule in reversed(self.firewall_rules):
                run(["ip", "netns", "exec", namespace, "iptables", "-D", "OUTPUT", *rule], check=False)
            self.firewall_rules.clear()
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
                root = next((item for item in records if item.get("kind") == self.rate_kind), None)
                if root is None:
                    raise HarnessError(f"expected a root {self.rate_kind} qdisc on {ns}/{dev}")
                rate_options = root.get("options", {})
                if self.rate_kind == "htb":
                    classes = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-d",
                                              "class", "show", "dev", dev], capture=True))
                    traffic_class = next((item for item in classes if item.get("handle") == "1:1"), {})
                    rate_options = traffic_class.get("options", traffic_class)
                expected_rate = path["rate_mbps"] * 1_000_000 / 8
                if abs(rate_options.get("rate", -1) - expected_rate) > 1:
                    raise HarnessError(f"path rate cap readback mismatch on {ns}/{dev}: "
                                       f"{rate_options.get('rate')!r} != {expected_rate:g} bytes/s")
                if self.ack_priority:
                    if not any(item.get("kind") == "prio" for item in records):
                        raise HarnessError(f"expected a prio qdisc on {ns}/{dev}")
                    expected_parents = ("20:1", "20:2")
                else:
                    expected_parents = ("1:1",)
                netems = {item.get("parent"): item for item in records
                          if item.get("kind") == "netem"}
                for parent in expected_parents:
                    netem = netems.get(parent)
                    if netem is None:
                        raise HarnessError(f"missing netem qdisc at {parent} on {ns}/{dev}")
                    options = netem.get("options", {})
                    delay = options.get("delay", {})
                    if abs(delay.get("delay", -1) - path["rtt_ms"] / 4000) > 1e-6:
                        raise HarnessError(f"netem base delay readback mismatch at {parent} on {ns}/{dev}")
                    if abs(delay.get("jitter", -1) - path["jitter_ms"] / 4000) > 1e-6:
                        raise HarnessError(f"netem jitter readback mismatch at {parent} on {ns}/{dev}")
                    loss = options.get("loss-random", {}).get("loss", 0) * 100
                    if abs(loss - path["loss_percent"]) > 1e-6:
                        raise HarnessError(f"netem loss readback mismatch at {parent} on {ns}/{dev}")
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
            if record.get("kind") not in ("tbf", "htb") or not record.get("root"):
                continue
            for key in ("bytes", "stats64", "stats"):
                value = record.get(key)
                if isinstance(value, int) and value >= 0:
                    return value
                if isinstance(value, dict) and isinstance(value.get("bytes"), int):
                    return value["bytes"]
        raise HarnessError(f"tc did not report root shaper bytes for {namespace}/{interface}")

    def path_egress_bytes(self) -> dict[str, int]:
        counters = {}
        for path in self.paths:
            counters[path["id"]] = sum((
                self._root_tbf_bytes(path["client_ns"], path["client_if"]),
                self._root_tbf_bytes(path["router_ns"], path["router_client_if"]),
            ))
        return counters

    def directional_path_bytes(self, direction: str) -> dict[str, int]:
        ns, dev = ("client_ns", "client_if") if direction == "upload" else ("router_ns", "router_client_if")
        return {path["id"]: self._root_tbf_bytes(path[ns], path[dev]) for path in self.paths}


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
    lab = Lab(args.profile, ack_priority=args.ack_priority)
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


def workspace_evidence(root: Path) -> dict:
    prefix = ["git", "-C", str(root), "-c", f"safe.directory={root}", "-c", "core.fsmonitor=false"]
    commands = [
        ["rev-parse", "--verify", "HEAD"],
        ["status", "--porcelain=v1", "-z", "--untracked-files=normal"],
        ["diff", "--binary", "--no-ext-diff", "--no-textconv", "--no-color", "HEAD", "--"],
    ]
    try:
        commit, status, diff = (subprocess.run(prefix + command, check=True, capture_output=True,
            env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"}).stdout for command in commands)
    except (OSError, subprocess.CalledProcessError):
        return {"available": False}
    return {"available": True, "head": commit.decode().strip(), "dirty": bool(status),
            "status_sha256": hashlib.sha256(status).hexdigest(),
            "tracked_diff_sha256": hashlib.sha256(diff).hexdigest()}


def production_six_flow(args: argparse.Namespace) -> int:
    if args.intent_migration_acceptance and (args.suite or args.strict_profile or
            args.profile != "intent-50-80-155" or not args.fixed_pool or
            args.carriers_per_path not in (2, 3) or args.single_path):
        raise HarnessError("--intent-migration-acceptance requires intent-50-80-155 with 2 or 3 fixed carriers per path")
    if getattr(args, "suite", None) == "intent-strict":
        return intent_strict_suite(args)
    if not getattr(args, "strict_profile", None) and args.profile in STRICT_PROFILE_SPECS:
        args.strict_profile = args.profile
    if getattr(args, "strict_profile", None):
        spec = STRICT_PROFILE_SPECS[args.strict_profile]
        args.profile = args.strict_profile
        args.fixed_pool = spec["fixed_pool"]
        args.carriers_per_path = spec["carriers"]
        args.intent = True
        args.intent_acceptance = False
        args.pool_metrics = True
        args.socket_metrics = True
        args.duration = spec["duration"]
    if args.intent_acceptance or args.intent_migration_acceptance:
        args.intent = True
    source = workspace_evidence(Path(__file__).resolve().parents[3])
    output = Path(args.output).resolve()
    output.mkdir(parents=True, mode=0o700)
    evidence_path = output / "run-evidence.json"
    clock = {"unix_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns()}
    args.evidence = {"schema": 1, "argv": getattr(sys, "orig_argv", [sys.executable, *sys.argv]),
                     "cwd": os.getcwd(), "workspace": source, "clock_reference": clock,
                     "profile": args.profile, "executor_concurrency": args.concurrent,
                     "wire_interval": args.wire_interval,
                     "qdisc_metrics": args.qdisc_metrics,
                     "socket_metrics": args.socket_metrics,
                     "intent": args.intent,
                     "intent_acceptance": args.intent_acceptance,
                     "tbf_burst_kb": args.tbf_burst_kb,
                     "ack_priority": args.ack_priority,
                     "congestion_control": args.congestion_control,
                     "candidate_commands": {}, "exit_code": None}
    if args.intent_migration_acceptance:
        args.evidence["intent_migration_acceptance"] = True
    evidence_path.write_text(json.dumps(args.evidence, indent=2) + "\n")
    code = 1
    try:
        code = production_smoke(args)
    except KeyboardInterrupt:
        code = 130
        raise
    finally:
        summary_errors = {}
        if any((output / f"{direction}-wire.ndjson").is_file() for direction in ("upload", "download")):
            try:
                args.evidence["path_observations"] = record_path_windows(output)
            except (OSError, ValueError, TypeError, KeyError) as exc:
                summary_errors["path_telemetry"] = str(exc)
        if args.pool_metrics:
            try:
                record_pool_events(output, clock)
            except (OSError, ValueError) as exc:
                summary_errors["pool_events"] = str(exc)
        if summary_errors:
            args.evidence["summary_errors"] = summary_errors
            if code == 0:
                print(f"multipath harness error: could not summarize run evidence; inspect {evidence_path}",
                      file=sys.stderr)
                code = 1
        args.evidence["exit_code"] = code
        evidence_path.write_text(json.dumps(args.evidence, indent=2) + "\n")
    if code == 0:
        result_name = "intent-strict.json" if getattr(args, "strict_profile", None) else (
            "intent-migration.json" if args.intent_migration_acceptance else
            "intent-acceptance.json" if args.intent_acceptance else "six-flow-integrity.json")
        print(f"PASS: multipath acceptance; result={output / result_name}")
    return code


PAYLOAD = str(Path(__file__).with_name("payload.py").resolve())
# Server's s0 address. Client namespaces only have /32 routes to the carrier IPs.
BUSINESS_IP = "10.70.0.2"


def check_directional_receipts(lab: Lab, children: list[subprocess.Popen], size: int,
                               timeout: float, trial: str) -> dict:
    # Each endpoint is a one-shot process: in particular the download cannot pass
    # just because the client wrote K. Its sender must actually have read it.
    endpoints = {}
    for role in ("server", "client"):
        endpoints[role] = spawn_owned(children, ["ip", "netns", "exec", lab.names[role], sys.executable,
            PAYLOAD, role, BUSINESS_IP, "19003", str(size), str(timeout), trial],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    reports = {}
    for role in ("client", "server"):
        process = endpoints[role]
        out, err = process.communicate(timeout=timeout + 10)
        children.remove(process)
        if process.returncode:
            raise HarnessError(f"{trial} {role} directional check failed: {err.strip()}")
        reports[role] = json.loads(out)
    return {name: {**reports["client"][tag], **reports["server"][tag]}
            for tag, name in (("U", "upload"), ("D", "download"))}


def change_path_load(path: dict, action: str, value: int | None, restore: bool,
                     tbf_burst_kb: int = DEFAULT_TBF_BURST_KB,
                     ack_priority: bool = False) -> None:
    if action == "blackhole":
        apply_path_impairment(path, "reset" if restore else "blackhole",
                              ack_priority=ack_priority)
        return
    for namespace, interface in ((path["client_ns"], path["client_if"]),
                                 (path["router_ns"], path["router_client_if"])):
        if action == "rate":
            Lab._path_rate(namespace, interface, path["rate_mbps"] if restore else value, "change",
                           tbf_burst_kb)
        else:
            # The two egresses modified here contribute half the path RTT.
            rtt = path["rtt_ms"] + (0 if restore else 2 * value)
            Lab._apply_path_delay(namespace, interface, rtt, path["jitter_ms"],
                                  path["loss_percent"], path["jitter_distribution"],
                                  ack_priority)


QDISC_TBF_COUNTERS = ("bytes", "packets", "drops", "overlimits", "backlog", "qlen")
QDISC_NETEM_COUNTERS = ("packets", "drops")
QDISC_QUEUE_COUNTERS = ("qlen", "backlog")


def qdisc_counters(record: dict | None, fields: tuple[str, ...]) -> dict:
    counters = {}
    if not isinstance(record, dict):
        return counters
    for container in (record, record.get("stats64"), record.get("stats")):
        if not isinstance(container, dict):
            continue
        for field in fields:
            value = container.get(field)
            if field not in counters and type(value) is int and value >= 0:
                counters[field] = value
    return counters


def merged_qdisc_counters(records: list[dict], fields: tuple[str, ...]) -> dict:
    merged: dict[str, int] = {}
    for record in records:
        for field, value in qdisc_counters(record, fields).items():
            merged[field] = merged.get(field, 0) + value
    return merged


def qdisc_metrics_sample(path: dict, directions: tuple[str, ...], sampled_ms: int) -> list[dict]:
    """Read both path egress qdiscs; failures are recorded, never fatal."""
    samples = []
    for role, namespace_key, interface_key in (("client", "client_ns", "client_if"),
                                                ("router", "router_ns", "router_client_if")):
        entry = {"path_id": path["id"], "role": role,
                 "namespace": path[namespace_key], "interface": path[interface_key],
                 "directions": list(directions), "monotonic_ms": sampled_ms,
                 "tbf": {}, "netem": {}}
        try:
            records = json.loads(run(["ip", "netns", "exec", path[namespace_key], "tc", "-j", "-s",
                                      "qdisc", "show", "dev", path[interface_key]], capture=True))
        except (HarnessError, OSError, json.JSONDecodeError) as exc:
            entry["error"] = str(exc)
            samples.append(entry)
            continue
        if not isinstance(records, list):
            entry["error"] = "tc qdisc output is not a JSON list"
            samples.append(entry)
            continue
        tbf = next((record for record in records if record.get("kind") == "tbf"), None)
        prio = next((record for record in records if record.get("kind") == "prio"), None)
        netems = [record for record in records if record.get("kind") == "netem"]
        entry["tbf"] = qdisc_counters(tbf, QDISC_TBF_COUNTERS)
        if len(netems) == 1:
            entry["netem"] = qdisc_counters(netems[0], QDISC_NETEM_COUNTERS)
        elif netems:
            entry["netem"] = merged_qdisc_counters(netems, QDISC_NETEM_COUNTERS)
        if prio is not None:
            # Only present in the ACK-priority A/B mode.
            entry["prio"] = qdisc_counters(prio, QDISC_TBF_COUNTERS)
        if len(netems) > 1:
            # Per-band queue depth proves whether ACKs bypass bulk: band 0
            # (handle 30:, parent 20:1) should stay near empty while band 1
            # (handle 40:, parent 20:2) holds the TBF backlog.
            entry["netem_bands"] = [
                {"parent": record.get("parent"), "handle": record.get("handle"),
                 "counters": qdisc_counters(record, QDISC_NETEM_COUNTERS),
                 "queue": qdisc_counters(record, QDISC_QUEUE_COUNTERS)}
                for record in netems]
        samples.append(entry)
    return samples


def parse_established_sockets(output: str) -> list[dict]:
    """Parse `ss -tinm` output, keeping raw and best-effort structured established records."""
    sockets: list[dict] = []
    current: dict | None = None
    for line in output.splitlines():
        text = line.strip()
        if not text or text.startswith(("State ", "Recv-Q")):
            continue
        if not line[0].isspace():
            fields = line.split()
            current = None
            if not fields or fields[0] != "ESTAB":
                continue
            local = fields[3] if len(fields) > 3 else None
            peer = fields[4] if len(fields) > 4 else None
            current = {"state": fields[0],
                       "recv_q": fields[1] if len(fields) > 1 else None,
                       "send_q": fields[2] if len(fields) > 2 else None,
                       "local": local, "peer": peer,
                       "raw": [line], "detail": [], "fields": {}}
            if local is not None:
                host, _, port = local.rpartition(":")
                current["local_address"], current["local_port"] = host.strip("[]"), port
            if peer is not None:
                host, _, port = peer.rpartition(":")
                current["peer_address"], current["peer_port"] = host.strip("[]"), port
            sockets.append(current)
            continue
        if current is None:
            continue
        text = line.strip()
        current["raw"].append(line)
        current["detail"].append(text)
        for key, value in re.findall(r"(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z0-9_]*):([^\s]+)", text):
            current["fields"].setdefault(key, value)
        for key, value in re.findall(r"(?<!\S)(pacing_rate|delivery_rate)\s+([^\s]+)", text):
            current["fields"].setdefault(key, value)
    return sockets


def socket_path_id(paths: list[dict], role: str, socket: dict) -> str | None:
    """Best-effort carrier-path id for one socket from its local/peer addresses.

    Carrier sockets connect one path's client address to its server address.
    Application or proxy sockets (for example the loopback business stream)
    do not match and stay unattributed.
    """
    local = socket.get("local_address")
    peer = socket.get("peer_address")
    if local is None or peer is None:
        return None
    for path in paths:
        expected = ((path.get("client_ip"), path.get("server_ip")) if role == "client"
                    else (path.get("server_ip"), path.get("client_ip")))
        if (local, peer) == expected:
            return path.get("id")
    return None


def socket_metrics_sample(lab: Lab, directions: tuple[str, ...], sampled_ms: int) -> list[dict]:
    """Read established TCP sockets in both endpoints; failures are recorded, never fatal."""
    samples = []
    for role in ("client", "server"):
        namespace = lab.names[role]
        entry = {"role": role, "namespace": namespace, "directions": list(directions),
                 "monotonic_ms": sampled_ms, "command": ["ss", "-tinm"], "sockets": []}
        try:
            output = run(["ip", "netns", "exec", namespace, "ss", "-tinm"], capture=True)
        except (HarnessError, OSError) as exc:
            entry["error"] = str(exc)
            samples.append(entry)
            continue
        entry["raw"] = output
        try:
            sockets = parse_established_sockets(output)
        except (IndexError, TypeError, ValueError) as exc:
            entry["error"] = f"ss parse failed: {exc}"
            sockets = []
        for socket in sockets:
            path_id = socket_path_id(lab.paths, role, socket)
            if path_id is not None:
                socket["path_id"] = path_id
        entry["sockets"] = sockets
        samples.append(entry)
    return samples


def sample_directional_wire(lab: Lab, output: Path, directions: tuple[str, ...],
                            phase_started: float, send_seconds: int, impairment: dict | None,
                            wire_interval: int = 6, qdisc_metrics: bool = False,
                            socket_metrics: bool = False) -> None:
    # Include the send boundary so the final interval is measurable; consumers
    # must ignore any partial interval shorter than wire_interval.
    sample_offsets = set(range(9, send_seconds + 1, wire_interval))
    faults = {impairment["start"]: False, impairment["stop"]: True} if impairment else {}
    time.sleep(max(0, phase_started + 3 - time.monotonic()))
    before = {direction: lab.directional_path_bytes(direction) for direction in directions}
    started = time.monotonic()
    with contextlib.ExitStack() as handles:
        wires = {direction: handles.enter_context((output / f"{direction}-wire.ndjson").open("w"))
                 for direction in directions}
        metrics = (handles.enter_context((output / "qdisc-metrics.ndjson").open("a"))
                   if qdisc_metrics else None)
        sockets = (handles.enter_context((output / "socket-metrics.ndjson").open("a"))
                   if socket_metrics else None)
        for offset in sorted(sample_offsets | faults.keys()):
            time.sleep(max(0, phase_started + offset - time.monotonic()))
            lab.apply_rate_jitter()
            after = {direction: lab.directional_path_bytes(direction) for direction in directions}
            finished = time.monotonic()
            for direction in directions:
                deltas = {path: (count - before[direction][path] if count >= before[direction][path] else None)
                          for path, count in after[direction].items()}
                print(json.dumps({"start_seconds": started - phase_started,
                                  "end_seconds": finished - phase_started,
                                  "start_monotonic_ms": int(started * 1000),
                                  "end_monotonic_ms": int(finished * 1000),
                                  "start_tbf_bytes": before[direction],
                                  "end_tbf_bytes": after[direction],
                                  "tbf_bytes": deltas,
                                  "mbps": {path: count * 8 / (finished - started) / 1e6 if count is not None else None
                                           for path, count in deltas.items()}}),
                      file=wires[direction], flush=True)
            started, before = finished, after
            sampled_ms = int(time.monotonic() * 1000)
            if metrics is not None:
                for path in lab.paths:
                    for sample in qdisc_metrics_sample(path, directions, sampled_ms):
                        print(json.dumps(sample), file=metrics, flush=True)
            if sockets is not None:
                for sample in socket_metrics_sample(lab, directions, sampled_ms):
                    print(json.dumps(sample), file=sockets, flush=True)
            if offset in faults:
                path, action, value = impairment["path"], impairment["action"], impairment["value"]
                restore = faults[offset]
                change_started = time.monotonic()
                change_path_load(path, action, value, restore, lab.tbf_burst_kb,
                                 lab.ack_priority)
                with (output / "impairment-events.ndjson").open("a") as events:
                    print(json.dumps({"directions": directions, "path_id": path["id"], "action": action,
                                      "value": value, "event": "restored" if restore else "applied",
                                      "start_monotonic_ms": int(change_started * 1000),
                                      "end_monotonic_ms": int(time.monotonic() * 1000)}), file=events)


def read_stats(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    # The candidate may be appending its next sample while we read.
    records = [json.loads(line) for line in path.read_text().splitlines(keepends=True)
               if line.endswith("\n")]
    return [row for row in records if row.get("type") == "ppp-stats"
            and type(row.get("monotonic_ms")) is int]


def stats_at(records: list[dict], boundary_ms: int) -> dict:
    if not records:
        return {}
    # Samples are emitted asynchronously around the requested boundary. Use
    # the nearest complete runtime sample rather than silently accepting an
    # older sample up to two seconds away, which shortens the first window.
    row = min(records, key=lambda item: abs(item["monotonic_ms"] - boundary_ms))
    if abs(row["monotonic_ms"] - boundary_ms) <= 1000:
        return {"sample_ms": row["monotonic_ms"], **row.get("runtime", {}).get("multipath", {})}
    return {}


def path_links(sample: dict, path: str) -> set[tuple[int, int]]:
    return {(link["link_id"], link["generation"]) for link in sample.get("links", [])
            if link.get("path_id") == path}


def session_matches(first: dict, second: dict) -> bool:
    session_id = first.get("vmux_session_id")
    return session_id is not None and session_id == second.get("vmux_session_id")


def carrier_counter_deltas(start: dict, end: dict, link_ids: set[int], fields: tuple[str, ...]) -> dict:
    before = {link["link_id"]: link for link in start.get("links", []) if link["link_id"] in link_ids}
    after = {link["link_id"]: link for link in end.get("links", []) if link["link_id"] in link_ids}
    result = dict.fromkeys(fields)
    if (not link_ids or before.keys() != link_ids or after.keys() != link_ids or
            any(before[link]["generation"] != after[link]["generation"] for link in link_ids)):
        return result
    for field in fields:
        pairs = [(before[link].get(field), after[link].get(field)) for link in link_ids]
        if all(type(first) is int and type(last) is int and last >= first for first, last in pairs):
            result[field] = sum(last - first for first, last in pairs)
    return result


CARRIER_COUNTER_FIELDS = ("tx_bytes", "retransmit_bytes", "retransmit_events",
                          "scheduler_selection_count", "scheduler_probe_selection_count",
                          "scheduler_candidate_observation_count")
CARRIER_STATE_FIELDS = ("queued_bytes", "wire_inflight_bytes", "send_credit_bytes",
                        "scheduler_last_send_credit_bytes", "scheduler_last_wire_inflight_bytes",
                        "scheduler_last_queue_bytes",
                        "scheduler_last_observed_send_credit_bytes",
                        "scheduler_last_observed_wire_inflight_bytes",
                        "scheduler_last_observed_queue_bytes")


def read_socket_metrics(path: Path) -> list[dict]:
    """Read complete socket-metrics records, ignoring a partial or malformed line."""
    if not path.is_file():
        return []
    records = []
    for line in path.read_text().splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(row, dict) and type(row.get("monotonic_ms")) is int:
            records.append(row)
    return records


def _socket_number(socket: dict, name: str) -> int | None:
    value = (socket.get("fields") or {}).get(name)
    if value is None:
        return None
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def _socket_header_number(socket: dict, name: str) -> int | None:
    value = socket.get(name)
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _path_socket_state(sample: dict, role: str, path_id: str,
                       paths: list[dict]) -> dict | None:
    sockets = []
    for socket in sample.get("sockets", []):
        if not isinstance(socket, dict):
            continue
        attributed = socket.get("path_id")
        if attributed is None:
            attributed = socket_path_id(paths, role, socket)
        if attributed == path_id:
            sockets.append(socket)
    if not sockets:
        return None
    state = {"sample_ms": sample["monotonic_ms"], "socket_count": len(sockets),
             "identities": sorted((socket.get("local"), socket.get("peer"))
                                  for socket in sockets)}
    for name in ("notsent", "snd_wnd", "cwnd", "bytes_acked", "bytes_sent"):
        values = [_socket_number(socket, name) for socket in sockets]
        if values and all(value is not None for value in values):
            state[name] = sum(values)
    for name in ("send_q", "recv_q"):
        values = [_socket_header_number(socket, name) for socket in sockets]
        if values and all(value is not None for value in values):
            state[name] = sum(values)
    return state


def _socket_state_at(records: list[dict], role: str, path_id: str, paths: list[dict],
                     boundary_ms: int, tolerance_ms: int = 2500) -> dict | None:
    candidates = [record for record in records
                  if record.get("role") == role and
                  abs(record["monotonic_ms"] - boundary_ms) <= tolerance_ms]
    if not candidates:
        return None
    sample = min(candidates, key=lambda record: abs(record["monotonic_ms"] - boundary_ms))
    return _path_socket_state(sample, role, path_id, paths)


def socket_window_alignment(records: list[dict], role: str, path_id: str, paths: list[dict],
                            start_ms: int, end_ms: int) -> dict:
    """Align one path's sender-side carrier socket counters to a wire window."""
    first = _socket_state_at(records, role, path_id, paths, start_ms)
    last = _socket_state_at(records, role, path_id, paths, end_ms)
    result = {"role": role, "path_id": path_id, "sample_ms": None, "socket_count": 0,
              "notsent_bytes": None, "snd_wnd_bytes": None, "cwnd_segments": None,
              "send_q_bytes": None, "recv_q_bytes": None, "tcp_acked_bytes": None,
              "tcp_acked_delta_bytes": None, "tcp_sent_delta_bytes": None}
    if last is None:
        return result
    result.update({
        "sample_ms": last["sample_ms"],
        "socket_count": last["socket_count"],
        "notsent_bytes": last.get("notsent"),
        "snd_wnd_bytes": last.get("snd_wnd"),
        "cwnd_segments": last.get("cwnd"),
        "send_q_bytes": last.get("send_q"),
        "recv_q_bytes": last.get("recv_q"),
        "tcp_acked_bytes": last.get("bytes_acked"),
    })
    if first is not None and first["identities"] == last["identities"]:
        for source, target in (("bytes_acked", "tcp_acked_delta_bytes"),
                               ("bytes_sent", "tcp_sent_delta_bytes")):
            before, after = first.get(source), last.get(source)
            if type(before) is int and type(after) is int and after >= before:
                result[target] = after - before
    return result


def path_carrier_snapshot(sample: dict, path_id: str) -> dict:
    """Aggregate one path's VMUX carrier links at one stats sample.

    Server snapshots may not carry client path tokens. The fixture creates
    links in path order, so link ids 1..N are a deterministic fallback for
    download diagnostics only.
    """
    links = [link for link in sample.get("links", [])
             if isinstance(link, dict) and
             (link.get("path_id") == path_id or
              (not link.get("path_id") and
               link.get("link_id") == int(path_id.removeprefix("path"))))]
    identity = sorted((link.get("link_id"), link.get("generation")) for link in links)
    values = {}
    for field in CARRIER_COUNTER_FIELDS + CARRIER_STATE_FIELDS:
        candidates = [link.get(field) for link in links]
        if candidates and all(type(value) is int and value >= 0 for value in candidates):
            values[field] = sum(candidates)
    return {"identity": identity, "values": values}


def _carrier_delta(first: dict, last: dict, field: str) -> int | None:
    if not first["identity"] or first["identity"] != last["identity"]:
        return None
    before, after = first["values"].get(field), last["values"].get(field)
    if type(before) is int and type(after) is int and after >= before:
        return after - before
    return None


def vmux_window_alignment(sender_start: dict, sender_end: dict, path_id: str,
                          sender_role: str) -> dict:
    """Align VMUX scheduling and write counters for one path and wire window."""
    first = path_carrier_snapshot(sender_start, path_id)
    last = path_carrier_snapshot(sender_end, path_id)
    submitted = _carrier_delta(first, last, "tx_bytes")
    queued_first = first["values"].get("queued_bytes")
    queued_last = last["values"].get("queued_bytes")
    completed = None
    if (submitted is not None and type(queued_first) is int and type(queued_last) is int):
        completed = submitted - (queued_last - queued_first)
    return {
        "sender": sender_role,
        "path_id": path_id,
        "link_ids": sorted({link_id for link_id, _ in last["identity"]}),
        "stable": bool(first["identity"]) and first["identity"] == last["identity"],
        "selection_count": _carrier_delta(first, last, "scheduler_selection_count"),
        "probe_selection_count": _carrier_delta(first, last, "scheduler_probe_selection_count"),
        "candidate_observation_count": _carrier_delta(first, last, "scheduler_candidate_observation_count"),
        "send_credit_bytes": last["values"].get("send_credit_bytes"),
        "wire_inflight_bytes": last["values"].get("wire_inflight_bytes"),
        "observed_send_credit_bytes": last["values"].get("scheduler_last_observed_send_credit_bytes"),
        "observed_wire_inflight_bytes": last["values"].get("scheduler_last_observed_wire_inflight_bytes"),
        "selected_send_credit_bytes": last["values"].get("scheduler_last_send_credit_bytes"),
        "selected_wire_inflight_bytes": last["values"].get("scheduler_last_wire_inflight_bytes"),
        "write_submitted_bytes": submitted,
        "write_completed_bytes": completed,
        "queued_bytes": queued_last,
    }


def record_path_windows(output: Path) -> dict:
    client = read_stats(output / "client-stats.ndjson")
    server = read_stats(output / "server-stats.ndjson")
    socket_records = read_socket_metrics(output / "socket-metrics.ndjson")
    topology_paths: list[dict] = []
    try:
        topology = json.loads((output / "topology.json").read_text())
        if isinstance(topology, dict) and isinstance(topology.get("paths"), list):
            topology_paths = [path for path in topology["paths"] if isinstance(path, dict)]
    except (OSError, json.JSONDecodeError):
        pass
    observations = {}
    session_ids = set()
    with (output / "path-telemetry.ndjson").open("w") as report:
        for direction in ("upload", "download"):
            wire_file = output / f"{direction}-wire.ndjson"
            if not wire_file.is_file():
                continue
            windows = 0
            shared_windows = 0
            carrying_paths = set()
            max_matched_carriers = {}
            max_concurrent_matches = 0
            sender_role = "client" if direction == "upload" else "server"
            for line in wire_file.read_text().splitlines():
                wire = json.loads(line)
                windows += 1
                start_ms, end_ms = wire["start_monotonic_ms"], wire["end_monotonic_ms"]
                client_start, client_end = stats_at(client, start_ms), stats_at(client, end_ms)
                server_start, server_end = stats_at(server, start_ms), stats_at(server, end_ms)
                shared = (session_matches(client_start, server_start) and session_matches(client_end, server_end)
                          and session_matches(client_start, client_end))
                if shared:
                    shared_windows += 1
                    session_ids.add(client_end["vmux_session_id"])
                server_end_ids = {link["link_id"] for link in server_end.get("links", [])}
                client_pair = (client_start, client_end)
                server_pair = (server_start, server_end)
                tx, rx = (client_pair, server_pair) if direction == "upload" else (server_pair, client_pair)
                paths = {}
                for path, tbf_delta in wire["tbf_bytes"].items():
                    first, last = path_links(client_start, path), path_links(client_end, path)
                    latest_ids = {link_id for link_id, _ in last}
                    ids = latest_ids if shared and first == last and latest_ids <= server_end_ids else set()
                    received = carrier_counter_deltas(*rx, ids, ("rx_unique_payload_bytes",))
                    sent = carrier_counter_deltas(*tx, ids, ("tx_unique_payload_bytes", "retransmit_bytes",
                                                            "retransmit_events"))
                    matched = len(latest_ids & server_end_ids) if shared else None
                    unique = received["rx_unique_payload_bytes"]
                    if matched is not None:
                        max_matched_carriers[path] = max(max_matched_carriers.get(path, 0), matched)
                    if unique:
                        carrying_paths.add(path)
                    socket_alignment = socket_window_alignment(
                        socket_records, sender_role, path, topology_paths, start_ms, end_ms)
                    paths[path] = {
                        "tbf_bytes": tbf_delta,
                        "tbf_start_bytes": (wire.get("start_tbf_bytes") or {}).get(path),
                        "tbf_end_bytes": (wire.get("end_tbf_bytes") or {}).get(path),
                        "unique_vmux_payload_bytes": unique,
                        "matched_carriers": matched,
                        **sent,
                        "vmux": vmux_window_alignment(tx[0], tx[1], path, sender_role),
                        "socket": socket_alignment,
                        "ack_delivery": {
                            "role": sender_role,
                            "vmux_unique_payload_bytes": unique,
                            "tcp_acked_delta_bytes": socket_alignment["tcp_acked_delta_bytes"],
                        },
                    }
                if shared:
                    max_concurrent_matches = max(max_concurrent_matches,
                                                 sum(p["matched_carriers"] for p in paths.values()))
                print(json.dumps({"direction": direction, "start_monotonic_ms": start_ms,
                                  "end_monotonic_ms": end_ms, "same_vmux_session": shared,
                                  "client_start": client_start, "client_end": client_end,
                                  "server_start": server_start, "server_end": server_end, "paths": paths}),
                      file=report)
            observations[direction] = {"windows": windows, "same_session_windows": shared_windows,
                                       "max_matched_carriers": max_matched_carriers,
                                       "max_simultaneous_matched_carriers": max_concurrent_matches,
                                       "paths_with_unique_payload": sorted(carrying_paths)}
    return {"vmux_session_ids": sorted(session_ids), "directions": observations}


def record_pool_events(output: Path, clock: dict) -> None:
    metric = re.compile(r"\[([^\]]+)\] COUNT \[(mux\.pool\.[\w.]+)\].*? delta=(-?\d+)\b")
    with (output / "pool-events.ndjson").open("w") as events:
        log_paths = list(output.glob("strict-*-client.log")) + list(output.glob("strict-*-server.log"))
        log_paths.extend(output / f"six-flow-0-{role}.log" for role in ("client", "server"))
        for log_path in log_paths:
            role = "client" if "client" in log_path.name else "server"
            if not log_path.is_file():
                continue
            with log_path.open(errors="replace") as log:
                for line in log:
                    match = metric.search(line)
                    if match is None:
                        continue
                    timestamp, name, delta = match.groups()
                    wall_ns = int(datetime.strptime(timestamp, "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1e9)
                    estimated_ms = (wall_ns - clock["unix_ns"] + clock["monotonic_ns"]) // 1_000_000
                    print(json.dumps({"role": role, "metric": name, "delta": int(delta),
                                      "local_wall_time": timestamp, "estimated_monotonic_ms": estimated_ms}),
                          file=events)


def six_flow_impairment(args: argparse.Namespace, selected_paths: list[dict]) -> dict | None:
    if args.impairment is None:
        if args.impair_path is not None or args.impair_value is not None:
            raise HarnessError("--impair-path and --impair-value require --impairment")
        return None
    path = next((path for path in selected_paths if path["id"] == args.impair_path), None)
    if path is None or len(selected_paths) < 2 or not args.fixed_pool:
        raise HarnessError("impairment requires a selected --impair-path in a fixed pool with multiple paths")
    start, stop = args.impair_after, args.impair_after + args.impair_for
    default_duration = 25 if args.direction in ("upload", "bidirectional") else 12
    if not 3 < start < stop < (args.duration or default_duration):
        raise HarnessError("impairment must begin after the first wire sample and end before sending ends")
    value = args.impair_value
    if args.impairment == "delay" and (value is None or not 0 < value <= 5000):
        raise HarnessError("delay impairment requires --impair-value of 1..5000 added RTT milliseconds")
    if args.impairment == "rate" and (value is None or not 0 < value < path["rate_mbps"]):
        raise HarnessError("rate impairment requires --impair-value below the original path cap in Mbit/s")
    if args.impairment == "blackhole" and value is not None:
        raise HarnessError("blackhole impairment takes no --impair-value")
    return {"path": path, "action": args.impairment, "value": value, "start": start, "stop": stop}


def six_flow_acceptance(lab: Lab, children: list[subprocess.Popen], output: Path,
                        flow_count: int, duration: int | None, mode: str, impairment: dict | None,
                        wire_interval: int = 6, qdisc_metrics: bool = False,
                        socket_metrics: bool = False) -> dict:
    import six_flow as workload

    expected = set(range(flow_count))
    results = {}
    phases = (("upload", 6), ("download", 12)) if mode == "sequential" else ((mode, 6),)
    for phase, idle in phases:
        directions = ("upload", "download") if phase == "bidirectional" else (phase,)
        send_seconds = duration or max(workload.SECONDS[direction] for direction in directions)
        time.sleep(idle)
        ready = output / f"{phase}.ready"
        reports = {}
        endpoints = {}
        for role in ("server", "client"):
            prefix = output / f"{phase}-{role}"
            with prefix.with_suffix(".ndjson").open("w") as out, prefix.with_suffix(".log").open("w") as err:
                endpoints[role] = spawn_owned(children, ["ip", "netns", "exec", lab.names[role],
                    sys.executable, workload.__file__, role, BUSINESS_IP, "19003", phase, str(ready),
                    str(flow_count), str(send_seconds)],
                    stdout=out, stderr=err)
            if role == "server":
                deadline = time.monotonic() + workload.CONNECT_TIMEOUT
                while not ready.exists():
                    if endpoints[role].poll() is not None or time.monotonic() >= deadline:
                        raise HarnessError(f"{phase} origin not ready; inspect {prefix}.log")
                    time.sleep(.02)
                ready.unlink()

        # One deadline for all endpoint workers.
        phase_started = time.monotonic()
        deadline = phase_started + send_seconds + workload.IO_TIMEOUT
        sample_directional_wire(lab, output, directions, phase_started, send_seconds, impairment,
                                wire_interval, qdisc_metrics, socket_metrics)
        for role, process in endpoints.items():
            if process.wait(timeout=max(0, deadline - time.monotonic())):
                raise HarnessError(f"{phase} {role} failed; inspect {output}/{phase}-*.ndjson and *.log")
            children.remove(process)
            rows = [json.loads(line) for line in (output / f"{phase}-{role}.ndjson").read_text().splitlines()]
            if (len(rows) != flow_count or any(type(r.get("flow")) is not int or "error" in r for r in rows)
                    or {r["flow"] for r in rows} != expected):
                raise HarnessError(f"{phase} {role}: expected {flow_count} distinct successful flow IDs")
            reports[role] = {r["flow"]: r for r in rows}

        for direction in directions:
            sender, receiver = ("client", "server") if direction == "upload" else ("server", "client")
            flows = []
            for flow in sorted(expected):
                src, dst = reports[sender][flow], reports[receiver][flow]
                if phase == "bidirectional":
                    src, dst = src.get(direction, {}), dst.get(direction, {})
                size, digest = src.get("sent_bytes"), src.get("sent_sha256")
                if (type(size) is not int or size <= 0 or type(dst.get("received_bytes")) is not int or
                        size != dst["received_bytes"] or not isinstance(digest, str) or
                        not re.fullmatch(r"[0-9a-f]{64}", digest) or digest != dst.get("received_sha256") or
                        src.get("sender_receipt") is not True or dst.get("sender_confirmed") is not True or
                        dst.get("exact_eof") is not True):
                    raise HarnessError(f"{direction} flow {flow}: count, SHA-256, EOF or receipt mismatch")
                flows.append({**src, **dst, "flow": flow})
            verified_bytes = sum(flow["sent_bytes"] for flow in flows)
            observed_ms = (max(flow["send_end_monotonic_ms"] for flow in flows) -
                           min(flow["send_start_monotonic_ms"] for flow in flows))
            results[direction] = {"send_seconds": send_seconds, "observed_send_ms": observed_ms,
                                  "verified_payload_bytes": verified_bytes,
                                  "verified_payload_mbps": verified_bytes * 8 / observed_ms / 1000,
                                  "flows": flows}
    time.sleep(6)
    return results


INTENT_FLOW = str(Path(__file__).with_name("intent_flow.py").resolve())
INTENT_IO_TIMEOUT = 20


def sample_intent_wire(lab: Lab, output: Path, label: str,
                       phase_started: float, duration: int) -> list[dict]:
    before = lab.directional_path_bytes("download")
    started = time.monotonic()
    rows = []
    for offset in range(1, duration + 1):
        time.sleep(max(0, phase_started + offset - time.monotonic()))
        lab.apply_rate_jitter()
        after = lab.directional_path_bytes("download")
        finished = time.monotonic()
        deltas = {path: (count - before[path] if count >= before[path] else None)
                  for path, count in after.items()}
        row = {"traffic_group": label, "start_seconds": started - phase_started,
               "end_seconds": finished - phase_started,
               "start_monotonic_ms": int(started * 1000),
               "end_monotonic_ms": int(finished * 1000),
               "tbf_bytes": deltas,
               "mbps": {path: count * 8 / (finished - started) / 1e6
                        if count is not None else None
                        for path, count in deltas.items()}}
        rows.append(row)
        before, started = after, finished
    wire = output / f"{label}-wire.ndjson"
    wire.write_text("".join(json.dumps(row) + "\n" for row in rows))
    return rows


def intent_path_summary(rows: list[dict]) -> dict:
    if not rows:
        raise HarnessError("intent workload produced no path samples")
    path_ids = sorted(rows[0]["tbf_bytes"])
    initial_rows = rows[:min(3, len(rows))]
    tail_rows = rows[-min(3, len(rows)):]
    def totals(window: list[dict]) -> dict[str, int]:
        return {path: sum((row["tbf_bytes"].get(path) or 0) for row in window)
                for path in path_ids}
    initial = totals(initial_rows)
    tail = totals(tail_rows)
    return {"initial_bytes": initial, "tail_bytes": tail,
            "initial_share": _shares(initial), "tail_share": _shares(tail),
            "samples": len(rows)}


def _shares(values: dict[str, int]) -> dict[str, float]:
    total = sum(values.values())
    return {path: (value / total if total else 0.0) for path, value in values.items()}


def count_intent_metrics(paths: list[Path]) -> dict[str, int]:
    counts = {"latency": 0, "bandwidth": 0, "unknown": 0, "flip": 0,
              "fallback": 0, "credit_fallback": 0}
    for path in paths:
        if not path.is_file():
            continue
        text = path.read_text(errors="replace")
        for name in counts:
            deltas = re.findall(rf"\[mux\.intent\.{name}\][^\n]*\bdelta=([0-9]+)\b", text)
            counts[name] += sum(map(int, deltas))
    return counts


def run_intent_phase(lab: Lab, children: list[subprocess.Popen], output: Path,
                     label: str, mode: str, flows: int, duration: int, scenario: str = "") -> dict:
    ready = output / f"{label}.ready"
    ready.unlink(missing_ok=True)
    endpoint_processes = {}
    endpoint_logs = {}
    workload_port = 19004 if scenario or mode == "web" else 19003
    for role in ("server", "client"):
        log_path = output / f"{label}-{role}.log"
        endpoint_logs[role] = log_path
        endpoint_processes[role] = spawn_owned(
            children,
            ["ip", "netns", "exec", lab.names[role], sys.executable, "-u", INTENT_FLOW,
             role, BUSINESS_IP, str(workload_port), mode, str(ready), str(flows), str(duration),
             *([scenario] if scenario else [])],
            stdout=log_path.open("w"), stderr=subprocess.STDOUT)
        if role == "server":
            deadline = time.monotonic() + 5
            while not ready.exists():
                if endpoint_processes[role].poll() is not None or time.monotonic() >= deadline:
                    raise HarnessError(f"{label} origin did not become ready; inspect {log_path}")
                time.sleep(.02)
    phase_started = time.monotonic()
    rows = sample_intent_wire(lab, output, label, phase_started, duration)
    for role, process in endpoint_processes.items():
        if process.wait(timeout=duration + INTENT_IO_TIMEOUT):
            raise HarnessError(f"{label} {role} failed; inspect {endpoint_logs[role]}")
        children.remove(process)
    reports = {}
    for role, log_path in endpoint_logs.items():
        lines = [line for line in log_path.read_text(errors="replace").splitlines() if line.strip()]
        try:
            reports[role] = json.loads(lines[-1])
        except (IndexError, json.JSONDecodeError) as exc:
            raise HarnessError(f"{label} {role} did not produce a valid workload report") from exc
    if mode == "bulk":
        sent = {row.get("flow"): row for row in reports["server"]}
        received = {row.get("flow"): row for row in reports["client"]}
        if set(sent) != set(received) or not sent:
            raise HarnessError(f"{label} did not complete every download flow")
        for flow, source in sent.items():
            target = received[flow]
            if (source.get("sent_bytes") != target.get("received_bytes") or
                    source.get("sent_sha256") != target.get("received_sha256")):
                raise HarnessError(f"{label} flow {flow} failed exact payload verification")
    elif (reports["server"][0].get("requests", 0) < 20 or
          reports["server"][0].get("requests") != reports["client"][0].get("requests")):
        raise HarnessError(f"{label} did not complete the web request burst")
    return {"mode": mode, "flows": flows, "duration_seconds": duration,
            "reports": reports, "paths": intent_path_summary(rows),
            "windowed_path_bytes": rows, **({"port": workload_port} if scenario else {})}


def intent_acceptance(lab: Lab, children: list[subprocess.Popen], output: Path) -> dict:
    if not args_profile_is_intent(lab.profile_name):
        raise HarnessError("intent-acceptance requires the intent-50-80-155 profile")
    phases = {
        "download4": run_intent_phase(lab, children, output, "intent-download4", "bulk", 4, 18),
        "web": run_intent_phase(lab, children, output, "intent-web", "web", 1, 6),
        "stream": run_intent_phase(lab, children, output, "intent-stream", "bulk", 1, 16),
    }
    download_paths = phases["download4"]["paths"]
    web_paths = phases["web"]["paths"]
    stream_paths = phases["stream"]["paths"]
    if download_paths["tail_bytes"].get("path3", 0) <= download_paths["initial_bytes"].get("path3", 0):
        raise HarnessError("four-flow download did not migrate toward the high-bandwidth path")
    if web_paths["tail_share"].get("path1", 0.0) < web_paths["tail_share"].get("path3", 0.0):
        raise HarnessError("web workload moved more traffic to the high-bandwidth path than the low-latency path")
    if stream_paths["tail_share"].get("path3", 0.0) <= stream_paths["tail_share"].get("path1", 0.0):
        raise HarnessError("streaming workload did not prefer the high-bandwidth path")
    metrics = count_intent_metrics([output / "six-flow-0-client.log", output / "six-flow-0-server.log"])
    if metrics["latency"] < 1 or metrics["bandwidth"] < 2:
        raise HarnessError(f"intent classifier metrics did not cover web and bulk flows: {metrics}")
    max_stall_ms = max((row.get("max_stall_ms", 0) for phase in phases.values()
                        for row in phase["reports"]["client"]), default=0)
    web_report = phases["web"]["reports"].get("client", [{}])[0]
    telemetry = {**metrics, "intent_flip_count": metrics["flip"], "credit_fallback_count": metrics["credit_fallback"]}
    return {"profile": lab.profile_name, "paths": lab.topology()["paths"],
            "classification_metrics": metrics, "telemetry": telemetry,
            "phases": phases,
            "windowed_path_bytes": [row for phase in phases.values()
                                    for row in phase.get("windowed_path_bytes", [])],
            "web_rtt_ms": {"p95": web_report.get("rtt_p95_ms", 0),
                           "p99": web_report.get("rtt_p99_ms", 0),
                           "max": web_report.get("rtt_max_ms", 0)},
            "max_stall_ms": max_stall_ms, "passed": True}


def args_profile_is_intent(profile: str) -> bool:
    return profile == "intent-50-80-155"


def intent_migration_acceptance(lab: Lab, children: list[subprocess.Popen], output: Path) -> dict:
    phases = {}
    for scenario, mode, flows, duration in (("download", "bulk", 4, 18),
                                            ("web", "web", 1, 8),
                                            ("video", "bulk", 1, 16)):
        phases[scenario] = run_intent_phase(lab, children, output, f"migration-{scenario}",
                                            mode, flows, duration, scenario)

    connections = {}
    for scenario, phase in phases.items():
        for row in phase["reports"]["client"]:
            key = (phase["port"], row["peer_port"])
            connections.setdefault(key, []).append({"scenario": scenario, **row})

    flows = {}
    endpoint_intents = {"client": {}, "server": {}}
    server_bytes = {}
    for side in endpoint_intents:
        path = output / f"{side}-flow-trace.ndjson"
        if not path.is_file():
            raise HarnessError(f"candidate did not record VMUX flow evidence at {path}")
        with path.open() as source:
            for line in source:
                event = json.loads(line)
                cid = event["cid"]
                if event["event"] == "intent":
                    if cid in endpoint_intents[side]:
                        raise HarnessError(f"duplicate {side} intent for VMUX flow {cid}")
                    endpoint_intents[side][cid] = event["intent"]
                elif side == "client" and event["event"] == "open":
                    key = (event["destination_port"], event["peer_port"])
                    opened = event["monotonic_us"] * 1000
                    matches = [row for row in connections.get(key, ())
                               if row["connection_start_monotonic_ns"] <= opened + 999
                               and opened <= row["connection_end_monotonic_ns"]]
                    if len(matches) != 1 or cid in flows:
                        raise HarnessError(f"could not uniquely attribute VMUX flow {cid} for {key}")
                    flows[cid] = {"connection": matches[0], "prelude": Counter(), "body": Counter(),
                                  "first_delivery_path": None, "first_body_delivery_ns": None,
                                  "first_body_path3_ns": None}
                elif event["event"] == "delivery" and cid in flows:
                    carrier = event["path_id"]
                    if side == "server":
                        server_bytes.setdefault(cid, Counter())[carrier] += event["bytes"]
                        continue
                    flow = flows[cid]
                    tick = event["monotonic_us"] * 1000
                    is_body = tick >= flow["connection"]["body_start_monotonic_ns"]
                    flow["body" if is_body else "prelude"][carrier] += event["bytes"]
                    if flow["first_delivery_path"] is None:
                        flow["first_delivery_path"] = carrier
                    if is_body and flow["first_body_delivery_ns"] is None:
                        flow["first_body_delivery_ns"] = tick
                    if is_body and carrier == "path3" and flow["first_body_path3_ns"] is None:
                        flow["first_body_path3_ns"] = tick

    by_scenario = {name: [] for name in phases}
    for cid, flow in flows.items():
        row = flow.pop("connection")
        scenario = row["scenario"]
        intents = {side: values.get(cid) for side, values in endpoint_intents.items()}
        expected_intent = "latency" if scenario == "web" else "bandwidth"
        upstream = server_bytes.get(cid, {})
        body_share = _shares(flow["body"])
        checks = {
            "detected_on_both_endpoints": all(intent == expected_intent for intent in intents.values()),
            "first_payload_on_path1": flow["first_delivery_path"] == "path1" and flow["prelude"]["path1"] > 0,
            "all_deliveries_attributed": (set(flow["prelude"]) | set(flow["body"]) | set(upstream)) <=
                                         {"path1", "path2", "path3"},
        }
        first_delivery = flow["first_body_delivery_ns"]
        first_body_ms = ((first_delivery - row["body_start_monotonic_ns"]) / 1_000_000
                         if first_delivery is not None else None)
        if scenario == "web":
            checks["web_payload_stays_off_path3"] = (
                sum(flow["body"].values()) > 0 and sum(upstream.values()) > 0 and
                flow["prelude"]["path3"] + flow["body"]["path3"] + upstream.get("path3", 0) == 0)
            checks["all_web_requests_completed"] = row["requests"] == 40
            checks["web_p95_ms_at_most_85"] = row["rtt_p95_ms"] <= 85
        else:
            checks["migrated_on_same_connection"] = (
                flow["first_body_path3_ns"] is not None and body_share.get("path3", 0) >= .80)
            checks["bulk_integrity"] = row["exact_eof"] and row["received_bytes"] > 0
            checks["first_body_ms_at_most_1000"] = first_body_ms is not None and first_body_ms <= 1000
            checks["max_stall_ms_at_most_1000"] = row["max_stall_ms"] <= 1000
        by_scenario[scenario].append({"cid": cid, "flow": row["flow"], "intent": intents, **flow,
                                      "body_path_shares": body_share, "first_body_ms": first_body_ms,
                                      "upstream_path_bytes": upstream,
                                      "checks": checks, "passed": all(checks.values())})
    metrics = count_intent_metrics([output / "six-flow-0-client.log", output / "six-flow-0-server.log"])
    passed = metrics["flip"] == 0 and all(
        sorted(flow["flow"] for flow in results) == list(range(phases[name]["flows"])) and
        all(flow["passed"] for flow in results)
        for name, results in by_scenario.items())
    return {"port": next(iter(phases.values()))["port"], "phases": phases, "flows": by_scenario,
            "classification_metrics": metrics, "passed": passed}


@dataclass(frozen=True)
class StrictWorkload:
    label: str
    mode: str
    start_s: float
    flows: int
    byte_count: int = 0
    duration_s: float = 0
    requests: int = 0
    interval_ms: int = 0
    port: int = 0
    group: str = ""


MIB = 1024 * 1024
STRICT_PROFILE_SPECS = {
    "intent-strict-narrow-p1": {
        "duration": 45, "fixed_pool": True, "carriers": 2,
        "workloads": [StrictWorkload("download", "bulk", 0, 8, 25 * MIB, 43),
                      StrictWorkload("stream", "bulk", 0, 1, 32 * MIB, 43),
                      StrictWorkload("web", "web", 10, 4, requests=40, interval_ms=200)],
        "gates": {"download_path1_max": .01, "download_path3_min": .45,
                   "web_path3_max": .01, "web_p95_max": 55, "web_p99_max": 65,
                   "stall_max": 1000},
    },
    "intent-strict-role-flip": {
        "duration": 55, "fixed_pool": True, "carriers": 2,
        "workloads": [StrictWorkload("download", "bulk", 0, 4, 62 * MIB, 54),
                      StrictWorkload("stream", "bulk", 0, 1, 48 * MIB, 54),
                      StrictWorkload("web-initial", "web", 1, 1, requests=8, interval_ms=500, group="web"),
                      StrictWorkload("web-degrade-edge", "web", 14, 1, requests=6, interval_ms=500, group="web"),
                      StrictWorkload("web-fault-early", "web", 18, 1, requests=8, interval_ms=500, group="web"),
                      StrictWorkload("web-fault-late", "web", 29, 1, requests=8, interval_ms=500, group="web"),
                      StrictWorkload("web-restore-edge", "web", 34, 1, requests=6, interval_ms=500, group="web"),
                      StrictWorkload("web-recovery-early", "web", 40, 1, requests=8, interval_ms=500, group="web"),
                      StrictWorkload("web-recovery-late", "web", 51, 1, requests=6, interval_ms=500, group="web")],
        "faults": [{"at_s": 15, "path": "path1", "action": "degrade", "value": 140},
                   {"at_s": 15, "path": "path3", "action": "throttle", "value": 30},
                   {"at_s": 35, "path": "path1", "action": "restore", "restore_action": "degrade"},
                   {"at_s": 35, "path": "path3", "action": "restore", "restore_action": "throttle"}],
        "gates": {"fault_web_path1_max": .10, "fault_web_path2_min": .80,
                   "fault_download_path3_max": .15, "fault_download_path2_min": .70,
                   "recovery_web_path1_min": .50, "recovery_download_path3_min": .40,
                   "stall_max": 2000},
    },
    "intent-strict-credit-blackhole": {
        "duration": 45, "fixed_pool": True, "carriers": 2,
        "workloads": [StrictWorkload("download", "bulk", 0, 2, 75 * MIB, 43),
                      StrictWorkload("stream", "bulk", 0, 1, 24 * MIB, 43),
                      StrictWorkload("web", "web", 7, 1, requests=45, interval_ms=600)],
        "faults": [{"at_s": 10, "path": "path1", "action": "blackhole"},
                   {"at_s": 10, "path": "path2", "action": "blackhole"},
                   {"at_s": 20, "path": "path1", "action": "restore"},
                   {"at_s": 20, "path": "path2", "action": "restore"}],
        "gates": {"blackhole_web_path3_min": .90, "recovery_web_path3_max": .05,
                   "recovery_web_p95_max": 85, "stall_max": 2500,
                   "credit_fallback_min": 1},
    },
    "intent-strict-grayzone": {
        "duration": 40, "fixed_pool": True, "carriers": 2,
        "workloads": [StrictWorkload("port443_web", "web", 0, 4, requests=20, interval_ms=250, port=443),
                      StrictWorkload("phase_shift", "phase", 0, 2, 100 * MIB, interval_ms=500),
                      StrictWorkload("grayzone_unknown", "gray", 0, 3, requests=10, interval_ms=300),
                      StrictWorkload("port443_bulk", "bulk", 5, 2, 50 * MIB, 24, port=443),
                      StrictWorkload("nonstd_port_bulk", "bulk", 5, 2, 50 * MIB, 24,
                                     port=24443, group="nonstd_bulk")],
        "gates": {"unknown_min": 3, "latency_min": 1, "bandwidth_min": 4,
                   "nonstd_bulk_path3_min": .40, "port443_bulk_path1_max": .05,
                   "flip_max": 0},
    },
    "intent-strict-burst-kill": {
        "duration": 50, "fixed_pool": True, "carriers": 2,
        "workloads": [StrictWorkload("download", "bulk", 0, 4, 50 * MIB, 47),
                      StrictWorkload("stream", "bulk", 0, 1, 32 * MIB, 47),
                      StrictWorkload("web", "web", 2, 1, requests=30, interval_ms=1500)],
        "faults": [{"at_s": 18, "path": "path3", "action": "drop_carrier", "duration_s": 5}],
        "gates": {"download_path1_max": .03, "web_path3_max": .02,
                   "stall_max": 2000, "web_p99_max": 120},
    },
    "intent-strict-dynamic-tide": {
        "duration": 90, "fixed_pool": False, "carriers": 1,
        "workloads": [StrictWorkload("idle_web_only", "web", 0, 1, requests=15, interval_ms=1000, group="web"),
                      StrictWorkload("burst-download-1", "bulk", 15, 6, duration_s=25, group="download"),
                      StrictWorkload("burst-stream-1", "bulk", 15, 1, 18 * MIB, 23, group="stream"),
                      StrictWorkload("burst-web-1", "web", 15, 1, requests=50, interval_ms=500, group="web"),
                      StrictWorkload("cooldown_web_only", "web", 40, 1, requests=20, interval_ms=1000, group="web"),
                      StrictWorkload("burst-download-2", "bulk", 60, 6, duration_s=25, group="download"),
                      StrictWorkload("burst-stream-2", "bulk", 60, 1, 18 * MIB, 23, group="stream"),
                      StrictWorkload("burst-web-2", "web", 60, 1, requests=50, interval_ms=500, group="web")],
        "gates": {"active_path1_min": 1, "scale_up": True, "scale_down": True,
                   "burst_path3_min": .40, "cooldown_web_p99_max": 85,
                   "stall_max": 1500},
    },
}


def _strict_fault_path(lab: Lab, name: str) -> dict:
    for path in lab.paths:
        if path["id"] == name:
            return path
    raise HarnessError(f"strict fault references unknown path {name}")


def _strict_carrier_socket(lab: Lab, path: dict, carrier_index: int) -> dict:
    sockets = []
    for protocol, flag in (("tcp", "-Htn"), ("udp", "-Huan")):
        output = run(["ip", "netns", "exec", lab.names["client"], "ss", flag], capture=True)
        for row in parse_established_sockets(output):
            if (row.get("local_address") != path["client_ip"] or
                    row.get("peer_address") != path["server_ip"] or
                    row.get("peer_port") != str(path["carrier_listener_port"])):
                continue
            if row.get("local_port", "").isdigit():
                sockets.append({"protocol": protocol, "source_ip": row["local_address"],
                                "source_port": int(row["local_port"]), "peer_ip": row["peer_address"],
                                "peer_port": int(row["peer_port"])})
    sockets.sort(key=lambda item: (item["source_port"], item["protocol"]))
    if carrier_index >= len(sockets):
        raise HarnessError(f"could not resolve {path['id']}:carrier{carrier_index}")
    return sockets[carrier_index]


def _strict_drop_carrier(lab: Lab, path: dict, restore: bool, event: dict) -> dict:
    carrier = event.get("carrier_socket")
    if carrier is None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                carrier = _strict_carrier_socket(lab, path, event.get("carrier_index", 0))
                break
            except HarnessError:
                time.sleep(.1)
    if carrier is None:
        raise HarnessError(f"drop_carrier could not resolve {path['id']} source port")
    event["carrier_socket"] = carrier
    namespace = lab.names["client"]
    rule = ["-o", path["client_if"], "-p", carrier["protocol"], "-s", carrier["source_ip"],
            "--sport", str(carrier["source_port"]), "-d", carrier["peer_ip"],
            "--dport", str(carrier["peer_port"]), "-j", "DROP"]
    tracked_rule = (namespace, rule)
    previous = block_interrupt_signals()
    try:
        run(["ip", "netns", "exec", namespace, "iptables", "-D" if restore else "-I", "OUTPUT", *rule])
        if restore:
            lab.firewall_rules.remove(tracked_rule)
        else:
            lab.firewall_rules.append(tracked_rule)
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)
    return {"path_id": path["id"], "source_port": carrier["source_port"], "protocol": carrier["protocol"],
            "five_tuple": carrier}


def _strict_apply_fault(lab: Lab, event: dict, restore: bool = False) -> dict:
    path = _strict_fault_path(lab, event["path"])
    action = event["action"]
    if action == "drop_carrier":
        return _strict_drop_carrier(lab, path, restore, event)
    lab.change_path_action(path, event.get("restore_action", action),
                           restore=restore or action == "restore", value=event.get("value"))
    return {"path_id": path["id"], "action": action,
            "value": event.get("value"), "restored": restore or action == "restore"}


def _strict_runtime_sample(output: Path, sampled_ms: int, role: str = "client") -> dict:
    records = read_stats(output / f"{role}-stats.ndjson")
    if not records:
        return {"sample_ms": sampled_ms, "paths": [], "intent": {}}
    row = min(records, key=lambda item: abs(item["monotonic_ms"] - sampled_ms))
    multipath = row.get("runtime", {}).get("multipath", {})
    return {"sample_ms": row.get("monotonic_ms", sampled_ms),
            "paths": multipath.get("paths", []), "intent": multipath.get("intent", {})}


@contextlib.contextmanager
def _strict_fault_schedule(lab: Lab, output: Path, started: float, duration: int, events: list[dict]):
    stop = threading.Event()
    schedule = [(event["at_s"], False, event) for event in events]
    schedule.extend((event["at_s"] + event["duration_s"], True, event)
                    for event in events if event.get("duration_s"))
    if lab.rate_jitter_percent:
        event_times = {offset for offset, _, _ in schedule}
        schedule.extend((second, False, None) for second in range(duration) if second not in event_times)
    schedule.sort(key=lambda item: item[0])

    def apply_events():
        if not schedule:
            return
        throttled_paths = set()
        with (output / "fault-events.ndjson").open("a", buffering=1) as event_file:
            for offset, restore, event in schedule:
                if stop.wait(max(0, started + offset - time.monotonic())):
                    return
                if event is None:
                    lab.apply_rate_jitter(throttled_paths)
                    continue
                applied_at = time.monotonic() - started
                details = _strict_apply_fault(lab, event, restore=restore)
                if event.get("restore_action", event["action"]) == "throttle":
                    if details["restored"]:
                        throttled_paths.discard(event["path"])
                    else:
                        throttled_paths.add(event["path"])
                event_file.write(json.dumps({**event, **details, "scheduled_at_s": offset,
                                             "applied_at_s": applied_at,
                                             "event": "restored" if details.get("restored") or restore else "applied"}) + "\n")

    with ThreadPoolExecutor(max_workers=1) as executor:
        worker = executor.submit(apply_events)
        try:
            yield worker
            worker.result()
        finally:
            stop.set()


def _strict_start_job(lab: Lab, children: list[subprocess.Popen], output: Path,
                      workload: StrictWorkload, port: int, starts_at: float) -> dict:
    prefix = output / f"strict-{workload.label}"
    ready = prefix.with_suffix(".ready")
    script = str(Path(__file__).with_name("strict_flow.py"))
    endpoints = {}
    for role in ("server", "client"):
        report = Path(f"{prefix}-{role}.json")
        log = Path(f"{prefix}-{role}.log")
        args = [script, role, BUSINESS_IP, str(port), workload.mode, str(ready),
                str(workload.flows), str(workload.byte_count), str(workload.requests),
                str(workload.interval_ms), "10", str(starts_at), str(workload.duration_s)]
        with report.open("w") as stdout, log.open("w") as stderr:
            process = spawn_owned(children, ["ip", "netns", "exec", lab.names[role],
                                  sys.executable, "-u", *args], stdout=stdout, stderr=stderr)
        endpoints[role] = (process, report, log)
    return {"workload": workload, "port": port, "endpoints": endpoints}


def _strict_read_report(path: Path, process: subprocess.Popen, label: str) -> list[dict]:
    if process.returncode:
        raise HarnessError(f"strict workload {label} failed with status {process.returncode}")
    lines = [line for line in path.read_text(errors="replace").splitlines() if line.strip()]
    try:
        value = json.loads(lines[-1])
    except (IndexError, json.JSONDecodeError) as exc:
        raise HarnessError(f"strict workload {label} did not produce JSON") from exc
    if not isinstance(value, list):
        raise HarnessError(f"strict workload {label} report is not an array")
    return value


def _strict_run_workloads(lab: Lab, children: list[subprocess.Popen], output: Path,
                          spec: dict, started: float, fault_worker) -> tuple[list[dict], list[dict]]:
    workloads = sorted(enumerate(spec["workloads"]), key=lambda item: item[1].start_s)
    pending = list(workloads)
    jobs = []
    carrier_samples = []
    duration = spec["duration"]
    with (output / "carrier-samples.ndjson").open("w", buffering=1) as carrier_file, \
            (output / "strict-socket-metrics.ndjson").open("w", buffering=1) as socket_file:
        while True:
            now = time.monotonic()
            if fault_worker.done():
                fault_worker.result()
            while pending and now >= started + pending[0][1].start_s - .5:
                index, workload = pending.pop(0)
                jobs.append(_strict_start_job(lab, children, output, workload,
                                              workload.port or 24000 + index, started + workload.start_s))
            if len(carrier_samples) <= duration and now >= started + len(carrier_samples):
                second = len(carrier_samples)
                sample = _strict_runtime_sample(output, int(now * 1000))
                sample["second"] = second
                sample["active_carriers"] = {path["id"]: 0 for path in lab.paths}
                for path in sample["paths"]:
                    if path["id"] in sample["active_carriers"]:
                        sample["active_carriers"][path["id"]] = path.get("carriers", 0)
                carrier_samples.append(sample)
                carrier_file.write(json.dumps(sample) + "\n")
                for row in socket_metrics_sample(lab, ("download",), int(now * 1000)):
                    socket_file.write(json.dumps(row) + "\n")
            running = False
            for job in jobs:
                for role, (process, _, log) in job["endpoints"].items():
                    status = process.poll()
                    if status is None:
                        running = True
                    elif status:
                        raise HarnessError(f"strict workload {job['workload'].label}-{role} exited {status}; inspect {log}")
            if not pending and not running and len(carrier_samples) > duration:
                break
            if now >= started + duration + 45:
                raise HarnessError("strict workload exceeded the profile and receipt deadline")
            time.sleep(.04)
    phases = []
    for job in jobs:
        workload = job["workload"]
        expected = workload.requests if workload.mode == "web" else workload.flows
        phase = {"label": workload.label, "group": workload.group or workload.label,
                 "mode": workload.mode, "flows": expected, "port": job["port"],
                 "byte_count": workload.byte_count}
        for role, (process, report, _) in job["endpoints"].items():
            children.remove(process)
            rows = _strict_read_report(report, process, f"{workload.label}-{role}")
            if ((workload.mode != "web" or role == "client") and
                    (len(rows) != expected or {row.get("flow") for row in rows} != set(range(expected)))):
                raise HarnessError(f"strict workload {workload.label}-{role} lacks {expected} distinct receipts")
            phase[role] = rows
        phases.append(phase)
    return phases, carrier_samples


def _strict_flow_windows(output: Path, phases: list[dict], paths: list[str],
                         started: float, duration: int) -> tuple[list[dict], dict]:
    """Bucket ordered VMUX payload on the carrier that delivered it."""
    connections = {}
    expected_flows = 0
    for phase in phases:
        for row in phase["client"]:
            if "peer_port" not in row:
                continue
            socket_key = (phase["port"], row["peer_port"])
            connections.setdefault(socket_key, []).append((row["connection_start_monotonic_ns"],
                                                            row["connection_end_monotonic_ns"], phase["group"]))
            expected_flows += 1
    groups = sorted({group for lifetimes in connections.values() for _, _, group in lifetimes})
    windows = [{group: dict.fromkeys(paths, 0) for group in groups} for _ in range(duration)]
    flow_groups = {}
    flow_intents = {}
    observed = set()
    unmatched_bytes = 0
    overflow_bytes = 0
    trace = output / "client-flow-trace.ndjson"
    if not trace.is_file():
        raise HarnessError(f"candidate did not emit per-flow VMUX delivery evidence at {trace}")
    with trace.open() as source:
        for line in source:
            record = json.loads(line)
            cid = record["cid"]
            if record["event"] == "intent":
                if cid in flow_intents:
                    raise HarnessError(f"VMUX flow {cid} was accounted more than once")
                flow_intents[cid] = record["intent"]
                continue
            if record["event"] == "open":
                connection = (record["destination_port"], record["peer_port"])
                if connection in connections:
                    opened_ns = record["monotonic_us"] * 1000
                    matches = [(began, group) for began, ended, group in connections[connection]
                               if began <= opened_ns + 999 and opened_ns <= ended]
                    if len(matches) != 1 or cid in flow_groups:
                        raise HarnessError(f"ambiguous VMUX flow attribution for socket {connection}")
                    began, group = matches[0]
                    instance = (*connection, began)
                    if instance in observed:
                        raise HarnessError(f"ambiguous VMUX flow attribution for socket {connection}")
                    flow_groups[cid] = group
                    observed.add(instance)
                continue
            path, size = record["path_id"], record["bytes"]
            second = math.floor(record["monotonic_us"] / 1_000_000 - started)
            if cid not in flow_groups or path not in paths:
                unmatched_bytes += size
            elif 0 <= second < duration:
                windows[second][flow_groups[cid]][path] += size
            else:
                overflow_bytes += size
    rows = [{"start_seconds": second, "end_seconds": second + 1,
             "start_monotonic_ms": int((started + second) * 1000),
             "end_monotonic_ms": int((started + second + 1) * 1000),
             "windowed_path_bytes": groups} for second, groups in enumerate(windows)]
    with (output / "windowed_path_bytes.ndjson").open("w") as target:
        for row in rows:
            target.write(json.dumps(row) + "\n")
    classifications = {group: Counter() for group in groups}
    for cid, group in flow_groups.items():
        if cid in flow_intents:
            classifications[group][flow_intents[cid]] += 1
    return rows, {"expected_flows": expected_flows, "matched_flows": len(observed),
                  "unmatched_bytes": unmatched_bytes, "outside_profile_bytes": overflow_bytes,
                  "classifications": classifications}


def _strict_shares(rows: list[dict], group: str, start: int, end: int) -> dict[str, float]:
    totals = Counter()
    for row in rows[start:end]:
        totals.update(row["windowed_path_bytes"].get(group, {}))
    total = sum(totals.values())
    return {path: value / total for path, value in totals.items()} if total else {}


def _strict_rtt_summary(samples: list[float]) -> dict:
    return {"count": len(samples), "p95": _percentile(samples, .95) if samples else None,
            "p99": _percentile(samples, .99) if samples else None, "max": max(samples, default=None)}


def _strict_all_reports(phases: list[dict]) -> dict:
    integrity = []
    web_samples = []
    max_stall = 0
    for phase in phases:
        server = {row.get("flow"): row for row in phase["server"]}
        client = {row.get("flow"): row for row in phase["client"]}
        for flow, source in server.items():
            target = client.get(flow, {})
            if phase["mode"] in ("bulk", "phase"):
                sent = source.get("sent_bytes")
                received = target.get("received_bytes")
                digest = source.get("sent_sha256")
                integrity.append({"label": phase["label"], "flow": flow,
                                  "sha256_ok": isinstance(digest, str) and len(digest) == 64 and
                                               digest == target.get("received_sha256"),
                                  "bytes_match": type(sent) is int and type(received) is int and
                                                 sent == received and sent > 0 and
                                                 (phase["byte_count"] == 0 or sent == phase["byte_count"]),
                                  "eof_ok": source.get("eof_ok") is True and target.get("eof_ok") is True,
                                  "sent_bytes": sent, "received_bytes": received})
                max_stall = max(max_stall, source.get("max_stall_ms", 0), target.get("max_stall_ms", 0))
        for row in phase["client"]:
            web_samples.extend(zip(row.get("web_start_monotonic_ms", []), row.get("web_rtt_ms", []), strict=True))
            max_stall = max(max_stall, row.get("max_stall_ms", 0))
    return {"integrity": integrity, "web_samples": web_samples,
            "web_rtt_ms": _strict_rtt_summary([value for _, value in web_samples]),
            "max_stall_ms": max_stall}


def strict_profile_acceptance(lab: Lab, children: list[subprocess.Popen], output: Path,
                              profile_name: str) -> dict:
    spec = STRICT_PROFILE_SPECS[profile_name]
    duration = spec["duration"]
    lab.enable_baseline_loss()
    timeline_start = time.monotonic() + .5
    with _strict_fault_schedule(lab, output, timeline_start, duration, copy.deepcopy(spec.get("faults", []))) as faults:
        phases, carrier_samples = _strict_run_workloads(lab, children, output, spec, timeline_start, faults)

    rows, attribution = _strict_flow_windows(
        output, phases, [path["id"] for path in lab.paths], timeline_start, duration)
    reports = _strict_all_reports(phases)
    classified = Counter()
    for group_counts in attribution["classifications"].values():
        classified.update(group_counts)
    kinds = ("latency", "bandwidth", "unknown")
    # The runtime publisher and the per-flow trace run on different clocks.
    # Allow the final connection counts to reach the next runtime snapshot.
    completed_ms = time.monotonic_ns() // 1_000_000
    stats_deadline = time.monotonic() + 3
    while True:
        sampled_ms = time.monotonic_ns() // 1_000_000
        snapshots = {role: _strict_runtime_sample(output, sampled_ms, role)
                     for role in ("client", "server")}
        endpoint_telemetry = {role: snapshot["intent"] for role, snapshot in snapshots.items()}
        if (all(snapshot["sample_ms"] >= completed_ms for snapshot in snapshots.values()) and
                all(endpoint_telemetry["client"].get(kind) == classified[kind] for kind in kinds)):
            break
        if time.monotonic() >= stats_deadline:
            break
        time.sleep(.1)
    telemetry = dict(endpoint_telemetry["client"])
    for field in ("intent_flip_count", "credit_fallback_count"):
        if all(field in values for values in endpoint_telemetry.values()):
            telemetry[field] = sum(values[field] for values in endpoint_telemetry.values())
        else:
            telemetry.pop(field, None)
    groups = {phase["group"]: _strict_shares(rows, phase["group"], 0, duration) for phase in phases}
    windows = {
        "intent-strict-role-flip": {"degraded": (18, 34), "recovery": (40, 55)},
        "intent-strict-credit-blackhole": {"blackhole": (12, 19), "recovery": (25, 45)},
        "intent-strict-dynamic-tide": {"first_burst": (15, 40), "cooldown": (40, 60), "second_burst": (60, 85)},
    }.get(profile_name, {})
    timeline_start_ms = int(timeline_start * 1000)
    window_shares = {name: {group: _strict_shares(rows, group, start, end) for group in groups}
                     for name, (start, end) in windows.items()}
    web_rtt_by_window = {name: _strict_rtt_summary([
        value for started, value in reports["web_samples"]
        if start * 1000 <= started - timeline_start_ms < end * 1000
    ]) for name, (start, end) in windows.items()}
    web_success = 0
    for phase in phases:
        if phase["mode"] != "web":
            continue
        origins = {row.get("flow"): row for row in phase["server"]}
        for row in phase["client"]:
            origin = origins.get(row["flow"], {})
            web_success += (
                row.get("requests") == origin.get("requests") == 1 and
                row.get("bytes") == origin.get("bytes") and
                row.get("web_success_rate") == origin.get("web_success_rate") == 1.0 and
                row.get("web_error_count") == origin.get("web_error_count") == 0 and
                row.get("eof_ok") is True)
    expected_requests = sum(workload.requests for workload in spec["workloads"] if workload.mode == "web")
    web_errors = expected_requests - web_success
    result = {"schema": 2, "profile": profile_name, "duration_seconds": duration,
              "timeline_start_monotonic_ms": timeline_start_ms, "windowed_path_bytes": rows,
              "group_path_shares": groups, "window_path_shares": window_shares,
              "web_rtt_ms": reports["web_rtt_ms"], "web_rtt_ms_by_window": web_rtt_by_window,
              "web_success_rate": web_success / expected_requests, "web_error_count": web_errors,
              "max_stall_ms": reports["max_stall_ms"], "integrity": reports["integrity"],
              "telemetry": telemetry, "endpoint_telemetry": endpoint_telemetry,
              "phases": phases, "carrier_samples": carrier_samples,
              "payload_attribution": attribution, "passed": False}
    gates = spec["gates"]
    checks = {}
    compared = {}

    def compare(name: str, actual, expected, condition: str):
        passed = actual is not None and {
            "min": lambda: actual >= expected,
            "max": lambda: actual <= expected,
            "equal": lambda: actual == expected,
        }[condition]()
        checks[name] = passed
        compared[name] = {"actual": actual, condition: expected}

    def share(label: str, path: str, window: str | None = None) -> float | None:
        shares = window_shares[window] if window is not None else groups
        return shares.get(label, {}).get(path)

    if profile_name == "intent-strict-narrow-p1":
        compare("download_path1_share", share("download", "path1"), gates["download_path1_max"], "max")
        compare("download_path3_share", share("download", "path3"), gates["download_path3_min"], "min")
        compare("web_path3_share", share("web", "path3"), gates["web_path3_max"], "max")
        compare("web_p95_rtt_ms", reports["web_rtt_ms"]["p95"], gates["web_p95_max"], "max")
        compare("web_p99_rtt_ms", reports["web_rtt_ms"]["p99"], gates["web_p99_max"], "max")
    elif profile_name == "intent-strict-role-flip":
        compare("web_path1_share", share("web", "path1", "degraded"), gates["fault_web_path1_max"], "max")
        compare("web_path2_share", share("web", "path2", "degraded"), gates["fault_web_path2_min"], "min")
        compare("download_path3_share", share("download", "path3", "degraded"), gates["fault_download_path3_max"], "max")
        compare("download_path2_share", share("download", "path2", "degraded"), gates["fault_download_path2_min"], "min")
        compare("recovery_web_path1_share", share("web", "path1", "recovery"), gates["recovery_web_path1_min"], "min")
        compare("recovery_download_path3_share", share("download", "path3", "recovery"), gates["recovery_download_path3_min"], "min")
    elif profile_name == "intent-strict-credit-blackhole":
        compare("credit_fallback_count", telemetry.get("credit_fallback_count"), gates["credit_fallback_min"], "min")
        compare("blackhole_web_path3_share", share("web", "path3", "blackhole"), gates["blackhole_web_path3_min"], "min")
        compare("blackhole_web_samples", web_rtt_by_window["blackhole"]["count"], 1, "min")
        compare("recovery_web_path3_share", share("web", "path3", "recovery"), gates["recovery_web_path3_max"], "max")
        compare("recovery_web_p95_rtt_ms", web_rtt_by_window["recovery"]["p95"], gates["recovery_web_p95_max"], "max")
    elif profile_name == "intent-strict-grayzone":
        for kind in ("unknown", "latency", "bandwidth"):
            compare(kind, telemetry.get(kind), gates[f"{kind}_min"], "min")
        compare("grayzone_unknown", attribution["classifications"].get("grayzone_unknown", {}).get("unknown", 0),
                gates["unknown_min"], "min")
        compare("nonstd_port_bulk_path3_share", share("nonstd_bulk", "path3"), gates["nonstd_bulk_path3_min"], "min")
        compare("port443_bulk_path1_share", share("port443_bulk", "path1"), gates["port443_bulk_path1_max"], "max")
    elif profile_name == "intent-strict-burst-kill":
        compare("download_path1_share", share("download", "path1"), gates["download_path1_max"], "max")
        compare("web_path3_share", share("web", "path3"), gates["web_path3_max"], "max")
        compare("web_p99_rtt_ms", reports["web_rtt_ms"]["p99"], gates["web_p99_max"], "max")
    else:
        counts = [row["active_carriers"] for row in carrier_samples]
        totals = [sum(row.values()) for row in counts]
        idle, first_burst = max(totals[:15]), max(totals[15:40])
        cooldown, second_burst = min(totals[40:60]), max(totals[60:85])
        result["path1_active_carriers_min_ever"] = min(row["path1"] for row in counts)
        result["scale_up_detected"] = first_burst > idle and second_burst > totals[59]
        result["scale_down_detected"] = cooldown < first_burst and any(
            totals[second] < totals[second - 1] for second in range(40, 60))
        result["carrier_pool_tide"] = {"idle_peak": idle, "first_burst_peak": first_burst,
                                        "cooldown_floor": cooldown, "second_burst_peak": second_burst}
        compare("path1_active_carriers_min_ever", result["path1_active_carriers_min_ever"], gates["active_path1_min"], "min")
        compare("scale_up_detected", result["scale_up_detected"], True, "equal")
        compare("scale_down_detected", result["scale_down_detected"], True, "equal")
        compare("per_path_carrier_min", min(min(row.values()) for row in counts), 1, "min")
        compare("per_path_carrier_max", max(max(row.values()) for row in counts), 4, "max")
        compare("burst_full_1_path3_share", share("download", "path3", "first_burst"), gates["burst_path3_min"], "min")
        compare("burst_full_2_path3_share", share("download", "path3", "second_burst"), gates["burst_path3_min"], "min")
        compare("cooldown_web_p99_rtt_ms", web_rtt_by_window["cooldown"]["p99"], gates["cooldown_web_p99_max"], "max")

    compare("matched_flows", attribution["matched_flows"], attribution["expected_flows"], "equal")
    compare("unmatched_bytes", attribution["unmatched_bytes"], 0, "equal")
    compare("classified_flows", sum(classified.values()), attribution["matched_flows"], "equal")
    for kind in kinds:
        compare(f"telemetry_{kind}_count", telemetry.get(kind), classified[kind], "equal")
    if "stall_max" in gates:
        compare("max_stall_ms", reports["max_stall_ms"], gates["stall_max"], "max")
    compare("intent_flip_count", telemetry.get("intent_flip_count"), 0, "equal")
    compare("web_success_rate", result["web_success_rate"], 1.0, "equal")
    compare("web_error_count", web_errors, 0, "equal")
    compare("integrity", bool(reports["integrity"]) and all(
        item["sha256_ok"] and item["bytes_match"] and item["eof_ok"] for item in reports["integrity"]), True, "equal")
    result["assertions"] = checks
    result["assertion_values"] = compared
    result["passed"] = all(checks.values())
    if not result["passed"]:
        (output / "intent-strict.json").write_text(json.dumps(result, indent=2) + "\n")
        failed = {name: compared[name] for name, passed in checks.items() if not passed}
        raise HarnessError(f"{profile_name} strict assertions failed: {json.dumps(failed, sort_keys=True)}")
    return result


def intent_strict_suite(args: argparse.Namespace) -> int:
    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise HarnessError(f"candidate PPP binary is not executable: {binary}")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, mode=0o700)
    candidate = output / "ppp-candidate"
    shutil.copy2(binary, candidate)
    summary = {"schema": 2, "suite": "intent-strict", "profiles": [], "passed": False,
               "candidate": {"source_binary": str(binary), "path": str(candidate),
                             "sha256": _sha256_file(candidate)}}
    summary_path = output / "intent-strict-summary.json"
    for name in STRICT_PROFILE_SPECS:
        profile_args = copy.copy(args)
        profile_args.suite = None
        profile_args.binary = str(candidate)
        profile_args.strict_profile = name
        profile_args.profile = name
        profile_args.output = str(output / name)
        profile_args.fixed_pool = STRICT_PROFILE_SPECS[name]["fixed_pool"]
        profile_args.carriers_per_path = STRICT_PROFILE_SPECS[name]["carriers"]
        profile_args.intent = True
        profile_args.intent_acceptance = False
        profile_args.pool_metrics = True
        profile_args.socket_metrics = True
        profile_args.duration = STRICT_PROFILE_SPECS[name]["duration"]
        profile_path = Path(profile_args.output) / "intent-strict.json"
        try:
            code = production_six_flow(profile_args)
            profile = json.loads(profile_path.read_text())
            if code:
                profile.update(passed=False, error=f"runner exited with status {code}")
        except Exception as exc:
            profile = {"profile": name}
            if profile_path.is_file():
                try:
                    evidence = json.loads(profile_path.read_text())
                except (OSError, json.JSONDecodeError):
                    evidence = None
                if isinstance(evidence, dict) and evidence.get("profile") == name:
                    profile = evidence
            profile.update(passed=False, error=str(exc))
        profile["artifacts"] = {name: str(profile_path.parent / name)
                                for name in ("run-evidence.json", "topology.json",
                                             "carrier-samples.ndjson", "fault-events.ndjson",
                                             "windowed_path_bytes.ndjson", "client-flow-trace.ndjson",
                                             "server-flow-trace.ndjson")
                                if (profile_path.parent / name).is_file()}
        profile_path.parent.mkdir(parents=True, exist_ok=True)
        profile_path.write_text(json.dumps(profile, indent=2) + "\n")
        if not profile.get("passed"):
            print(f"FAIL: {name}; {profile.get('error', 'strict assertions failed')}; result={profile_path}",
                  file=sys.stderr, flush=True)
        else:
            print(f"PASS: {name}; result={profile_path}", flush=True)
        summary["profiles"].append(profile)
        summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    summary["passed"] = all(profile.get("passed") is True for profile in summary["profiles"])
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{'PASS' if summary['passed'] else 'FAIL'}: intent-strict suite; result={summary_path}")
    return 0 if summary["passed"] else 1


def production_smoke(args: argparse.Namespace) -> int:
    six_flow_mode = args.command == "six-flow-integrity"
    intent_mode = six_flow_mode and args.intent_acceptance
    migration_mode = six_flow_mode and args.intent_migration_acceptance
    fixed_pool = six_flow_mode and args.fixed_pool
    carriers_per_path = args.carriers_per_path if six_flow_mode else 1
    if carriers_per_path > 1 and not fixed_pool:
        raise HarnessError("--carriers-per-path above one requires --fixed-pool")
    if intent_mode and (not fixed_pool or carriers_per_path < 2):
        raise HarnessError("--intent-acceptance requires --fixed-pool with at least two carriers per path")
    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise HarnessError(f"candidate PPP binary is not executable: {binary}")
    if not six_flow_mode and (args.repeats < 2 or args.repeats > 50 or args.payload_bytes < 1024 or
            args.latency_probes < 5 or args.timeout <= 0 or args.startup_timeout <= 0 or
            args.startup_wait < 0 or args.min_throughput_ratio <= 0 or args.max_p99_ratio <= 0):
        raise HarnessError("repeats must be 2..50, payload at least 1024 bytes, and at least 5 latency probes")
    root = Path(__file__).resolve().parents[3]
    compat = root / "tools" / "compat"
    output = Path(args.output).resolve()
    if not six_flow_mode:
        output.mkdir(parents=True, exist_ok=True)
        output.chmod(0o700)
    run_binary = output / "ppp-candidate"
    shutil.copy2(binary, run_binary)
    binary_sha256 = _sha256_file(run_binary)
    candidate_file = output / "candidate.json"
    candidate_identity = {"source_binary": str(binary), "run_binary": str(run_binary), "sha256": binary_sha256}
    candidate_file.write_text(json.dumps(candidate_identity, indent=2) + "\n")
    candidate_file.chmod(0o600)
    if six_flow_mode:
        args.evidence["candidate"] = candidate_identity
    tbf_burst_kb = args.tbf_burst_kb if six_flow_mode else DEFAULT_TBF_BURST_KB
    congestion_control = args.congestion_control if six_flow_mode else None
    lab = Lab(args.profile, tbf_burst_kb, ack_priority=args.ack_priority,
              congestion_control=congestion_control)
    children: list[subprocess.Popen] = []
    handles = []
    secret_path: Path | None = None
    try:
        lab.setup()
        lab.verify()
        # Both the sysctl and its readback run before any endpoint starts, so
        # every candidate transport socket is created with the selected
        # algorithm. The default (no option) leaves the kernel value untouched.
        lab.apply_congestion_control()
        topology_path = output / "topology.json"
        topology = lab.topology()
        if six_flow_mode:
            topology["executor_concurrency"] = args.concurrent
            topology["tbf_burst_kb"] = tbf_burst_kb
        topology_path.write_text(json.dumps(topology, indent=2) + "\n")
        if six_flow_mode:
            args.evidence["topology"] = str(topology_path)
        server_template = json.loads((compat / "server.json").read_text())
        client_template = json.loads((compat / "client_proxy.json").read_text())
        for template in (server_template, client_template):
            template["telemetry"].update({"enabled": True, "level": 2, "console-log": True})
            if six_flow_mode:
                template["concurrent"] = args.concurrent
                template["mux"] = {"mode": "flow", "turbo": not fixed_pool}
                if args.intent:
                    template["mux"]["intent"] = {
                        "enabled": True,
                        "mode": "auto",
                        "window_ms": 250,
                        "min_samples": 16384,
                        "hysteresis": 20,
                        "hold_ms": 5000,
                    }
                    if args.intent_acceptance and not getattr(args, "strict_profile", None):
                        template["mux"]["intent"]["rules"] = [
                            {"intent": "latency", "ports": [19004]},
                            {"intent": "bandwidth", "ports": [19003]},
                        ]
                # Intent-routed short exchanges require immediate carrier and
                # proxy writes. TCP_NODELAY is part of the latency policy and
                # avoids adding a full carrier RTT to each request/response.
                if args.tcp_turbo or args.intent:
                    template["tcp"]["turbo"] = True
                if args.tcp_cwnd is not None:
                    template["tcp"]["cwnd"] = args.tcp_cwnd
                if args.pool_metrics or args.intent_acceptance or migration_mode:
                    template["telemetry"].update({"count": True, "console-metric": True})
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
        if six_flow_mode:
            client_template["client"]["guid"] = "{" + str(uuid.uuid4()) + "}"
        trial_index = 0
        def start_trial(label: str, repeat: int, selected_paths: list[dict], multipath: bool):
            nonlocal trial_index
            trial_index += 1
            server_port = 20000 + trial_index
            if getattr(args, "strict_profile", None):
                for path in selected_paths:
                    path["carrier_listener_port"] = server_port
            server = copy.deepcopy(server_template)
            server["tcp"]["listen"]["port"] = server_port
            server["udp"]["listen"]["port"] = server_port
            if multipath:
                # Keep the VMUX session resumable when the primary carrier is
                # one of the paths under fault injection. Child carriers on a
                # surviving path must carry the flow while the primary TCP
                # socket is re-established.
                server["server"]["multipath"] = {
                    "enabled": True, "resume_grace_ms": 30000}
            else:
                server["server"].pop("multipath", None)
            client = copy.deepcopy(client_template)
            primary_path = selected_paths[-1] if getattr(args, "strict_profile", None) == \
                "intent-strict-credit-blackhole" else selected_paths[0]
            client["client"]["server"] = f"ppp://{primary_path['server_ip']}:{server_port}/"
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
                connection_count = len(selected_paths) * carriers_per_path
                max_per_path = (4 if getattr(args, "strict_profile", None) == "intent-strict-dynamic-tide"
                                else carriers_per_path)
                configured_paths = list(selected_paths)
                if getattr(args, "strict_profile", None) == "intent-strict-credit-blackhole":
                    # The strict blackhole deliberately removes path1 and
                    # path2. Make path3 the VMUX primary so the surviving
                    # carrier remains the session anchor while the other
                    # paths are being recovered.
                    configured_paths.sort(key=lambda item: item["id"] != "path3")
                client["client"]["multipath"] = {
                    "enabled": True, "policy": "adaptive", "max_connections": len(selected_paths) * max_per_path,
                    "max_concurrent_connects": min(connection_count, 4), "resume_grace_ms": 30000,
                    "paths": [{"id": p["id"], "transport": "tcp", "connect_host": p["server_ip"],
                               "port": server_port, "connections": max_per_path} for p in configured_paths],
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
            candidate_args = ["--rt=no"] if six_flow_mode else ([] if multipath else ["--tun-mux=1"])
            candidate_cwd = output if six_flow_mode else None
            server_command = ["ip", "netns", "exec", lab.names["server"], str(run_binary),
                              "--mode=server", f"--config={server_config}", *candidate_args]
            client_command = ["ip", "netns", "exec", lab.names["client"], str(run_binary),
                              "--mode=proxy", f"--config={client_config}", "--tun-host=no", "--tun-vnet=no",
                              *candidate_args, "--proxy-subnet=yes", "--proxy-internet=yes",
                              "--tun-protect=no", "--tun-route=no"]
            if six_flow_mode:
                server_command.append(f"--stats-json={output / 'server-stats.ndjson'}")
                client_command.append(f"--stats-json={output / 'client-stats.ndjson'}")
                args.evidence["candidate_commands"] = {"server": server_command, "client": client_command}
                args.evidence["candidate_logs"] = {"server": server_log.name, "client": client_log.name}
            server_env = None
            if migration_mode or getattr(args, "strict_profile", None) == "intent-strict-burst-kill":
                server_trace = output / "server-flow-trace.ndjson"
                server_env = {**os.environ, "OPENPPP2_MULTIPATH_FLOW_TRACE_JSON": str(server_trace)}
                args.evidence["server_flow_trace"] = str(server_trace)
            server_process = spawn_owned(children, server_command, stdout=server_log, cwd=candidate_cwd,
                                         stderr=subprocess.STDOUT, env=server_env)
            time.sleep(args.startup_wait)
            if server_process.poll() is not None:
                raise HarnessError(f"candidate server exited; inspect {output / f'{label}-{repeat}-server.log'}")
            client_env = None
            if migration_mode or getattr(args, "strict_profile", None):
                flow_trace = output / "client-flow-trace.ndjson"
                client_env = {**os.environ, "OPENPPP2_MULTIPATH_FLOW_TRACE_JSON": str(flow_trace)}
                args.evidence["flow_trace"] = str(flow_trace)
            client_process = spawn_owned(children, client_command, stdout=client_log, cwd=candidate_cwd,
                                         stderr=subprocess.STDOUT, env=client_env)
            dynamic_tide = getattr(args, "strict_profile", None) == "intent-strict-dynamic-tide"
            required_paths = selected_paths if fixed_pool or dynamic_tide else selected_paths[:1]
            required_count = carriers_per_path if fixed_pool else 1
            def carrier_counts() -> dict[str, int]:
                sockets = established_path_endpoints(lab.names["client"])
                return {path["id"]: sockets[path["server_ip"], server_port] for path in required_paths}

            deadline = time.monotonic() + args.startup_timeout
            def mux_ready() -> bool:
                server_text = Path(server_log.name).read_text(errors="replace")
                if multipath:
                    # Gate the first carrier, not just eventual success on a retry.
                    first = re.search(r"\[multipath\].*?first authenticated record type=(\d+)", server_text)
                    if not first:
                        return False
                    if int(first[1]) != 0x10:  # wire::RecordType::CreateRequest
                        raise HarnessError(f"fresh carrier sent record {first[1]} instead of CreateRequest")
                client_text = Path(client_log.name).read_text(errors="replace")
                ready_count = len(required_paths) * required_count
                return all(len(re.findall(r"\[mux\][^\n]* :: link open\b", text)) >= ready_count
                           for text in (client_text, server_text))
            def strict_pool_ready() -> bool:
                if not getattr(args, "strict_profile", None):
                    return True
                snapshot = _strict_runtime_sample(output, int(time.monotonic() * 1000))
                active = {path["id"]: path["carriers"] for path in snapshot["paths"]}
                return all(active.get(path["id"]) == required_count for path in required_paths)

            while time.monotonic() < deadline:
                if (mux_ready() and all(count == required_count for count in carrier_counts().values())
                        and strict_pool_ready()):
                    break
                if server_process.poll() is not None or client_process.poll() is not None:
                    raise HarnessError(f"candidate PPP process exited; inspect {output}/*.log")
                time.sleep(0.1)
            counts = carrier_counts()
            if six_flow_mode:
                args.evidence["startup_carriers"] = counts
            if any(count != required_count for count in counts.values()):
                raise HarnessError(f"required carrier readiness timed out for {label} trial {repeat}: {counts}")
            if not mux_ready():
                raise HarnessError(f"VMUX link-open readiness timed out for {label} trial {repeat}")
            if not strict_pool_ready():
                raise HarnessError(f"strict runtime pool readiness timed out for {label} trial {repeat}")
            return server_process, client_process

        if six_flow_mode:
            selected_paths = lab.paths
            if args.single_path:
                selected_paths = [path for path in lab.paths if path["id"] == args.single_path]
                if not selected_paths:
                    raise HarnessError(f"{args.profile} has no path named {args.single_path}")
            args.evidence["configured_carriers"] = {path["id"]: carriers_per_path for path in selected_paths}
            impairment = six_flow_impairment(args, selected_paths)
            server_process, client_process = start_trial("six-flow", 0, selected_paths, True)
            if getattr(args, "strict_profile", None):
                strict_result = strict_profile_acceptance(lab, children, output, args.strict_profile)
                (output / "intent-strict.json").write_text(json.dumps(strict_result, indent=2) + "\n")
                return 0
            if migration_mode:
                try:
                    migration = intent_migration_acceptance(lab, children, output)
                except (HarnessError, subprocess.TimeoutExpired) as failure:
                    migration = {"passed": False, "error": str(failure)}
                if server_process.poll() is not None or client_process.poll() is not None:
                    migration.update(passed=False, error="candidate exited during intent migration acceptance")
                result_path = output / "intent-migration.json"
                result_path.write_text(json.dumps({
                    "kind": "production-multipath-intent-migration", "profile": args.profile,
                    "candidate": candidate_identity, "topology": str(topology_path),
                    "fixed_pool": fixed_pool, "carriers_per_path": carriers_per_path, **migration,
                }, indent=2) + "\n")
                if not migration["passed"]:
                    print(f"FAIL: intent migration acceptance; result={result_path}", file=sys.stderr)
                return 0 if migration["passed"] else 1
            if intent_mode:
                directions = intent_acceptance(lab, children, output)
            else:
                directions = six_flow_acceptance(lab, children, output, args.flows, args.duration, args.direction,
                                                 impairment, args.wire_interval, args.qdisc_metrics,
                                                 args.socket_metrics)
            if server_process.poll() is not None or client_process.poll() is not None:
                raise HarnessError("candidate exited during six-flow acceptance")
            result = {"kind": "production-multipath-intent" if intent_mode else "production-multipath-six-flow",
                      "profile": args.profile,
                      "candidate": {"path": str(run_binary), "sha256": binary_sha256},
                      "topology": str(topology_path), "paths": [path["id"] for path in selected_paths],
                      "fixed_pool": fixed_pool,
                      "tcp_turbo": args.tcp_turbo,
                      "carriers_per_path": carriers_per_path,
                      "executor_concurrency": args.concurrent,
                      "tbf_burst_kb": args.tbf_burst_kb,
                      "ack_priority": args.ack_priority,
                      "congestion_control": args.congestion_control,
                      "direction_mode": args.direction,
                      "directions": directions, "passed": True}
            result_path = output / ("intent-acceptance.json" if intent_mode else "six-flow-integrity.json")
            result_path.write_text(json.dumps(result, indent=2) + "\n")
            return 0

        echo = spawn_owned(children, ["ip", "netns", "exec", lab.names["server"], sys.executable, "-u", "-c",
            "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); "
            f"s.bind(('0.0.0.0',{echo_port})); s.listen(); "
            "\nwhile True:\n c,_=s.accept()\n with c:\n  while True:\n   b=c.recv(65536)\n   if not b: break\n   c.sendall(b)"],
            stdout=(output / "echo.log").open("w"), stderr=subprocess.STDOUT)
        time.sleep(0.1)
        if echo.poll() is not None:
            raise HarnessError(f"echo fixture exited; inspect {output / 'echo.log'}")

        def measure(label: str, selected_paths: list[dict], multipath: bool) -> dict:
            trials = []
            for repeat in range(args.repeats):
                server_process, client_process = start_trial(label, repeat, selected_paths, multipath)
                before = lab.path_egress_bytes()
                transfer_path = output / f"{label}-{repeat}-transfer.json"
                transfer_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], sys.executable,
                    PAYLOAD, "benchmark", BUSINESS_IP, str(echo_port), str(args.payload_bytes), str(args.timeout),
                    f"{label}-{repeat}", str(args.latency_probes), str(transfer_path)],
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
                # Keep the receipt traffic outside the timed echo and path-counter window.
                measurement["directional"] = check_directional_receipts(
                    lab, children, args.payload_bytes, args.timeout, f"{label}-{repeat}")
                transfer_path.write_text(json.dumps(measurement) + "\n")
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


RECOVERY_WORKLOAD = str(Path(__file__).with_name("recovery_workload.py").resolve())


def _wait_for_ready(path: Path, process: subprocess.Popen, timeout: float, label: str) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise HarnessError(f"{label} exited before establishing the application stream")
        time.sleep(0.05)
    raise HarnessError(f"{label} did not establish the application stream within {timeout:g}s")


def _path_counter_evidence(start: dict[str, int], fault_start: dict[str, int],
                           fault_end: dict[str, int], end: dict[str, int]) -> dict:
    evidence = {}
    for path_id in sorted(start):
        start_bytes = start[path_id]
        before_fault = fault_start[path_id]
        after_fault = fault_end[path_id]
        end_bytes = end[path_id]
        evidence[path_id] = {
            "counter_start": start_bytes,
            "counter_at_fault_start": before_fault,
            "counter_at_fault_end": after_fault,
            "counter_end": end_bytes,
            "pre_fault_bytes": max(0, before_fault - start_bytes),
            "during_fault_bytes": max(0, after_fault - before_fault),
            "post_fault_bytes": max(0, end_bytes - after_fault),
            "total_bytes": max(0, end_bytes - start_bytes),
        }
    return evidence


def _require_path_counter_evidence(evidence: dict, fault_ids: set[str], minimum: int,
                                   *, expect_recovery: bool) -> None:
    if not evidence:
        raise HarnessError("fault matrix produced no per-path counter evidence")
    missing = [path_id for path_id, item in evidence.items()
               if item["pre_fault_bytes"] < minimum or item["total_bytes"] < minimum]
    if missing:
        raise HarnessError(f"paths did not reach the minimum carrier byte delta: {missing}; evidence={evidence}")
    if expect_recovery:
        survivors = [item for path_id, item in evidence.items() if path_id not in fault_ids]
        if survivors:
            if any(item["post_fault_bytes"] < minimum for item in survivors):
                raise HarnessError(f"surviving paths did not reach the minimum post-fault carrier byte delta: {evidence}")
        elif max(item["post_fault_bytes"] for item in evidence.values()) < minimum:
            raise HarnessError(f"no path reached the minimum carrier byte delta after restoration: {evidence}")


def _validate_recovery_result(path: Path, label: str) -> dict:
    try:
        result = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise HarnessError(f"{label} did not produce a valid recovery result: {exc}") from exc
    if (type(result.get("sent_bytes")) is not int or result["sent_bytes"] <= 0 or
            result.get("sent_bytes") != result.get("received_bytes")):
        raise HarnessError(f"{label} reported incomplete application bytes")
    if (not isinstance(result.get("sent_sha256"), str) or
            result["sent_sha256"] != result.get("received_sha256") or
            not re.fullmatch(r"[0-9a-f]{64}", result["sent_sha256"])):
        raise HarnessError(f"{label} reported mismatched application SHA-256")
    if result.get("eof") is not True or result.get("connection_errors"):
        raise HarnessError(f"{label} did not complete a clean half-close: {result}")
    return result

def _runtime_event_evidence(output: Path, label: str) -> dict[str, bool]:
    texts = []
    for role in ("server", "client"):
        log = output / f"{label}-{role}.log"
        try:
            texts.append(log.read_text(errors="replace"))
        except OSError:
            texts.append("")
    combined = "\n".join(texts)
    return {
        "carrier_exit_isolated": "carrier exit isolated" in combined,
        "resume_window": "last carrier exited; retaining session for bounded resume window" in combined,
    }


def production_fault_matrix(args: argparse.Namespace) -> int:
    if args.outage_ms <= 0 or args.grace_ms <= 0 or args.stream_seconds <= 0 or args.timeout <= 0:
        raise HarnessError("outage, grace, stream duration, and timeout must be positive")
    if (args.fault_lead_ms <= 0 or args.minimum_path_bytes <= 0 or args.startup_timeout <= 0 or
            args.startup_wait < 0):
        raise HarnessError("fault lead, minimum path bytes, and startup timing must be valid")
    if "all-short-outage" in args.cases and args.outage_ms >= args.grace_ms:
        raise HarnessError("all-short-outage must be shorter than the configured session grace")
    if len(PROFILES[args.profile]["paths"]) < 2:
        raise HarnessError("fault matrix requires at least two configured paths")

    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise HarnessError(f"candidate PPP binary is not executable: {binary}")
    root = Path(__file__).resolve().parents[3]
    compat = root / "tools" / "compat"
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    output.chmod(0o700)
    run_binary = output / "ppp-candidate"
    shutil.copy2(binary, run_binary)
    binary_sha256 = _sha256_file(run_binary)
    children: list[subprocess.Popen] = []
    handles = []
    lab = Lab(args.profile, ack_priority=args.ack_priority)
    secret_path: Path | None = None
    cases = []
    try:
        lab.setup()
        lab.verify()
        topology_path = output / "topology.json"
        topology_path.write_text(json.dumps(lab.topology(), indent=2) + "\n")
        server_template = json.loads((compat / "server.json").read_text())
        client_template = json.loads((compat / "client_proxy.json").read_text())
        for template in (server_template, client_template):
            template["telemetry"].update({"enabled": True, "level": 2, "console-log": True})
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
        policy_rules = output / "multipath-policy.rules"
        policy_rules.write_text("default proxy\ndns direct direct\ndns proxy proxy\n")
        policy_rules.chmod(0o600)

        echo_port = 19003
        echo_log = (output / "echo.log").open("w")
        echo = spawn_owned(children, ["ip", "netns", "exec", lab.names["server"], sys.executable, "-u", "-c",
            "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); "
            f"s.bind(('0.0.0.0',{echo_port})); s.listen(); "
            "\nwhile True:\n c,_=s.accept()\n with c:\n  while True:\n   b=c.recv(65536)\n   if not b: break\n   c.sendall(b)"], stdout=echo_log, stderr=subprocess.STDOUT)
        handles.append(echo_log)
        time.sleep(0.1)
        if echo.poll() is not None:
            raise HarnessError("recovery echo fixture exited")

        session_number = 0

        def start_session(label: str):
            nonlocal session_number
            session_number += 1
            server_port = 23000 + session_number
            server = copy.deepcopy(server_template)
            server["tcp"]["listen"]["port"] = server_port
            server["udp"]["listen"]["port"] = server_port
            server["server"]["multipath"] = {"enabled": True, "resume_grace_ms": args.grace_ms}
            client = copy.deepcopy(client_template)
            client["client"]["server"] = f"ppp://{lab.paths[0]['server_ip']}:{server_port}/"
            client["client"].pop("mappings", None)
            client["client"]["policy"] = {
                "version": 2, "rules": {"path": str(policy_rules)},
                "dns": {"mode": "real", "resolvers": {
                    "direct": {"via": "direct", "servers": ["udp://192.0.2.53:53"]},
                    "proxy": {"via": "proxy", "servers": ["udp://192.0.2.53:53"]},
                }},
            }
            client["client"]["multipath"] = {
                "enabled": True, "policy": "adaptive", "max_connections": len(lab.paths),
                "max_concurrent_connects": len(lab.paths), "resume_grace_ms": args.grace_ms,
                "paths": [{"id": p["id"], "transport": "tcp", "connect_host": p["server_ip"],
                           "port": server_port, "connections": 1} for p in lab.paths],
            }
            server_config = output / f"{label}-server.json"
            client_config = output / f"{label}-client.json"
            server_config.write_text(json.dumps(server, indent=2) + "\n")
            client_config.write_text(json.dumps(client, indent=2) + "\n")
            server_config.chmod(0o600)
            client_config.chmod(0o600)
            server_log = (output / f"{label}-server.log").open("w")
            client_log = (output / f"{label}-client.log").open("w")
            handles.extend((server_log, client_log))
            server_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["server"], str(run_binary),
                "--mode=server", f"--config={server_config}"], stdout=server_log, stderr=subprocess.STDOUT)
            time.sleep(args.startup_wait)
            if server_process.poll() is not None:
                raise HarnessError(f"{label} server exited during startup")
            client_process = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], str(run_binary),
                "--mode=proxy", f"--config={client_config}", "--tun-host=no", "--tun-vnet=no",
                "--proxy-subnet=yes", "--proxy-internet=yes", "--tun-protect=no", "--tun-route=no",
                f"--stats-json={output / (label + '-client-stats.ndjson')}"],
                stdout=client_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.startup_timeout
            primary = (lab.paths[0]["server_ip"], server_port)
            while time.monotonic() < deadline:
                server_text = Path(server_log.name).read_text(errors="replace")
                client_text = Path(client_log.name).read_text(errors="replace")
                ready = all(re.search(r"\[mux\][^\n]* :: link open\b", text)
                            for text in (client_text, server_text))
                if ready:
                    first = re.search(r"\[multipath\].*?first authenticated record type=(\d+)", server_text)
                    if not first:
                        time.sleep(0.1)
                        continue
                    if int(first[1]) != 0x10:
                        raise HarnessError(f"{label} fresh carrier sent record {first[1]} instead of CreateRequest")
                    if primary in established_path_endpoints(lab.names["client"]):
                        return server_process, client_process
                if server_process.poll() is not None or client_process.poll() is not None:
                    raise HarnessError(f"{label} candidate exited during startup")
                time.sleep(0.1)
            raise HarnessError(f"{label} VMUX/carrier readiness timed out")

        def stop_session(processes):
            if processes is None:
                return
            for process in reversed(processes):
                if process in children:
                    terminate_process_group(process)
                    children.remove(process)

        def run_fresh_session_check(name: str) -> dict:
            fresh_session = None
            fresh_workload = None
            result_path = output / f"{name}-fresh-workload.json"
            ready_path = output / f"{name}-fresh-ready"
            ready_path.unlink(missing_ok=True)
            try:
                fresh_session = start_session(f"{name}-fresh")
                fresh_workload = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], sys.executable,
                    RECOVERY_WORKLOAD, "client", BUSINESS_IP, str(echo_port), "5", str(result_path),
                    "--ready-file", str(ready_path), "--io-timeout", "20"],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                _wait_for_ready(ready_path, fresh_workload, args.startup_timeout, f"{name}-fresh")
                stdout, stderr = fresh_workload.communicate(timeout=args.timeout)
                if fresh_workload in children:
                    children.remove(fresh_workload)
                if fresh_workload.returncode:
                    raise HarnessError(f"{name} fresh session workload failed: {stderr.strip()}")
                result = _validate_recovery_result(result_path, f"{name} fresh session")
                result["stdout"] = stdout.strip()
                return result
            finally:
                stop_session(fresh_session)
                if fresh_workload is not None and fresh_workload in children:
                    terminate_process_group(fresh_workload)
                    children.remove(fresh_workload)

        def run_case(name: str, fault_ids: list[str], outage_ms: int, expect_recovery: bool) -> dict:
            session = start_session(name)
            workload_result = output / f"{name}-workload.json"
            ready_file = output / f"{name}-ready"
            ready_file.unlink(missing_ok=True)
            workload_seconds = max(args.stream_seconds,
                                   args.grace_ms / 1000 + outage_ms / 1000 + 5) if not expect_recovery else args.stream_seconds
            io_timeout = max(20, args.grace_ms / 1000 + 5) if not expect_recovery else 20
            workload = None
            fault_started_at_ms = None
            fault_ended_at_ms = None
            try:
                workload = spawn_owned(children, ["ip", "netns", "exec", lab.names["client"], sys.executable,
                    RECOVERY_WORKLOAD, "client", BUSINESS_IP, str(echo_port), str(workload_seconds),
                    str(workload_result), "--ready-file", str(ready_file), "--io-timeout", str(io_timeout)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                _wait_for_ready(ready_file, workload, args.startup_timeout, name)
                counters_start = lab.path_egress_bytes()
                time.sleep(args.fault_lead_ms / 1000)
                counters_fault_start = lab.path_egress_bytes()
                selected = [path for path in lab.paths if path["id"] in set(fault_ids)]
                if len(selected) != len(fault_ids):
                    raise HarnessError(f"{name} selected unknown paths: {fault_ids}")
                for path in selected:
                    apply_path_impairment(path, "blackhole", ack_priority=lab.ack_priority)
                fault_started_at_ms = round(time.time() * 1000)
                time.sleep(outage_ms / 1000)
                fault_ended_at_ms = round(time.time() * 1000)
                counters_fault_end = lab.path_egress_bytes()
                for path in selected:
                    apply_path_impairment(path, "reset", ack_priority=lab.ack_priority)
                reset_at = time.monotonic()
                try:
                    stdout, stderr = workload.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired as exc:
                    terminate_process_group(workload)
                    if workload in children:
                        children.remove(workload)
                    raise HarnessError(f"{name} workload exceeded timeout") from exc
                if workload in children:
                    children.remove(workload)
                counters_end = lab.path_egress_bytes()
                evidence = _path_counter_evidence(counters_start, counters_fault_start,
                                                   counters_fault_end, counters_end)
                elapsed_after_reset_ms = round((time.monotonic() - reset_at) * 1000)
                runtime_events = _runtime_event_evidence(output, name)
                record = {"name": name, "fault_paths": fault_ids, "outage_ms": outage_ms,
                          "expected_recovery": expect_recovery, "workload_exit": workload.returncode,
                          "workload_stdout": stdout.strip(), "workload_stderr": stderr.strip(),
                          "fault_started_at_ms": fault_started_at_ms, "fault_ended_at_ms": fault_ended_at_ms,
                          "runtime_events": runtime_events, "path_bytes": evidence,
                          "reset_to_workload_exit_ms": elapsed_after_reset_ms,
                          "candidate": {"sha256": binary_sha256}}
                (output / f"{name}-observation.json").write_text(json.dumps(record, indent=2) + "\n")
                if name in ("single-path-outage", "primary-path-outage") and not runtime_events["carrier_exit_isolated"]:
                    raise HarnessError(f"{name} did not log an isolated carrier exit")
                if name in ("all-short-outage", "all-grace-expired") and not runtime_events["resume_window"]:
                    raise HarnessError(f"{name} did not log the bounded resume window")
                _require_path_counter_evidence(evidence, set(fault_ids), args.minimum_path_bytes,
                                                expect_recovery=expect_recovery)
                if expect_recovery:
                    if workload.returncode:
                        raise HarnessError(f"{name} application stream failed after path recovery: {stderr.strip()}")
                    record["workload"] = _validate_recovery_result(workload_result, name)
                    carrier_bytes = sum(item["total_bytes"] for item in evidence.values())
                    verified_bytes = record["workload"]["sent_bytes"] + record["workload"]["received_bytes"]
                    if carrier_bytes < verified_bytes:
                        raise HarnessError(f"{name} carrier counters under-report verified application bytes: {evidence}")
                    record["carrier_total_bytes"] = carrier_bytes
                    record["verified_application_bytes"] = verified_bytes
                    record["passed"] = True
                else:
                    if workload.returncode == 0:
                        raise HarnessError(f"{name} unexpectedly completed after grace-expired full outage")
                    try:
                        failure = json.loads(workload_result.read_text())
                    except (OSError, json.JSONDecodeError) as exc:
                        raise HarnessError(f"{name} did not produce an auditable failure result") from exc
                    if failure.get("passed") is not False or failure.get("transport_terminated") is not True:
                        raise HarnessError(f"{name} workload did not observe transport termination: {failure}")
                    if any(process.poll() is not None for process in session):
                        raise HarnessError(f"{name} candidate exited before transport termination could be verified")
                    failure_at = failure.get("ended_at_ms")
                    grace_deadline = fault_started_at_ms + args.grace_ms - 1000
                    if type(failure_at) is not int or failure_at < grace_deadline:
                        raise HarnessError(f"{name} failed before the configured grace deadline: {failure}")
                    record["workload"] = failure
                    record["failure_after_grace"] = True
                    stop_session(session)
                    session = None
                    record["fresh_session"] = run_fresh_session_check(name)
                    record["passed"] = True
                    record["expected_failure"] = True
                return record
            finally:
                for path in lab.paths:
                    apply_path_impairment(path, "reset", ack_priority=lab.ack_priority)
                stop_session(session)
                if workload is not None and workload in children:
                    terminate_process_group(workload)
                    children.remove(workload)

        all_ids = [path["id"] for path in lab.paths]
        case_specs = {
            "single-path-outage": ([all_ids[-1]], args.outage_ms, True),
            "primary-path-outage": ([all_ids[0]], args.outage_ms, True),
            "all-short-outage": (all_ids, args.outage_ms, True),
            "all-grace-expired": (all_ids, max(args.outage_ms, args.grace_ms + 2000), False),
        }
        for name in args.cases:
            fault_ids, outage_ms, expect_recovery = case_specs[name]
            try:
                case = run_case(name, fault_ids, outage_ms, expect_recovery)
            except Exception as exc:
                failure = {"kind": "production-multipath-fault-case", "name": name,
                           "fault_paths": fault_ids, "outage_ms": outage_ms, "passed": False,
                           "error": str(exc), "candidate": {"sha256": binary_sha256}}
                (output / f"{name}.json").write_text(json.dumps(failure, indent=2) + "\n")
                raise
            cases.append(case)
            (output / f"{name}.json").write_text(json.dumps(case, indent=2) + "\n")
        result = {"kind": "production-multipath-fault-matrix", "profile": args.profile,
                  "cases": cases, "topology": str(topology_path),
                  "candidate": {"path": str(run_binary), "sha256": binary_sha256},
                  "evidence_note": "The workload is one non-reconnecting SOCKS TCP stream; path bytes are TBF deltas sampled after stream readiness and before cleanup.",
                  "passed": all(case["passed"] for case in cases)}
        result_path = output / "production-fault-matrix.json"
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        print(f"{'PASS' if result['passed'] else 'FAIL'}: production fault matrix; result={result_path}")
        return 0 if result["passed"] else 1
    finally:
        previous = block_interrupt_signals()
        try:
            for child in reversed(children):
                terminate_process_group(child)
            for handle in handles:
                if hasattr(handle, "close"):
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
    ack_priority = bool(topology.get("ack_priority", False))
    apply_path_impairment(path, args.action,
                          float(args.loss.rstrip("%")) if isinstance(args.loss, str) else args.loss,
                          ack_priority=ack_priority)
    print(f"applied {args.action} to {args.path}; the path TBF rate cap remains configured"
          + ("; ACK-priority bands preserved" if ack_priority else ""))
    return 0


def apply_path_impairment(path: dict, action: str, loss_percent: float = 5,
                          ack_priority: bool = False) -> None:
    if not 0 <= loss_percent <= 100:
        raise HarnessError("loss percentage must be between 0 and 100")
    loss_by_action = {"reset": path["loss_percent"], "blackhole": 100, "drop": loss_percent}
    if action not in loss_by_action:
        raise HarnessError(f"unknown impairment action: {action}")
    for ns, dev in ((path["client_ns"], path["client_if"]),
                    (path["router_ns"], path["router_client_if"])):
        Lab._apply_path_delay(ns, dev, path["rtt_ms"], path["jitter_ms"],
                              loss_by_action[action],
                              path.get("jitter_distribution", "normal"), ack_priority)


def load_owned_topology(path: Path) -> dict:
    try:
        topology = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise HarnessError(f"cannot read topology: {exc}") from exc
    if not isinstance(topology, dict) or topology.get("schema") != 1 or topology.get("profile") not in PROFILES:
        raise HarnessError("unsupported topology schema or profile")
    if "ack_priority" in topology and not isinstance(topology["ack_priority"], bool):
        raise HarnessError("topology ack_priority must be boolean")
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
        apply_path_impairment(path, "blackhole", ack_priority=lab.ack_priority)
        for (ns, dev), rate in zip(interfaces, expected):
            records = json.loads(run(["ip", "netns", "exec", ns, "tc", "-j", "-d", "qdisc", "show", "dev", dev], capture=True))
            tbf = next((item for item in records if item.get("kind") == "tbf"), None)
            netems = [item for item in records if item.get("kind") == "netem"]
            if not tbf or int(tbf["options"]["rate"]) != rate:
                raise HarnessError(f"blackhole replacement changed TBF path cap on {ns}/{dev}")
            if lab.ack_priority and not any(item.get("kind") == "prio" for item in records):
                raise HarnessError(f"blackhole replacement dropped the prio qdisc on {ns}/{dev}")
            if not netems or any(netem.get("options", {}).get("loss-random", {}).get("loss") != 1
                                 for netem in netems):
                raise HarnessError(f"blackhole replacement did not install 100% netem loss on {ns}/{dev}")
    finally:
        apply_path_impairment(path, "reset", ack_priority=lab.ack_priority)
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
            apply_path_impairment(path, "reset", ack_priority=lab.ack_priority)
        lab.verify()


def fault_plan(args: argparse.Namespace) -> int:
    topology = load_owned_topology(Path(args.topology))
    ack_priority = bool(topology.get("ack_priority", False))
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
            apply_path_impairment(by_id[path_id], "blackhole", ack_priority=ack_priority)
        print(json.dumps({"event": "blackhole", "paths": selected, "elapsed_ms": 0}))
        time.sleep(args.outage_ms / 1000)
    except BaseException:
        for path_id in selected:
            apply_path_impairment(by_id[path_id], "reset", ack_priority=ack_priority)
        raise
    if not expires:
        for path_id in selected:
            apply_path_impairment(by_id[path_id], "reset", ack_priority=ack_priority)
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
    run_parser.add_argument("--ack-priority", action="store_true",
                            help="diagnostic A/B: small ACK/control frames get strict "
                                 "priority over bulk on each shaped egress (default: off)")
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
    smoke_parser.add_argument("--ack-priority", action="store_true",
                              help="diagnostic A/B: small ACK/control frames get strict "
                                   "priority over bulk on each shaped egress (default: off)")
    smoke_parser.set_defaults(func=production_smoke)
    six_parser = sub.add_parser("six-flow-integrity", help="require six concurrent timed streams per direction")
    six_parser.add_argument("--binary", default=str(Path(__file__).resolve().parents[3] / "bin/ppp"))
    six_parser.add_argument("--profile", choices=tuple(PROFILES),
                            default="independent-12ms-24mbit")
    six_parser.add_argument("--suite", choices=("intent-strict",),
                            help="run the registered strict TCP-intent profile suite in order")
    six_parser.add_argument("--strict-profile", choices=tuple(STRICT_PROFILE_SPECS),
                            help=argparse.SUPPRESS)
    six_parser.add_argument("--output", required=True, help="new result directory (must not already exist)")
    six_parser.add_argument("--single-path", choices=("path1", "path2", "path3"),
                            help="cap the authenticated multipath session at one carrier on this path")
    six_parser.add_argument("--pool-metrics", action="store_true",
                            help="record VMUX counters, including pool decisions, in the candidate logs")
    six_parser.add_argument("--intent", action="store_true",
                            help="enable the TCP intent classifier and flow-v2 intent path pools")
    intent_acceptance_options = six_parser.add_mutually_exclusive_group()
    intent_acceptance_options.add_argument("--intent-acceptance", action="store_true",
                            help="run four-flow download, web burst, and streaming intent gates")
    intent_acceptance_options.add_argument("--intent-migration-acceptance", action="store_true",
                            help="verify behavior-classified routing and migration on individual TCP connections")
    six_parser.add_argument("--fixed-pool", action="store_true",
                            help="disable Turbo and wait for all selected carriers before the workload")
    six_parser.add_argument("--flows", type=int, choices=range(1, 65), default=6, metavar="1..64",
                            help="number of concurrent logical streams (default: 6)")
    six_parser.add_argument("--direction", choices=("upload", "download", "sequential", "bidirectional"),
                            default="sequential", help="bidirectional sends both ways on each stream")
    six_parser.add_argument("--duration", type=int, choices=range(12, 121), metavar="12..120",
                            help="send window in seconds for each direction (default: 25 up, 12 down)")
    six_parser.add_argument("--wire-interval", type=int, choices=range(1, 7), metavar="1..6", default=6,
                            help="seconds between root TBF wire samples during each direction "
                                 "(default: 6)")
    six_parser.add_argument("--tcp-cwnd", type=int, metavar="BYTES",
                            help="diagnostic TCP send-buffer override for each endpoint")
    six_parser.add_argument("--qdisc-metrics", action="store_true",
                            help="also record per-interval TBF/netem qdisc counters in "
                                 "qdisc-metrics.ndjson (default: off)")
    six_parser.add_argument("--socket-metrics", action="store_true",
                            help="also record established TCP socket state from both endpoints in "
                                 "socket-metrics.ndjson (default: off)")
    six_parser.add_argument("--tbf-burst-kb", type=int, choices=TBF_BURST_KB_CHOICES,
                            default=DEFAULT_TBF_BURST_KB, metavar="128|256|512|1024",
                            help="harness root TBF burst in KiB for per-path egresses; the shared "
                                 "router/server link uses twice this value (default: 128)")
    six_parser.add_argument("--ack-priority", action="store_true",
                            help="diagnostic A/B: insert a size-classified prio qdisc under the "
                                 "unchanged root TBF so pure ACKs bypass bulk queues (default: off)")
    six_parser.add_argument("--congestion-control", choices=CONGESTION_CONTROL_CHOICES,
                            metavar="bbr|cubic",
                            help="set net.ipv4.tcp_congestion_control inside the fixture's client, "
                                 "router, and server namespaces after setup verification and before "
                                 "the endpoints start (default: leave the kernel default untouched)")
    six_parser.add_argument("--tcp-turbo", action="store_true",
                            help="enable tcp.turbo on both candidate endpoints")
    six_parser.add_argument("--carriers-per-path", type=int, choices=(1, 2, 3, 4), default=1,
                            help="number of TCP carriers on each selected path in a fixed pool")
    six_parser.add_argument("--concurrent", type=int, choices=range(1, 65), default=1, metavar="1..64",
                            help="top-level executor worker threads on both candidate endpoints "
                                 "(default: 1, the compatibility baseline)")
    six_parser.add_argument("--impairment", choices=("delay", "rate", "blackhole"),
                            help="temporarily degrade one path during the timed workload")
    six_parser.add_argument("--impair-path", choices=("path1", "path2", "path3"))
    six_parser.add_argument("--impair-value", type=int, help="added RTT in ms or reduced cap in Mbit/s")
    six_parser.add_argument("--impair-after", type=int, default=15,
                            help="seconds from phase start before applying the impairment (default: 15)")
    six_parser.add_argument("--impair-for", type=int, default=12,
                            help="seconds before restoring the path (default: 12)")
    six_parser.set_defaults(func=production_six_flow, startup_timeout=15, startup_wait=1)
    matrix_parser = sub.add_parser("production-fault-matrix",
                                   help="run persistent application traffic through real path outages")
    matrix_parser.add_argument("--binary", default=str(Path(__file__).resolve().parents[3] / "bin/ppp"))
    matrix_parser.add_argument("--profile", choices=PROFILES, default="asymmetric-5-80-200")
    matrix_parser.add_argument("--output", required=True)
    matrix_parser.add_argument("--cases", nargs="+", choices=("single-path-outage", "primary-path-outage",
                            "all-short-outage", "all-grace-expired"),
                            default=["single-path-outage", "primary-path-outage", "all-short-outage",
                                     "all-grace-expired"])
    matrix_parser.add_argument("--outage-ms", type=int, default=1500)
    matrix_parser.add_argument("--grace-ms", type=int, default=30000)
    matrix_parser.add_argument("--stream-seconds", type=float, default=45)
    matrix_parser.add_argument("--fault-lead-ms", type=int, default=5000)
    matrix_parser.add_argument("--minimum-path-bytes", type=int, default=16384)
    matrix_parser.add_argument("--timeout", type=float, default=90)
    matrix_parser.add_argument("--startup-timeout", type=float, default=15)
    matrix_parser.add_argument("--startup-wait", type=float, default=2)
    matrix_parser.add_argument("--ack-priority", action="store_true",
                               help="diagnostic A/B: small ACK/control frames get strict "
                                    "priority over bulk on each shaped egress (default: off)")
    matrix_parser.set_defaults(func=production_fault_matrix)
    infra_parser = sub.add_parser("infrastructure-check", help="fixture capability check; not production acceptance")
    infra_parser.add_argument("--profile", choices=PROFILES, default="shared-5-80-200")
    infra_parser.add_argument("--ack-priority", action="store_true",
                              help="also verify the ACK-priority qdisc hierarchy and impairment "
                                   "restore path (default: off)")
    args = parser.parse_args()
    try:
        if args.command == "infrastructure-check":
            lab = Lab(args.profile, ack_priority=args.ack_priority)
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
