from __future__ import annotations

from harness import IntegrationEnvironment, TestCase, TestResult


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


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


PROXY_CASES: list[TestCase] = [
    TestCase(name="proxy-echo", runner=proxy_echo, requires_proxy=True),
    TestCase(name="proxy-payload-raw", runner=proxy_payload_raw, requires_proxy=True),
    TestCase(name="proxy-exec-failure", runner=proxy_exec_failure, requires_proxy=True),
    TestCase(name="proxy-payload-reject", runner=proxy_payload_reject, requires_proxy=True),
    TestCase(name="proxy-multi-call", runner=proxy_multi_call, requires_proxy=True),
    TestCase(name="proxy-inline-payload-inline", runner=proxy_inline_payload_inline, requires_proxy=True),
]
