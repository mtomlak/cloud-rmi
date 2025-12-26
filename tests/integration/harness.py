#!/usr/bin/env python3
"""
Shared harness utilities for integration testing.

This module centralizes process management, protocol sessions, and
zombie detection so test cases can focus on assertions rather than
boilerplate.
"""

from __future__ import annotations

import base64
import logging
import os
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, List, Optional

LOG = logging.getLogger("integration.harness")


def parse_status(line: str) -> tuple[int, str]:
    parts = line.split(" ", 1)
    try:
        code = int(parts[0])
    except Exception:
        return 0, ""
    return code, parts[1] if len(parts) > 1 else ""


def parse_max_proto(ext: str) -> int:
    proto = ext.split("PROTO=", 1)
    if len(proto) == 1:
        return 1

    digits: list[str] = []
    for ch in proto[1]:
        if ch.isdigit():
            digits.append(ch)
        else:
            break

    return int("".join(digits)) if digits else 1


def wait_for_port(host: str, port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"Port {port} on {host} did not open within {timeout}s")


def wait_for_unix_socket(path: str, timeout: float) -> None:
    deadline = time.time() + timeout
    socket_path = Path(path)
    while time.time() < deadline:
        if socket_path.exists():
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                    sock.settimeout(1)
                    sock.connect(str(socket_path))
                return
            except OSError:
                time.sleep(0.1)
                continue
        time.sleep(0.1)
    raise TimeoutError(f"Unix socket {path} did not open within {timeout}s")


def snapshot_processes() -> set[int]:
    """
    Capture the current set of zombie PIDs for rmi-server and rmi-proxy.
    """
    output = subprocess.check_output(["ps", "-eo", "pid,state,comm"], text=True)
    zombies: set[int] = set()
    for line in output.strip().splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        pid, state, comm = parts
        if state == "Z" and comm in {"rmi-server", "rmi-proxy"}:
            try:
                zombies.add(int(pid))
            except ValueError:
                continue
    return zombies


@dataclass
class WireEvent:
    direction: str
    content: str


@dataclass
class WireTrace:
    events: list[WireEvent] = field(default_factory=list)

    def record(self, direction: str, content: str) -> None:
        self.events.append(WireEvent(direction, content))

    def extend(self, direction: str, prefix: str, payload: bytes, limit: int = 64) -> None:
        preview = payload[:limit]
        suffix = "" if len(payload) <= limit else f"... (+{len(payload) - limit} bytes)"
        self.record(direction, f"{prefix} ({len(payload)} bytes) {preview!r}{suffix}")

    def formatted(self) -> list[str]:
        return [f"{evt.direction}: {evt.content}" for evt in self.events]


@dataclass
class CallResult:
    status: int
    message: str
    payload: bytes
    decoded: str
    protocol_version: int
    trace: WireTrace


