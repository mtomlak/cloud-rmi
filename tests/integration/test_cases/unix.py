from __future__ import annotations

from harness import IntegrationEnvironment, TestCase, TestResult
from test_cases.direct import direct_echo, payload_uppercase
from test_cases.proxy import proxy_echo, proxy_payload_raw


def _wrap(case_name: str, func, requires_proxy: bool = False) -> TestCase:
    def runner(env: IntegrationEnvironment) -> TestResult:
        result = func(env)
        result.name = case_name
        return result

    return TestCase(name=case_name, runner=runner, requires_proxy=requires_proxy)


UNIX_CASES: list[TestCase] = [
    _wrap("unix-direct-echo", direct_echo),
    _wrap("unix-payload-uppercase", payload_uppercase),
    _wrap("unix-proxy-echo", proxy_echo, requires_proxy=True),
    _wrap("unix-proxy-payload-raw", proxy_payload_raw, requires_proxy=True),
]
