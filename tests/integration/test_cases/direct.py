from __future__ import annotations

import json
from pathlib import Path

from harness import IntegrationEnvironment, TestCase, TestResult, parse_status


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def direct_echo(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        result = session.call("EchoInterface", "echo", arg="integration", decode_output=True)
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.decoded == "TEST_ECHO_RESPONSE:integration", f"unexpected echo: {result.decoded}")
    return TestResult(name="direct-echo", passed=True, traces=[result.trace])


def exec_failure(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        result = session.call("ExecInterface", "fail", arg="fatal", decode_output=False)
    _require(result.status == 500, f"expected 500 got {result.status}")
    _require("FAIL:fatal (42)" in result.message, f"unexpected message: {result.message}")
    return TestResult(name="exec-failure", passed=True, traces=[result.trace])


def exec_stderr(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        result = session.call("ExecInterface", "stderr", arg="loud", decode_output=True)
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.decoded == "STDERR:loud", f"unexpected stderr content: {result.decoded}")
    return TestResult(name="exec-stderr", passed=True, traces=[result.trace])


def exec_large(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        result = session.call("ExecInterface", "large", arg="20480", decode_output=True)
    _require(len(result.decoded) == 20480, f"unexpected length {len(result.decoded)}")
    _require(result.decoded.startswith("L"), "large response missing prefix")
    return TestResult(name="exec-large", passed=True, traces=[result.trace])


def posix_acl_args(env: IntegrationEnvironment) -> TestResult:
    path = "/tmp/rmi-arg-test"
    args = "'www' 'x' '" + path + "' ''"
    with env.server_session() as session:
        result = session.call(
            "PosixAccessServiceInterface",
            "setPosixGroupAccessMode",
            arg=args,
            decode_output=True,
        )
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(
        result.decoded == f"GROUP:www:x:{path}:",
        f"unexpected argument mapping: {result.decoded}",
    )
    return TestResult(name="posix-acl-args", passed=True, traces=[result.trace])


def payload_uppercase(env: IntegrationEnvironment) -> TestResult:
    payload = b"mixedCase"
    with env.server_session() as session:
        result = session.call(
            "PayloadInterface",
            "uppercase",
            payload=payload,
            encoding="JSON",
            decode_output=True,
        )
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
    _require(result.decoded == "JSON:MIXEDCASE", f"unexpected payload content: {result.decoded}")
    return TestResult(name="payload-uppercase", passed=True, traces=[result.trace])


def payload_reject(env: IntegrationEnvironment) -> TestResult:
    payload = b"ignored"
    with env.server_session() as session:
        result = session.call(
            "PayloadInterface",
            "reject",
            payload=payload,
            encoding="RAW",
            decode_output=False,
        )
    _require(result.status == 500, f"expected 500 got {result.status}")
    _require("REJECT:RAW" in result.message, f"unexpected reject message: {result.message}")
    return TestResult(name="payload-reject", passed=True, traces=[result.trace])


def connection_reuse(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        first = session.call("EchoInterface", "echo", arg="reused", decode_output=True)
        _require(first.status == 200, f"expected 200 got {first.status}")
        _require(first.decoded == "TEST_ECHO_RESPONSE:reused", f"unexpected echo: {first.decoded}")
        second = session.call(
            "PayloadInterface",
            "repeat",
            payload=b"combo",
            encoding="RAW",
            decode_output=True,
        )
    _require(second.status == 200, f"expected 200 got {second.status}")
    _require(second.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
    _require(second.decoded == "RAW:combo:combo", f"unexpected combo payload: {second.decoded}")
    return TestResult(name="connection-reuse", passed=True, traces=[first.trace])


def inline_payload_inline(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        inline_before = session.call("EchoInterface", "echo", arg="before", decode_output=True)
        _require(inline_before.status == 200, f"expected 200 got {inline_before.status}")
        _require(
            inline_before.decoded == "TEST_ECHO_RESPONSE:before",
            f"unexpected inline response: {inline_before.decoded}",
        )
        payload = session.call(
            "PayloadInterface",
            "uppercase",
            payload=b"followup",
            encoding="RAW",
            decode_output=True,
        )
        _require(payload.status == 200, f"expected 200 got {payload.status}")
        _require(payload.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
        _require(payload.decoded == "RAW:FOLLOWUP", f"unexpected payload decoding: {payload.decoded}")
        inline_after = session.call("EchoInterface", "echo", arg="after", decode_output=True)
    _require(inline_after.status == 200, f"expected 200 got {inline_after.status}")
    _require(
        inline_after.decoded == "TEST_ECHO_RESPONSE:after",
        f"unexpected trailing inline response: {inline_after.decoded}",
    )
    return TestResult(name="inline-payload-inline", passed=True, traces=[inline_after.trace])


def inline_payload_large_inline(env: IntegrationEnvironment) -> TestResult:
    large = b"x" * 12000
    with env.server_session() as session:
        first = session.call("EchoInterface", "echo", arg="start-large", decode_output=True)
        _require(first.status == 200, f"expected 200 got {first.status}")
        big_payload = session.call(
            "PayloadInterface",
            "repeat",
            payload=large,
            encoding="RAW",
            decode_output=False,
        )
        expected = b"RAW:" + large + b":" + large
        _require(big_payload.status == 200, f"expected 200 got {big_payload.status}")
        _require(big_payload.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
        _require(
            big_payload.payload == expected,
            f"unexpected payload body length {len(big_payload.payload)} expected {len(expected)}",
        )
        final = session.call("EchoInterface", "echo", arg="end-large", decode_output=True)
    _require(final.status == 200, f"expected 200 got {final.status}")
    _require(
        final.decoded == "TEST_ECHO_RESPONSE:end-large",
        f"unexpected final inline response: {final.decoded}",
    )
    return TestResult(name="inline-payload-large-inline", passed=True, traces=[final.trace])

def control_commands(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        session._send_line("TIME")
        time_status, time_message = parse_status(session._read_line())
        _require(time_status == 200, f"expected 200 got {time_status}")
        _require(len(time_message) > 0, "TIME response should include a timestamp payload")

        session._send_line("NOOP")
        noop_status, noop_message = parse_status(session._read_line())
        _require(noop_status == 200, f"expected 200 got {noop_status}")
        _require(noop_message == "OK", f"unexpected NOOP response: {noop_message}")

        trace = session.trace

    return TestResult(name="control-commands", passed=True, traces=[trace])


def server_stats_json(env: IntegrationEnvironment) -> TestResult:
    with env.server_session() as session:
        session._send_line("SERVER stats")
        status, message = parse_status(session._read_line())

    _require(status == 200, f"expected 200 got {status}")
    try:
        payload = json.loads(message)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"invalid JSON payload: {exc.msg}") from exc

    _require(isinstance(payload.get("workers"), list), "workers must be a list")
    _require(len(payload["workers"]) <= 25, "workers list exceeded cap")

    listed = payload.get("listed")
    if isinstance(listed, int):
        _require(listed == len(payload["workers"]), "listed count mismatch")

    _require("reuse_enabled" in payload, "reuse_enabled flag missing")
    _require("trimmed" in payload, "trimmed flag missing")
    _require("total_spare" in payload, "total_spare missing")

    return TestResult(name="server-stats-json", passed=True)


def debug_traffic_logging(env: IntegrationEnvironment) -> TestResult:
    log_path = Path("/tmp/rmi-server.log")
    try:
        log_path.unlink()
    except FileNotFoundError:
        pass

    payload = b"traffic-check"
    with env.server_session() as session:
        session._send_line("DEBUG traffic on")
        status, message = parse_status(session._read_line())
        _require(status == 200, f"expected 200 got {status} ({message})")
        result = session.call(
            "PayloadInterface",
            "repeat",
            payload=payload,
            encoding="RAW",
            decode_output=True,
        )
    _require(result.status == 200, f"expected 200 got {result.status}")
    _require(result.message.startswith("PAYLOAD "), "payload response missing PAYLOAD keyword")
    _require(result.decoded == "RAW:traffic-check:traffic-check", f"unexpected payload content: {result.decoded}")
    _require(log_path.exists(), "traffic log file missing")
    contents = log_path.read_text(encoding="utf-8", errors="replace")
    _require("traffic:" in contents, "expected traffic log entry")
    _require("traffic-check" not in contents, "payload bytes should not be logged verbatim")
    return TestResult(name="debug-traffic-logging", passed=True, traces=[result.trace])


DIRECT_CASES: list[TestCase] = [
    TestCase(name="direct-echo", runner=direct_echo),
    TestCase(name="exec-failure", runner=exec_failure),
    TestCase(name="exec-stderr", runner=exec_stderr),
    TestCase(name="exec-large", runner=exec_large),
    TestCase(name="posix-acl-args", runner=posix_acl_args),
    TestCase(name="payload-uppercase", runner=payload_uppercase),
    TestCase(name="payload-reject", runner=payload_reject),
    TestCase(name="connection-reuse", runner=connection_reuse),
    TestCase(name="inline-payload-inline", runner=inline_payload_inline),
    TestCase(name="inline-payload-large-inline", runner=inline_payload_large_inline),
    TestCase(name="control-commands", runner=control_commands),
    TestCase(name="server-stats-json", runner=server_stats_json),
    TestCase(name="debug-traffic-logging", runner=debug_traffic_logging),
]
