Cloud-RMI Integration Tests
==========================

The integration suite is driven by `run_tests.py`, a Python harness that
starts the test server and proxy, executes a set of protocol checks, and
prints concise status lines by default:

```
testing direct-echo... ok
testing exec-failure... ok
...
```

Usage
-----

* Default run (used by `make test`):

  ```
  python tests/integration/run_tests.py
  ```

  The script automatically creates a virtual environment in
  `tests/integration/.venv` and re-executes itself inside it for consistent
  dependencies.

* Verbose mode with protocol transcripts:

  ```
  python tests/integration/run_tests.py --verbose
  ```

  Use `-vv` for even more debug logging (including server/proxy stdout if
  available).

* Run a subset:

  ```
  python tests/integration/run_tests.py --filter proxy
  ```

Ports, tokens, and namespaces can be overridden via flags (see `--help`).
Cleanup is automatic and the harness checks for zombie processes to protect
the environment between runs.
