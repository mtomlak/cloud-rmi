#!/usr/bin/env python3
from __future__ import annotations

import argparse
import logging
import os
import subprocess
import sys
from pathlib import Path

from harness import IntegrationEnvironment, IntegrationRunner
from test_cases import DIRECT_CASES, PROXY_CASES, UNIX_CASES, WORKER_MODEL_CASES


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


def main() -> int:
    args = parse_args()

    log_level = logging.WARNING
    if args.verbose >= 2:
        log_level = logging.DEBUG
    elif args.verbose == 1:
        log_level = logging.INFO
    logging.basicConfig(level=log_level, format="%(asctime)s %(levelname)s %(message)s")

    root = Path(__file__).resolve().parents[2]
    suites: list[tuple[IntegrationEnvironment, list]] = []

    base_cases = DIRECT_CASES + PROXY_CASES
    if args.filter:
        base_cases = [case for case in base_cases if args.filter in case.name]

    if base_cases:
        suites.append(
            (
                IntegrationEnvironment(
                    root=root,
                    server_port=args.server_port,
                    proxy_port=args.proxy_port,
                    token=args.token,
                    namespace=args.namespace,
                    verbose=args.verbose > 0,
                ),
                base_cases,
            )
        )

    unix_cases = UNIX_CASES
    if args.filter:
        unix_cases = [case for case in unix_cases if args.filter in case.name]

    if not args.skip_unix and unix_cases:
        server_socket = Path(args.unix_server_socket)
        proxy_socket = Path(args.unix_proxy_socket)
        upstream_socket = Path(args.unix_upstream_socket) if args.unix_upstream_socket else server_socket

        for sock in (server_socket, proxy_socket, upstream_socket):
            sock.parent.mkdir(parents=True, exist_ok=True)
            try:
                sock.unlink()
            except FileNotFoundError:
                pass

        suites.append(
            (
                IntegrationEnvironment(
                    root=root,
                    server_port=args.server_port,
                    proxy_port=args.proxy_port,
                    token=args.token,
                    namespace=args.namespace,
                    server_unix_socket=str(server_socket),
                    proxy_unix_socket=str(proxy_socket),
                    upstream_unix_socket=str(upstream_socket),
                    verbose=args.verbose > 0,
                ),
                unix_cases,
            )
        )

    # Worker model tests (fork vs prefork)
    worker_model_cases = WORKER_MODEL_CASES
    if args.filter:
        worker_model_cases = [case for case in worker_model_cases if args.filter in case.name]

    if worker_model_cases:
        suites.append(
            (
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
        )

    if not suites:
        print("No tests matched the provided filter.")
        return 1

    results = []
    for env, cases in suites:
        runner = IntegrationRunner(env)
        try:
            results.extend(runner.run(cases, verbose=args.verbose > 0))
        except Exception as exc:  # pragma: no cover - integration only
            logging.exception("Integration harness failed")
            print(f"Integration harness failed: {exc}")
            return 1

    failures = [result for result in results if not result.passed]
    if failures:
        print("\nFailures:")
        for failure in failures:
            print(f"- {failure.name}: {failure.message or 'see logs'}")
        return 1

    print("integration tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