class ProtocolSession:
    def __init__(
        self,
        host: str,
        port: int,
        token: str,
        namespace: str,
        timeout: float = 5.0,
        unix_socket: str | None = None,
        negotiate_proto: bool = True,
    ) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.namespace = namespace
        self.timeout = timeout
        self.unix_socket = unix_socket
        self.sock: Optional[socket.socket] = None
        self.protocol_version = 1
        self.trace = WireTrace()
        self.negotiate_proto = negotiate_proto

    def __enter__(self) -> "ProtocolSession":
        self.connect()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def connect(self) -> None:
        if self.unix_socket:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect(self.unix_socket)
        else:
            self.sock = socket.create_connection((self.host, self.port))
            self.sock.settimeout(self.timeout)
        banner_line = self._read_line()
        self.trace.record("recv", banner_line)
        status, message = parse_status(banner_line)
        if status != 220:
            raise RuntimeError("Failed banner during connect")

        server_max_proto = 1
        if " EXT " in banner_line:
            server_max_proto = parse_max_proto(banner_line.split(" EXT ", 1)[1])

        self._send_line(f"AUTH TOKEN {self.token}")
        auth_status, _ = parse_status(self._read_line())
        if auth_status != 200:
            raise RuntimeError("Authentication failed")

        if server_max_proto >= 2 and self.negotiate_proto:
            self._send_line("PROTO 2")
            proto_status, _ = parse_status(self._read_line())
            if proto_status == 200:
                self.protocol_version = 2

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def _send_line(self, line: str) -> None:
        if not self.sock:
            raise RuntimeError("Socket not connected")
        self.trace.record("send", line)
        self.sock.sendall(f"{line}\n".encode("utf-8"))

    def _send_raw(self, payload: bytes) -> None:
        if not self.sock:
            raise RuntimeError("Socket not connected")
        self.trace.extend("send", "payload-bytes", payload)
        if payload:
            self.sock.sendall(payload)

    def _read_line(self) -> str:
        if not self.sock:
            raise RuntimeError("Socket not connected")
        data = bytearray()
        while True:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            data.extend(chunk)
            if chunk == b"\n":
                break
        line = data.decode("utf-8", errors="replace").strip("\r\n")
        self.trace.record("recv", line)
        return line

    def _read_exact(self, length: int) -> bytes:
        if not self.sock:
            raise RuntimeError("Socket not connected")
        payload = bytearray()
        remaining = length
        while remaining > 0:
            chunk = self.sock.recv(remaining)
            if not chunk:
                break
            payload.extend(chunk)
            remaining -= len(chunk)
        return bytes(payload)

    def call(
        self,
        iface: str,
        method: str,
        arg: str | None = None,
        payload: bytes | None = None,
        encoding: str = "RAW",
        decode_output: bool = True,
    ) -> CallResult:
        if not self.sock:
            raise RuntimeError("Socket not connected")

        iface_name = iface
        if self.namespace and not iface.startswith("\\"):
            iface_name = f"{self.namespace}\\{iface}"

        if payload is not None:
            self._send_line(f"CALL {iface_name} {method} PAYLOAD {encoding} {len(payload)}")
            self._send_raw(payload)
        else:
            encoded_arg = base64.b64encode((arg or "").encode("utf-8")).decode("ascii")
            self._send_line(f"CALL {iface_name} {method} {encoded_arg}")

        status, message = parse_status(self._read_line())
        payload_resp = b""

        if self.protocol_version >= 2 and status in (200, 500):
            parts = message.split(" ")
            payload_len = None

            if len(parts) >= 3 and parts[0] == "PAYLOAD":
                try:
                    payload_len = int(parts[2])
                except ValueError:
                    payload_len = None

            if payload_len is not None and payload_len >= 0:
                payload_resp = self._read_exact(payload_len)
                self.trace.extend("recv", "payload-bytes", payload_resp)

        decoded = ""
        if decode_output and status == 200:
            if payload_resp:
                decoded = payload_resp.decode("utf-8", errors="replace")
            elif message:
                prefix = ""
                base64_payload = message

                if ":" in message:
                    prefix, base64_payload = message.split(":", 1)

                try:
                    decoded_payload = base64.b64decode(base64_payload).decode("utf-8", errors="replace")
                    decoded = f"{prefix}:{decoded_payload}" if prefix else decoded_payload
                except Exception:
                    decoded = ""

        return CallResult(
            status=status,
            message=message,
            payload=payload_resp,
            decoded=decoded,
            protocol_version=self.protocol_version,
            trace=self.trace,
        )


@dataclass
class ProcessHandle:
    name: str
    process: subprocess.Popen
    logger: logging.Logger
    output: list[str] = field(default_factory=list)

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()


def _capture_output(handle: ProcessHandle, stream_name: str, stream) -> None:
    for line in iter(stream.readline, b""):
        decoded = line.decode("utf-8", errors="replace").rstrip()
        handle.output.append(decoded)
        handle.logger.debug("%s[%s]: %s", handle.name, stream_name, decoded)


def launch_process(
    name: str,
    command: list[str],
    logger: logging.Logger,
    verbose: bool = False,
) -> ProcessHandle:
    stdout = subprocess.PIPE if verbose else subprocess.DEVNULL
    stderr = subprocess.STDOUT if verbose else subprocess.DEVNULL
    proc = subprocess.Popen(command, stdout=stdout, stderr=stderr)
    handle = ProcessHandle(name=name, process=proc, logger=logger)

    if verbose and proc.stdout is not None:
        thread = threading.Thread(
            target=_capture_output, args=(handle, "stdout", proc.stdout), daemon=True
        )
        thread.start()

    return handle


