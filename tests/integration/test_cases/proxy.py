from __future__ import annotations

import base64
import socket
import threading

from harness import (
    IntegrationEnvironment,
    ProtocolSession,
    TestCase,
    TestResult,
    launch_process,
    wait_for_port,
)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class PayloadStubServer:
    def __init__(self, token: str, host: str = "127.0.0.1", port: int = 0) -> None:
        self.token = token
        self.host = host
        self.port = port
        self._shutdown = threading.Event()
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.host, self.port))
        self._sock.listen()
        self.port = int(self._sock.getsockname()[1])
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._shutdown.set()
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
        if self._thread:
            self._thread.join(timeout=2)

    def _serve(self) -> None:
        if not self._sock:
            return

        while not self._shutdown.is_set():
            try:
                client, _ = self._sock.accept()
            except OSError:
                break

            thread = threading.Thread(target=self._handle_client, args=(client,), daemon=True)
            thread.start()

    def _handle_client(self, client: socket.socket) -> None:
        with client:
            stream = client.makefile("rwb")
            stream.write(b"220 stub Server Ready EXT PROTO=2,STATS\r\n")
            stream.flush()
            authed = False

            while not self._shutdown.is_set():
                line = stream.readline()
                if not line:
                    break

                text = line.decode("utf-8", errors="replace").strip("\r\n")
                if not text:
                    continue

                parts = text.split()
                cmd = parts[0].upper()

                if cmd == "AUTH" and len(parts) >= 3 and parts[1].upper() == "TOKEN":
                    authed = parts[2] == self.token
                    response = b"200 Hello Stub\r\n" if authed else b"401 Unauthorized\r\n"
                    stream.write(response)
                    stream.flush()
                elif cmd == "CAPS":
                    stream.write(b'200 JSON ["STATS"]\r\n')
                    stream.flush()
                elif cmd == "IFACES":
                    stream.write(b'200 JSON ["\\\\Test\\\\Integration\\\\PayloadInterface"]\r\n')
                    stream.flush()
                elif cmd == "PROTO" and len(parts) >= 2:
                    stream.write(b"200 OK\r\n")
                    stream.flush()
                elif cmd == "PING":
                    stream.write(b"200 OK\r\n")
                    stream.flush()
                elif cmd == "CALL":
                    if not authed:
                        stream.write(b"401 Unauthorized\r\n")
                        stream.flush()
                        continue

                    encoding = "RAW"
                    payload = b""

                    if len(parts) >= 6 and parts[3].upper() == "PAYLOAD":
                        encoding = parts[4]
                        try:
                            payload_len = int(parts[5])
                        except ValueError:
                            payload_len = 0
                        if payload_len > 0:
                            payload = stream.read(payload_len) or b""

                    response_body = payload.upper() if payload else b"DOWNSHIFT"
                    header = f"200 PAYLOAD {encoding} {len(response_body)}\r\n".encode("utf-8")
                    stream.write(header)
                    if response_body:
                        stream.write(response_body)
                    stream.flush()
                else:
                    stream.write(b"400 Bad Request\r\n")
                    stream.flush()


def proxy_echo(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        result = session.call("EchoInterface", "echo", arg="proxy", decode_output=True)
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.decoded == "TEST_ECHO_RESPONSE:proxy", f"unexpected echo: {result.decoded}")
    return TestResult(name="proxy-echo", passed=True, traces=[result.trace])


