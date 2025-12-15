#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="$ROOT_DIR/rmi-server"
PROXY_BIN="$ROOT_DIR/rmi-proxy"
CONFIG="$ROOT_DIR/tests/integration/fixtures/rmi-server.conf"
INTERFACE_MAP="$ROOT_DIR/tests/integration/fixtures/interfaces.map"
PROTOCOL_CLIENT="$ROOT_DIR/tests/integration/protocol_client.py"
TEST_PORT=19950
PROXY_PORT=19951
TOKEN="testtoken"
NAMESPACE="Test\\Integration"
SERVER_PID=""
PROXY_PID=""
BASE_ZOMBIES=$(ps -eo pid,state,comm | awk '$2=="Z" && ($3=="rmi-server" || $3=="rmi-proxy") {print $1}')

cleanup() {
  for pid in "$PROXY_PID" "$SERVER_PID"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

check_no_zombies() {
  # Ensure no defunct rmi-server or rmi-proxy children are left behind.
  local new_zombies=()

  while read -r pid state comm; do
    skip=false
    for base in $BASE_ZOMBIES; do
      if [ "$pid" = "$base" ]; then
        skip=true
        break
      fi
    done
    if [ "$skip" = false ]; then
      new_zombies+=("$pid $state $comm")
    fi
  done < <(ps -eo pid,state,comm | awk '$2=="Z" && ($3=="rmi-server" || $3=="rmi-proxy")')

  if [ ${#new_zombies[@]} -ne 0 ]; then
    echo "Detected zombie child processes" >&2
    for entry in "${new_zombies[@]}"; do
      echo "$entry"
    done
    exit 1
  fi
}

wait_for_port() {
  local host="$1" port="$2" ready=0
  for _ in $(seq 1 100); do
    if timeout 1 bash -c "exec 3<>/dev/tcp/${host}/${port}" 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      ready=1
      break
    fi
    sleep 0.1
  done
  if [ "$ready" -ne 1 ]; then
    echo "Port ${port} on ${host} did not open" >&2
    exit 1
  fi
}

start_server() {
  "$SERVER_BIN" -c "$CONFIG" -m "$INTERFACE_MAP" -p "$TEST_PORT" -e "$ROOT_DIR/tests/integration/fixtures/scripts" -b "Test\\Integration" -t "$TOKEN" >/dev/null 2>&1 &
  SERVER_PID=$!
  wait_for_port 127.0.0.1 "$TEST_PORT"
}

start_proxy() {
  "$PROXY_BIN" -c "$CONFIG" -m "$INTERFACE_MAP" -p "$PROXY_PORT" -x "127.0.0.1:$TEST_PORT" -t "$TOKEN" >/dev/null 2>&1 &
  PROXY_PID=$!
  wait_for_port 127.0.0.1 "$PROXY_PORT"
}

run_call() {
  local port="$1" iface="$2" method="$3" arg="$4" payload="$5" encoding="$6" decode="$7"
  shift || true
  local cmd=("$PROTOCOL_CLIENT" --host 127.0.0.1 --port "$port" --token "$TOKEN" --iface "$iface" --method "$method" --namespace "$NAMESPACE")
  if [ -n "$payload" ]; then
    cmd+=(--payload "$payload" --encoding "$encoding")
  else
    cmd+=(--arg "$arg")
  fi
  if [ "$decode" = "yes" ]; then
    cmd+=(--decode-output)
  fi
  python3 "${cmd[@]}"
}

get_field() {
  echo "$1" | sed -n "s/^$2://p"
}

start_server

# Direct server tests
result=$(run_call "$TEST_PORT" "EchoInterface" "echo" "integration" "" "" "yes")
status=$(get_field "$result" status)
decoded=$(get_field "$result" decoded)
if [ "$status" != "200" ] || [ "$decoded" != "TEST_ECHO_RESPONSE:integration" ]; then
  echo "Echo test failed" >&2
  exit 1
fi

result=$(run_call "$TEST_PORT" "ExecInterface" "fail" "fatal" "" "" "no")
status=$(get_field "$result" status)
message=$(get_field "$result" message)
if [ "$status" != "500" ] || [[ "$message" != *"FAIL:fatal (42)"* ]]; then
  echo "Non-zero exit handling failed" >&2
  exit 1
fi

result=$(run_call "$TEST_PORT" "ExecInterface" "stderr" "loud" "" "" "yes")
status=$(get_field "$result" status)
decoded=$(get_field "$result" decoded)
if [ "$status" != "200" ] || [ "$decoded" != "STDERR:loud" ]; then
  echo "Stderr handling failed" >&2
  exit 1
fi

result=$(run_call "$TEST_PORT" "ExecInterface" "large" "20480" "" "" "yes")
decoded=$(get_field "$result" decoded)
if [ "${#decoded}" -ne 20480 ] || [ "${decoded:0:1}" != "L" ]; then
  echo "Large output handling failed" >&2
  exit 1
fi

result=$(run_call "$TEST_PORT" "PayloadInterface" "uppercase" "" "mixedCase" "JSON" "yes")
status=$(get_field "$result" status)
decoded=$(get_field "$result" decoded)
if [ "$status" != "200" ] || [ "$decoded" != "JSON:MIXEDCASE" ]; then
  echo "Payload uppercase test failed" >&2
  exit 1
fi

check_no_zombies

# Proxy tests
start_proxy

result=$(run_call "$PROXY_PORT" "EchoInterface" "echo" "proxy" "" "" "yes")
status=$(get_field "$result" status)
decoded=$(get_field "$result" decoded)
if [ "$status" != "200" ] || [ "$decoded" != "TEST_ECHO_RESPONSE:proxy" ]; then
  echo "Proxy legacy CALL failed" >&2
  exit 1
fi

result=$(run_call "$PROXY_PORT" "PayloadInterface" "uppercase" "" "proxypay" "RAW" "yes")
status=$(get_field "$result" status)
decoded=$(get_field "$result" decoded)
if [ "$status" != "200" ] || [ "$decoded" != "RAW:PROXYPAY" ]; then
  echo "Proxy payload CALL failed" >&2
  exit 1
fi

result=$(run_call "$PROXY_PORT" "ExecInterface" "fail" "proxyerr" "" "" "no")
status=$(get_field "$result" status)
message=$(get_field "$result" message)
if [ "$status" != "500" ] || [[ "$message" != *"FAIL:proxyerr (42)"* ]]; then
  echo "Proxy error propagation failed" >&2
  exit 1
fi

result=$(run_call "$PROXY_PORT" "PayloadInterface" "reject" "" "ignored" "RAW" "no")
status=$(get_field "$result" status)
message=$(get_field "$result" message)
if [ "$status" != "500" ] || [[ "$message" != *"REJECT:RAW"* ]]; then
  echo "Invalid payload handling through proxy failed" >&2
  exit 1
fi

check_no_zombies

python3 - "$PROXY_PORT" "$TOKEN" "$NAMESPACE" <<'PY'
import base64
import socket
import sys

def read_line(sock):
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            break
        data.extend(chunk)
        if chunk == b"\n":
            break
    return data.decode("utf-8", errors="replace").strip("\r\n")

def parse_status(line):
    parts = line.split(" ", 1)
    try:
        code = int(parts[0])
    except Exception:
        return 0, ""
    return code, parts[1] if len(parts) > 1 else ""

host = "127.0.0.1"
port = int(sys.argv[1])
token = sys.argv[2]
namespace = sys.argv[3]

with socket.create_connection((host, port)) as sock:
    parse_status(read_line(sock))
    sock.sendall(f"AUTH TOKEN {token}\n".encode())
    code, _ = parse_status(read_line(sock))
    if code != 200:
        raise SystemExit("auth failed")

    arg = base64.b64encode(b"conn-test").decode()
    sock.sendall(f"CALL {namespace}\\EchoInterface echo {arg}\n".encode())
    code1, msg1 = parse_status(read_line(sock))

    sock.sendall(f"CALL {namespace}\\PayloadInterface repeat\n".encode())
    sock.sendall(b"PAYLOAD RAW 3\nabc")
    code2, msg2 = parse_status(read_line(sock))

    if code1 != 200 or code2 != 200:
        raise SystemExit("unexpected status")
    if base64.b64decode(msg2).decode() != "RAW:abc:abc":
        raise SystemExit("payload mismatch")

print("proxy-multi-ok")
PY

check_no_zombies

echo "integration tests passed"