@dataclass
class IntegrationEnvironment:
    root: Path
    server_port: int
    proxy_port: int
    token: str
    namespace: str
    server_unix_socket: str | None = None
    proxy_unix_socket: str | None = None
    upstream_unix_socket: str | None = None
    verbose: bool = False
    logger: logging.Logger = LOG
    server_worker_model: str | None = None  # "fork" or "prefork"
    server_prefork_workers: int | None = None  # Number of prefork workers
    proxy_worker_model: str | None = None  # "fork" or "prefork"
    proxy_prefork_workers: int | None = None  # Number of prefork workers
    _server: Optional[ProcessHandle] = field(default=None, init=False)
    _proxy: Optional[ProcessHandle] = field(default=None, init=False)
    _baseline_zombies: set[int] = field(default_factory=set, init=False)

    def __post_init__(self) -> None:
        self._baseline_zombies = snapshot_processes()

    @property
    def server_host(self) -> str:
        return "127.0.0.1"

    @property
    def proxy_host(self) -> str:
        return "127.0.0.1"

    @property
    def config_path(self) -> Path:
        return self.root / "tests" / "integration" / "fixtures" / "rmi-server.conf"

    @property
    def interface_map(self) -> Path:
        return self.root / "tests" / "integration" / "fixtures" / "interfaces.map"

    @property
    def scripts_dir(self) -> Path:
        return self.root / "tests" / "integration" / "fixtures" / "scripts"

    @property
    def server_bin(self) -> Path:
        return self.root / "rmi-server"

    @property
    def proxy_bin(self) -> Path:
        return self.root / "rmi-proxy"

    def start_server(self) -> None:
        if self._server:
            return
        command = [
            str(self.server_bin),
            "-c",
            str(self.config_path),
            "-m",
            str(self.interface_map),
            "-p",
            str(self.server_port),
            "-e",
            str(self.scripts_dir),
            "-b",
            self.namespace,
            "-t",
            self.token,
        ]
        if self.server_unix_socket:
            Path(self.server_unix_socket).parent.mkdir(parents=True, exist_ok=True)
            command.extend(["-u", self.server_unix_socket])
        if self.server_worker_model:
            command.extend(["-w", self.server_worker_model])
        if self.server_prefork_workers is not None:
            command.extend(["-W", str(self.server_prefork_workers)])
        self.logger.debug("Starting server: %s", " ".join(command))
        self._server = launch_process("rmi-server", command, self.logger, self.verbose)
        if self.server_unix_socket:
            wait_for_unix_socket(self.server_unix_socket, 10)
        else:
            wait_for_port(self.server_host, self.server_port, 10)

    def start_proxy(self) -> None:
        if self._proxy:
            return
        upstream_target = (
            f"unix:{self.upstream_unix_socket}"
            if self.upstream_unix_socket
            else f"{self.server_host}:{self.server_port}"
        )
        command = [
            str(self.proxy_bin),
            "-c",
            str(self.config_path),
            "-m",
            str(self.interface_map),
            "-p",
            str(self.proxy_port),
            "-x",
            upstream_target,
            "-t",
            self.token,
        ]
        if self.proxy_unix_socket:
            Path(self.proxy_unix_socket).parent.mkdir(parents=True, exist_ok=True)
            command.extend(["-u", self.proxy_unix_socket])
        if self.proxy_worker_model:
            command.extend(["-w", self.proxy_worker_model])
        if self.proxy_prefork_workers is not None:
            command.extend(["-W", str(self.proxy_prefork_workers)])
        self.logger.debug("Starting proxy: %s", " ".join(command))
        self._proxy = launch_process("rmi-proxy", command, self.logger, self.verbose)
        if self.proxy_unix_socket:
            wait_for_unix_socket(self.proxy_unix_socket, 10)
        else:
            wait_for_port(self.proxy_host, self.proxy_port, 10)

    def stop_all(self) -> None:
        for handle in (self._proxy, self._server):
            if handle:
                handle.close()
        self._proxy = None
        self._server = None

    def check_no_new_zombies(self) -> None:
        current = snapshot_processes()
        new = current - self._baseline_zombies
        if new:
            zombies = ", ".join(str(pid) for pid in sorted(new))
            raise RuntimeError(f"Detected zombie processes: {zombies}")

    def server_session(self) -> ProtocolSession:
        return ProtocolSession(
            host=self.server_host,
            port=self.server_port,
            token=self.token,
            namespace=self.namespace,
            unix_socket=self.server_unix_socket,
        )

    def proxy_session(self) -> ProtocolSession:
        return ProtocolSession(
            host=self.proxy_host,
            port=self.proxy_port,
            token=self.token,
            namespace=self.namespace,
            unix_socket=self.proxy_unix_socket,
        )


@dataclass
class TestResult:
    name: str
    passed: bool
    message: str = ""
    traces: list[WireTrace] = field(default_factory=list)


TestFunc = Callable[["IntegrationEnvironment"], TestResult]


@dataclass
class TestCase:
    name: str
    runner: TestFunc
    requires_proxy: bool = False


class IntegrationRunner:
    def __init__(self, env: IntegrationEnvironment, logger: logging.Logger | None = None):
        self.env = env
        self.logger = logger or LOG

    def run(self, cases: Iterable[TestCase], verbose: bool = False) -> list[TestResult]:
        results: list[TestResult] = []
        try:
            self.env.start_server()
            for case in cases:
                if case.requires_proxy:
                    self.env.start_proxy()
                results.append(self._run_case(case, verbose=verbose))
        finally:
            self.env.stop_all()
            self.env.check_no_new_zombies()
        return results

    def _run_case(self, case: TestCase, verbose: bool) -> TestResult:
        print(f"testing {case.name}... ", end="", flush=True)
        start = time.perf_counter()
        try:
            result = case.runner(self.env)
            if not result.name:
                result.name = case.name
            outcome = "ok" if result.passed else "FAIL"
            elapsed = time.perf_counter() - start
            suffix = f" ({elapsed:.3f}s)" if verbose else ""
            print(f"{outcome}{suffix}")
            if verbose and result.traces:
                for idx, trace in enumerate(result.traces, start=1):
                    header = f"- transcript {idx} for {case.name}"
                    print(header)
                    for line in trace.formatted():
                        print(f"  {line}")
            return result
        except Exception as exc:  # pragma: no cover - defensive, integration only
            elapsed = time.perf_counter() - start
            suffix = f" ({elapsed:.3f}s)" if verbose else ""
            print(f"FAIL{suffix}")
            message = str(exc)
            self.logger.error("Test %s failed: %s", case.name, message)
            if verbose:
                self.logger.exception("Exception during %s", case.name)
            return TestResult(name=case.name, passed=False, message=message)
