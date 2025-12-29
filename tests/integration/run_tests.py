#!/usr/bin/env python3
from __future__ import annotations

import argparse
import logging
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from harness import IntegrationEnvironment, IntegrationRunner
from test_cases import DIRECT_CASES, PROXY_CASES, UNIX_CASES, WORKER_MODEL_CASES


@dataclass
class SuitePlan:
    name: str
    env: IntegrationEnvironment
    cases: list
    name_prefix: str | None = None


@dataclass
class SuiteResult:
    name: str
    results: list


def bootstrap_venv(venv_path: Path) -> None:
    desired = venv_path.resolve()
    active = os.environ.get("VIRTUAL_ENV")
    if active and Path(active).resolve() == desired:
        return

    if not desired.exists():
        print(f"creating virtualenv at {desired}")
        subprocess.check_call([sys.executable, "-m", "venv", str(desired)])

    python_bin = desired / ("Scripts" if os.name == "nt" else "bin") / ("python.exe" if os.name == "nt" else "python")
    args = [str(python_bin), str(Path(__file__).resolve()), "--no-venv"]
    args.extend(arg for arg in sys.argv[1:] if arg != "--no-venv")
    os.execv(str(python_bin), args)


def parse_args() -> argparse.Namespace:
    bootstrap = argparse.ArgumentParser(add_help=False)
    bootstrap.add_argument("--venv-path", default=str(Path(__file__).parent / ".venv"))
    bootstrap.add_argument("--no-venv", action="store_true")
    pre_args, remaining = bootstrap.parse_known_args()

    if not pre_args.no_venv:
        bootstrap_venv(Path(pre_args.venv_path))

    parser = argparse.ArgumentParser(description="Run Cloud-RMI integration tests.")
    parser.add_argument("-v", "--verbose", action="count", default=0, help="Show protocol transcripts.")
    parser.add_argument("--venv-path", default=pre_args.venv_path, help=argparse.SUPPRESS)
    parser.add_argument("--no-venv", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--filter", help="Run only tests containing this substring.")
    parser.add_argument("--server-port", type=int, default=19950, help="Port for the RMI server.")
    parser.add_argument("--proxy-port", type=int, default=19951, help="Port for the RMI proxy.")
    parser.add_argument("--token", default="testtoken", help="Auth token for test calls.")
    parser.add_argument("--namespace", default="\\Test\\Integration", help="Base namespace for test interfaces.")
    parser.add_argument("--skip-unix", action="store_true", help="Skip unix socket coverage.")
    parser.add_argument(
        "--unix-server-socket",
        default="/tmp/cloud-rmi-integration-server.sock",
        help="Unix socket path for the test server.",
    )
    parser.add_argument(
        "--unix-proxy-socket",
        default="/tmp/cloud-rmi-integration-proxy.sock",
        help="Unix socket path for the test proxy.",
    )
    parser.add_argument(
        "--unix-upstream-socket",
        help="Unix socket path for the proxy upstream. Defaults to the server socket.",
    )
    return parser.parse_args(remaining)


def prepare_unix_paths(args: argparse.Namespace, suffix: str) -> tuple[str, str, str]:
    server_socket = Path(args.unix_server_socket)
    proxy_socket = Path(args.unix_proxy_socket)
    upstream_socket = Path(args.unix_upstream_socket) if args.unix_upstream_socket else server_socket

    def add_suffix(path: Path) -> Path:
        return path.parent / f"{path.stem}-{suffix}{path.suffix}"

    server_socket = add_suffix(server_socket)
    proxy_socket = add_suffix(proxy_socket)
    upstream_socket = add_suffix(upstream_socket)

    for sock in (server_socket, proxy_socket, upstream_socket):
        sock.parent.mkdir(parents=True, exist_ok=True)
        try:
            sock.unlink()
        except FileNotFoundError:
            pass

    return str(server_socket), str(proxy_socket), str(upstream_socket)


def main() -> int:
    args = parse_args()

    log_level = logging.WARNING
    if args.verbose >= 2:
        log_level = logging.DEBUG
    elif args.verbose == 1:
        log_level = logging.INFO
    logging.basicConfig(level=log_level, format="%(asctime)s %(levelname)s %(message)s")

    root = Path(__file__).resolve().parents[2]
    suites: list[SuitePlan] = []

    base_cases = DIRECT_CASES + PROXY_CASES
    if args.filter:
        base_cases = [case for case in base_cases if args.filter in case.name]

    unix_cases = UNIX_CASES
    if args.filter:
        unix_cases = [case for case in unix_cases if args.filter in case.name]

    all_cases = base_cases + ([] if args.skip_unix else unix_cases)

    def add_suite(name: str, env: IntegrationEnvironment, cases: list, prefix: str | None = None) -> None:
        if cases:
            suites.append(SuitePlan(name=name, env=env, cases=cases, name_prefix=prefix))

    # Default run with the configured worker model (from binary defaults)
    default_server_socket = default_proxy_socket = default_upstream_socket = None
    if not args.skip_unix and unix_cases:
        default_server_socket, default_proxy_socket, default_upstream_socket = prepare_unix_paths(args, "default")

    add_suite(
        "default",
        IntegrationEnvironment(
            root=root,
            server_port=args.server_port,
            proxy_port=args.proxy_port,
            token=args.token,
            namespace=args.namespace,
            server_unix_socket=default_server_socket,
            proxy_unix_socket=default_proxy_socket,
            upstream_unix_socket=default_upstream_socket,
            verbose=args.verbose > 0,
        ),
        all_cases,
    )

    # Run the full suite for each worker model variant
    worker_variants = [
        ("fork", {"server_worker_model": "fork", "proxy_worker_model": "fork"}),
        (
            "prefork",
            {
                "server_worker_model": "prefork",
                "proxy_worker_model": "prefork",
                "server_prefork_workers": 2,
                "proxy_prefork_workers": 2,
            },
        ),
        (
            "adaptive",
            {
                "server_worker_model": "adaptive",
                "proxy_worker_model": "adaptive",
                "server_adaptive_min_workers": 1,
                "server_adaptive_max_workers": 4,
                "server_adaptive_spare_workers": 1,
                "proxy_adaptive_min_workers": 1,
                "proxy_adaptive_max_workers": 4,
                "proxy_adaptive_spare_workers": 1,
            },
        ),
    ]

    for variant_name, variant_overrides in worker_variants:
        if not all_cases:
            continue

        server_socket = proxy_socket = upstream_socket = None
        if not args.skip_unix and unix_cases:
            server_socket, proxy_socket, upstream_socket = prepare_unix_paths(args, variant_name)

        add_suite(
            f"{variant_name}-worker-model",
            IntegrationEnvironment(
                root=root,
                server_port=args.server_port,
                proxy_port=args.proxy_port,
                token=args.token,
                namespace=args.namespace,
                server_unix_socket=server_socket,
                proxy_unix_socket=proxy_socket,
                upstream_unix_socket=upstream_socket,
                verbose=args.verbose > 0,
                **variant_overrides,
            ),
            all_cases,
            prefix=variant_name,
        )

    # Worker model tests (fork vs prefork)
    worker_model_cases = WORKER_MODEL_CASES
    if args.filter:
        worker_model_cases = [case for case in worker_model_cases if args.filter in case.name]

    add_suite(
        "worker-model-specific",
        IntegrationEnvironment(
            root=root,
            server_port=args.server_port,
            proxy_port=args.proxy_port,
            token=args.token,
            namespace=args.namespace,
            verbose=args.verbose > 0,
        ),
        worker_model_cases,
    )

    if not suites:
        print("No tests matched the provided filter.")
        return 1

    results: list = []
    suite_results: list[SuiteResult] = []
    for suite in suites:
        runner = IntegrationRunner(suite.env, name_prefix=suite.name_prefix)
        try:
            suite_run = runner.run(suite.cases, verbose=args.verbose > 0)
            for result in suite_run:
                result.suite = suite.name
            suite_results.append(SuiteResult(name=suite.name, results=suite_run))
            results.extend(suite_run)
        except Exception as exc:  # pragma: no cover - integration only
            logging.exception("Integration harness failed")
            print(f"Integration harness failed: {exc}")
            return 1

    if suite_results:
        print("\nTiming summary:")
        for summary in suite_results:
            total = sum(result.duration for result in summary.results)
            print(f"- {summary.name}: {total:.3f}s total")
            for result in summary.results:
                print(f"  - {result.name}: {result.duration:.3f}s")

    failures = [result for result in results if not result.passed]
    if failures:
        print("\nFailures:")
        for failure in failures:
            suite_name = getattr(failure, "suite", "") or "unknown"
            print(f"- {failure.name} [{suite_name}]: {failure.message or 'see logs'}")
        return 1

    print("integration tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
