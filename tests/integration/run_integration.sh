#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="$ROOT_DIR/rmi-server"
CLIENT_BIN="$ROOT_DIR/rmi-client"
CONFIG="$ROOT_DIR/tests/integration/fixtures/rmi-server.conf"
INTERFACE_MAP="$ROOT_DIR/tests/integration/fixtures/interfaces.map"
EXPECTED="$ROOT_DIR/tests/integration/expected/test_echo.out"
TEST_PORT=9950
TEMP_OUTPUT=""
SERVER_PID=""

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$TEMP_OUTPUT" ] && [ -f "$TEMP_OUTPUT" ]; then
    rm -f "$TEMP_OUTPUT"
  fi
}
trap cleanup EXIT

cd "$ROOT_DIR"
"$SERVER_BIN" -c "$CONFIG" -m "$INTERFACE_MAP" -p "$TEST_PORT" -e "tests/integration/fixtures/scripts" -b "Test\\Integration" -t "testtoken" >/dev/null 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 50); do
  if bash -c "exec 3<>/dev/tcp/127.0.0.1/$TEST_PORT" 2>/dev/null; then
    exec 3<&-
    exec 3>&-
    READY=1
    break
  fi
  sleep 0.1
done

if [ "$READY" -ne 1 ]; then
  echo "Server failed to start on port $TEST_PORT" >&2
  exit 1
fi

TEMP_OUTPUT="$(mktemp)"
"$CLIENT_BIN" -c "$CONFIG" -t "testtoken" EchoInterface echo "integration" > "$TEMP_OUTPUT"

if ! diff -u "$EXPECTED" "$TEMP_OUTPUT"; then
  echo "Integration test failed: output mismatch" >&2
  exit 1
fi

echo "integration:test_echo passed"
