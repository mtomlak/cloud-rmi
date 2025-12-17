from __future__ import annotations

from harness import IntegrationEnvironment, TestCase, TestResult


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
    _require(second.decoded == "RAW:combo:combo", f"unexpected combo payload: {second.decoded}")
    return TestResult(name="connection-reuse", passed=True, traces=[first.trace])


DIRECT_CASES: list[TestCase] = [
    TestCase(name="direct-echo", runner=direct_echo),
    TestCase(name="exec-failure", runner=exec_failure),
    TestCase(name="exec-stderr", runner=exec_stderr),
    TestCase(name="exec-large", runner=exec_large),
    TestCase(name="payload-uppercase", runner=payload_uppercase),
    TestCase(name="payload-reject", runner=payload_reject),
    TestCase(name="connection-reuse", runner=connection_reuse),
]