def proxy_payload_raw(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        result = session.call(
            "PayloadInterface",
            "uppercase",
            payload=b"proxypay",
            encoding="RAW",
            decode_output=True,
        )
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
    _require(result.decoded == "RAW:PROXYPAY", f"unexpected payload response: {result.decoded}")
    return TestResult(name="proxy-payload-raw", passed=True, traces=[result.trace])


def proxy_exec_failure(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        result = session.call("ExecInterface", "fail", arg="proxyerr", decode_output=False)
    _require(result.status == 500, f"expected 500 got {result.status}")
    _require("FAIL:proxyerr (42)" in result.message, f"unexpected message: {result.message}")
    return TestResult(name="proxy-exec-failure", passed=True, traces=[result.trace])


def proxy_payload_reject(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        result = session.call(
            "PayloadInterface",
            "reject",
            payload=b"ignored",
            encoding="RAW",
            decode_output=False,
        )
    _require(result.status == 500, f"expected 500 got {result.status}")
    _require("REJECT:RAW" in result.message, f"unexpected reject message: {result.message}")
    return TestResult(name="proxy-payload-reject", passed=True, traces=[result.trace])


def proxy_multi_call(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        first = session.call("EchoInterface", "echo", arg="conn-test", decode_output=True)
        _require(first.status == 200, f"expected 200 got {first.status}")
        _require(first.decoded == "TEST_ECHO_RESPONSE:conn-test", f"unexpected echo: {first.decoded}")
        second = session.call(
            "PayloadInterface",
            "repeat",
            payload=b"abc",
            encoding="RAW",
            decode_output=True,
        )
    _require(second.status == 200, f"expected 200 got {second.status}")
    _require(second.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
    _require(second.decoded == "RAW:abc:abc", f"unexpected payload repeat: {second.decoded}")
    return TestResult(name="proxy-multi-call", passed=True, traces=[first.trace])


def proxy_inline_payload_inline(env: IntegrationEnvironment) -> TestResult:
    with env.proxy_session() as session:
        first = session.call("EchoInterface", "echo", arg="proxy-before", decode_output=True)
        _require(first.status == 200, f"expected 200 got {first.status}")
        mid = session.call(
            "PayloadInterface",
            "uppercase",
            payload=b"proxypayload",
            encoding="RAW",
            decode_output=True,
        )
        _require(mid.status == 200, f"expected 200 got {mid.status}")
        _require(mid.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
        _require(mid.decoded == "RAW:PROXYPAYLOAD", f"unexpected decoded payload: {mid.decoded}")
        last = session.call("EchoInterface", "echo", arg="proxy-after", decode_output=True)
    _require(last.status == 200, f"expected 200 got {last.status}")
    _require(
        last.decoded == "TEST_ECHO_RESPONSE:proxy-after",
        f"unexpected trailing inline response: {last.decoded}",
    )
    return TestResult(name="proxy-inline-payload-inline", passed=True, traces=[last.trace])


def proxy_payload_downshift_encoding(env: IntegrationEnvironment) -> TestResult:
    stub = PayloadStubServer(env.token)
    proxy_handle = None

    try:
        stub.start()
        proxy_port = _reserve_port()
        proxy_handle = launch_process(
            "rmi-proxy",
            [
                str(env.proxy_bin),
                "-c",
                str(env.config_path),
                "-m",
                str(env.interface_map),
                "-p",
                str(proxy_port),
                "-x",
                f"{stub.host}:{stub.port}",
                "-t",
                env.token,
            ],
            env.logger,
            env.verbose,
        )
        wait_for_port(env.proxy_host, proxy_port, 10)

        session = ProtocolSession(
            host=env.proxy_host,
            port=proxy_port,
            token=env.token,
            namespace=env.namespace,
            negotiate_proto=False,
        )
        with session:
            result = session.call(
                "PayloadInterface",
                "uppercase",
                arg="downshift-me",
                decode_output=True,
            )

        _require(result.status == 200, f"expected 200 got {result.status}")
        _require(result.message.startswith("RAW:"), f"missing encoding prefix: {result.message}")
        _require(result.decoded == "RAW:DOWNSHIFT", f"unexpected decoded payload: {result.decoded}")
        decoded_bytes = base64.b64decode(result.message.split(":", 1)[1])
        _require(decoded_bytes == b"DOWNSHIFT", f"unexpected payload bytes: {decoded_bytes!r}")
        return TestResult(name="proxy-payload-downshift-inline", passed=True, traces=[result.trace])
    finally:
        if proxy_handle:
            proxy_handle.close()
        stub.stop()


PROXY_CASES: list[TestCase] = [
    TestCase(name="proxy-payload-downshift-inline", runner=proxy_payload_downshift_encoding, requires_proxy=False),
    TestCase(name="proxy-echo", runner=proxy_echo, requires_proxy=True),
    TestCase(name="proxy-payload-raw", runner=proxy_payload_raw, requires_proxy=True),
    TestCase(name="proxy-exec-failure", runner=proxy_exec_failure, requires_proxy=True),
    TestCase(name="proxy-payload-reject", runner=proxy_payload_reject, requires_proxy=True),
    TestCase(name="proxy-multi-call", runner=proxy_multi_call, requires_proxy=True),
    TestCase(name="proxy-inline-payload-inline", runner=proxy_inline_payload_inline, requires_proxy=True),
]
