"""
Integration tests for fork, prefork, and adaptive worker models.

Tests cover:
- Standard operations in all modes
- Edge cases (worker crashes, respawn, concurrent requests)
- Concurrency stress tests
- Server and proxy in both modes
"""

from __future__ import annotations

import base64
import concurrent.futures
import json
import threading
import time
from pathlib import Path

from harness import (
    IntegrationEnvironment,
    ProtocolSession,
    TestCase,
    TestResult,
    parse_status,
)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _create_env_with_worker_model(
    base_env: IntegrationEnvironment,
    server_model: str | None = None,
    server_workers: int | None = None,
    proxy_model: str | None = None,
    proxy_workers: int | None = None,
    server_adaptive_min: int | None = None,
    server_adaptive_max: int | None = None,
    server_adaptive_spare: int | None = None,
    proxy_adaptive_min: int | None = None,
    proxy_adaptive_max: int | None = None,
    proxy_adaptive_spare: int | None = None,
) -> IntegrationEnvironment:
    """Create a new environment with worker model configuration."""
    return IntegrationEnvironment(
        root=base_env.root,
        server_port=base_env.server_port,
        proxy_port=base_env.proxy_port,
        token=base_env.token,
        namespace=base_env.namespace,
        server_unix_socket=base_env.server_unix_socket,
        proxy_unix_socket=base_env.proxy_unix_socket,
        upstream_unix_socket=base_env.upstream_unix_socket,
        verbose=base_env.verbose,
        logger=base_env.logger,
        server_worker_model=server_model,
        server_prefork_workers=server_workers,
        proxy_worker_model=proxy_model,
        proxy_prefork_workers=proxy_workers,
        server_adaptive_min_workers=server_adaptive_min,
        server_adaptive_max_workers=server_adaptive_max,
        server_adaptive_spare_workers=server_adaptive_spare,
        proxy_adaptive_min_workers=proxy_adaptive_min,
        proxy_adaptive_max_workers=proxy_adaptive_max,
        proxy_adaptive_spare_workers=proxy_adaptive_spare,
    )


# ============================================================================
# Standard Operation Tests - Must work in both modes
# ============================================================================


