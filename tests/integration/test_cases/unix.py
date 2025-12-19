from __future__ import annotations

import socket

from harness import IntegrationEnvironment, TestCase, TestResult, WireTrace, parse_status
from test_cases.direct import direct_echo, payload_uppercase
from test_cases.proxy import proxy_echo, proxy_payload_raw


def _wrap(case_name: str, func, requires_proxy: bool = False) -> TestCase:
    def runner(env: IntegrationEnvironment) -> TestResult:
        result = func(env)
        result.name = case_name
        return result

    return TestCase(name=case_name, runner=runner, requires_proxy=requires_proxy)


def _read_line(sock: socket.socket) -> str:
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data.extend(chunk)
    return data.decode("utf-8", errors="replace").rstrip("\n")


def _read_exact(sock: socket.socket, length: int) -> bytes:
    remaining = length
    payload = bytearray()
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            break
        payload.extend(chunk)
        remaining -= len(chunk)
    return bytes(payload)


def _send_line(sock: socket.socket, line: str) -> None:
    sock.sendall(f"{line}\n".encode("utf-8"))


def unix_skip_auth_payload(env: IntegrationEnvironment) -> TestResult:
    trace = WireTrace()
    payload = b"mixedCase"
    iface = f"{env.namespace}\\PayloadInterface"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect(env.server_unix_socket)
        banner = _read_line(sock)
        trace.record("recv", banner)

        header = f"CALL {iface} uppercase PAYLOAD JSON {len(payload)}"
        _send_line(sock, header)
        trace.record("send", header)
        sock.sendall(payload)
        trace.extend("send", "payload-bytes", payload)

        response = _read_line(sock)
        trace.record("recv", response)
        status, message = parse_status(response)
        if status != 200:
            return TestResult(
                name="unix-skip-auth-payload",
                passed=False,
                message=f"expected 200 OK, got {status} {message}",
                traces=[trace],
            )

        parts = message.split(" ")
        if len(parts) < 3 or parts[0] != "PAYLOAD":
            return TestResult(
                name="unix-skip-auth-payload",
                passed=False,
                message=f"expected PAYLOAD response, got {message}",
                traces=[trace],
            )

        try:
            payload_len = int(parts[2])
        except ValueError:
            return TestResult(
                name="unix-skip-auth-payload",
                passed=False,
                message=f"invalid payload length in response: {message}",
                traces=[trace],
            )

        payload_resp = _read_exact(sock, payload_len)
        trace.extend("recv", "payload-bytes", payload_resp)
        if payload_resp != b"JSON:MIXEDCASE":
            return TestResult(
                name="unix-skip-auth-payload",
                passed=False,
                message=f"unexpected payload content: {payload_resp!r}",
                traces=[trace],
            )

    return TestResult(name="unix-skip-auth-payload", passed=True, traces=[trace])


def unix_reject_proto_downgrade(env: IntegrationEnvironment) -> TestResult:
    trace = WireTrace()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect(env.server_unix_socket)
        banner = _read_line(sock)
        trace.record("recv", banner)

        _send_line(sock, "PROTO 1")
        trace.record("send", "PROTO 1")

        response = _read_line(sock)
        trace.record("recv", response)
        status, _ = parse_status(response)
        if status != 400:
            return TestResult(
                name="unix-proto-downgrade-rejected",
                passed=False,
                message=f"expected PROTO 1 rejection, got {status}",
                traces=[trace],
            )

        try:
            sock.settimeout(0.5)
            extra = sock.recv(1)
            if extra:
                return TestResult(
                    name="unix-proto-downgrade-rejected",
                    passed=False,
                    message="connection remained open after PROTO 1",
                    traces=[trace],
                )
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            pass

    return TestResult(name="unix-proto-downgrade-rejected", passed=True, traces=[trace])


UNIX_CASES: list[TestCase] = [
    _wrap("unix-direct-echo", direct_echo),
    _wrap("unix-payload-uppercase", payload_uppercase),
    _wrap("unix-proxy-echo", proxy_echo, requires_proxy=True),
    _wrap("unix-proxy-payload-raw", proxy_payload_raw, requires_proxy=True),
    TestCase(name="unix-skip-auth-payload", runner=unix_skip_auth_payload),
    TestCase(name="unix-proto-downgrade-rejected", runner=unix_reject_proto_downgrade),
]