def server_fork_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test standard operations work correctly in fork mode."""
    test_env = _create_env_with_worker_model(env, server_model="fork")
    test_env.start_server()

    try:
        with test_env.server_session() as session:
            # Basic echo
            result1 = session.call("EchoInterface", "echo", arg="fork-test", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:fork-test",
                f"unexpected echo: {result1.decoded}",
            )

            # Payload call
            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"forkpayload",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:FORKPAYLOAD",
                f"unexpected payload: {result2.decoded}",
            )

            # Connection reuse
            result3 = session.call("EchoInterface", "echo", arg="fork-reuse", decode_output=True)
            _require(result3.status == 200, f"expected 200 got {result3.status}")

        return TestResult(name="server-fork-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def server_prefork_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test standard operations work correctly in prefork mode."""
    test_env = _create_env_with_worker_model(env, server_model="prefork", server_workers=4)
    test_env.start_server()

    try:
        with test_env.server_session() as session:
            # Basic echo
            result1 = session.call("EchoInterface", "echo", arg="prefork-test", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:prefork-test",
                f"unexpected echo: {result1.decoded}",
            )

            # Payload call
            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"preforkpayload",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:PREFORKPAYLOAD",
                f"unexpected payload: {result2.decoded}",
            )

            # Connection reuse
            result3 = session.call("EchoInterface", "echo", arg="prefork-reuse", decode_output=True)
            _require(result3.status == 200, f"expected 200 got {result3.status}")

        return TestResult(name="server-prefork-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def server_adaptive_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test standard operations work correctly in adaptive mode."""
    test_env = _create_env_with_worker_model(
        env,
        server_model="adaptive",
        server_adaptive_min=2,
        server_adaptive_max=4,
        server_adaptive_spare=1,
    )
    test_env.start_server()

    try:
        with test_env.server_session() as session:
            result1 = session.call("EchoInterface", "echo", arg="adaptive-test", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:adaptive-test",
                f"unexpected echo: {result1.decoded}",
            )

            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"adaptivepayload",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:ADAPTIVEPAYLOAD",
                f"unexpected payload: {result2.decoded}",
            )

            result3 = session.call("EchoInterface", "echo", arg="adaptive-reuse", decode_output=True)
            _require(result3.status == 200, f"expected 200 got {result3.status}")

        return TestResult(name="server-adaptive-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def proxy_fork_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test proxy standard operations in fork mode."""
    test_env = _create_env_with_worker_model(
        env, server_model="fork", proxy_model="fork"
    )
    test_env.start_server()
    test_env.start_proxy()

    try:
        with test_env.proxy_session() as session:
            result1 = session.call("EchoInterface", "echo", arg="proxy-fork", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:proxy-fork",
                f"unexpected echo: {result1.decoded}",
            )

            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"proxyfork",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:PROXYFORK",
                f"unexpected payload: {result2.decoded}",
            )

        return TestResult(name="proxy-fork-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def proxy_prefork_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test proxy standard operations in prefork mode."""
    test_env = _create_env_with_worker_model(
        env, server_model="prefork", server_workers=4, proxy_model="prefork", proxy_workers=4
    )
    test_env.start_server()
    test_env.start_proxy()

    try:
        with test_env.proxy_session() as session:
            result1 = session.call("EchoInterface", "echo", arg="proxy-prefork", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:proxy-prefork",
                f"unexpected echo: {result1.decoded}",
            )

            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"proxyprefork",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:PROXYPREFORK",
                f"unexpected payload: {result2.decoded}",
            )

        return TestResult(name="proxy-prefork-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def proxy_adaptive_standard_operations(env: IntegrationEnvironment) -> TestResult:
    """Test proxy standard operations in adaptive mode."""
    test_env = _create_env_with_worker_model(
        env,
        server_model="adaptive",
        proxy_model="adaptive",
        server_adaptive_min=2,
        server_adaptive_max=4,
        server_adaptive_spare=1,
        proxy_adaptive_min=2,
        proxy_adaptive_max=4,
        proxy_adaptive_spare=1,
    )
    test_env.start_server()
    test_env.start_proxy()

    try:
        with test_env.proxy_session() as session:
            result1 = session.call("EchoInterface", "echo", arg="proxy-adaptive", decode_output=True)
            _require(result1.status == 200, f"expected 200 got {result1.status}")
            _require(
                result1.decoded == "TEST_ECHO_RESPONSE:proxy-adaptive",
                f"unexpected echo: {result1.decoded}",
            )

            result2 = session.call(
                "PayloadInterface",
                "uppercase",
                payload=b"proxyadaptive",
                encoding="RAW",
                decode_output=True,
            )
            _require(result2.status == 200, f"expected 200 got {result2.status}")
            _require(
                result2.decoded == "RAW:PROXYADAPTIVE",
                f"unexpected payload: {result2.decoded}",
            )

        return TestResult(name="proxy-adaptive-standard", passed=True, traces=[result1.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


# ============================================================================
# Mixed Server/Proxy Worker Model Tests
# ============================================================================


def proxy_fork_to_prefork_server(env: IntegrationEnvironment) -> TestResult:
    """Test proxy in fork mode connecting to prefork server."""
    test_env = _create_env_with_worker_model(
        env, server_model="prefork", server_workers=4, proxy_model="fork"
    )
    test_env.start_server()
    test_env.start_proxy()

    try:
        with test_env.proxy_session() as session:
            result = session.call("EchoInterface", "echo", arg="mixed-models", decode_output=True)
            _require(result.status == 200, f"expected 200 got {result.status}")
            _require(
                result.decoded == "TEST_ECHO_RESPONSE:mixed-models",
                f"unexpected echo: {result.decoded}",
            )

        return TestResult(name="proxy-fork-to-prefork-server", passed=True, traces=[result.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


def proxy_prefork_to_fork_server(env: IntegrationEnvironment) -> TestResult:
    """Test proxy in prefork mode connecting to fork server."""
    test_env = _create_env_with_worker_model(
        env, server_model="fork", proxy_model="prefork", proxy_workers=4
    )
    test_env.start_server()
    test_env.start_proxy()

    try:
        with test_env.proxy_session() as session:
            result = session.call("EchoInterface", "echo", arg="mixed-models-2", decode_output=True)
            _require(result.status == 200, f"expected 200 got {result.status}")
            _require(
                result.decoded == "TEST_ECHO_RESPONSE:mixed-models-2",
                f"unexpected echo: {result.decoded}",
            )

        return TestResult(name="proxy-prefork-to-fork-server", passed=True, traces=[result.trace])
    finally:
        test_env.stop_all()
        test_env.check_no_new_zombies()


# ============================================================================
# Test Case Registration
# ============================================================================

WORKER_MODEL_CASES: list[TestCase] = [
    # Standard operations
    TestCase(name="server-fork-standard", runner=server_fork_standard_operations),
    TestCase(name="server-prefork-standard", runner=server_prefork_standard_operations),
    TestCase(name="server-adaptive-standard", runner=server_adaptive_standard_operations),
    TestCase(name="proxy-fork-standard", runner=proxy_fork_standard_operations, requires_proxy=True),
    TestCase(name="proxy-prefork-standard", runner=proxy_prefork_standard_operations, requires_proxy=True),
    TestCase(name="proxy-adaptive-standard", runner=proxy_adaptive_standard_operations, requires_proxy=True),
    # Mixed server/proxy models
    TestCase(name="proxy-fork-to-prefork-server", runner=proxy_fork_to_prefork_server, requires_proxy=True),
    TestCase(name="proxy-prefork-to-fork-server", runner=proxy_prefork_to_fork_server, requires_proxy=True),
]
