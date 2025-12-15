#!/usr/bin/env python3
import argparse
import base64
import socket
import sys
from typing import Tuple


def read_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        try:
            chunk = sock.recv(1)
        except socket.timeout:
            break
        if not chunk:
            break
        data.extend(chunk)
        if chunk == b"\n":
            break
    return data.decode("utf-8", errors="replace").strip("\r\n")


def parse_status(line: str) -> Tuple[int, str]:
    if not line:
        return 0, ""
    parts = line.split(" ", 1)
    try:
        code = int(parts[0])
    except ValueError:
        code = 0
    message = parts[1] if len(parts) > 1 else ""
    return code, message


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--iface", required=True)
    parser.add_argument("--method", required=True)
    parser.add_argument("--arg", default="")
    parser.add_argument("--payload", default=None)
    parser.add_argument("--encoding", default="RAW")
    parser.add_argument("--decode-output", action="store_true")
    parser.add_argument("--namespace", default=None)
    args = parser.parse_args()

    payload_bytes = args.payload.encode("utf-8") if args.payload is not None else None

    iface = args.iface
    if args.namespace and not iface.startswith("\\"):
        iface = f"{args.namespace}\\{iface}"

    with socket.create_connection((args.host, args.port)) as sock:
        sock.settimeout(5)
        banner = parse_status(read_line(sock))
        if banner[0] != 220:
            print("status:0")
            print("message:failed banner")
            print("decoded:")
            return 1

        sock.sendall(f"AUTH TOKEN {args.token}\n".encode("utf-8"))
        auth_code, _ = parse_status(read_line(sock))
        if auth_code != 200:
            print("status:0")
            print("message:auth failed")
            print("decoded:")
            return 1

        if payload_bytes is not None:
            sock.sendall(f"CALL {iface} {args.method}\n".encode("utf-8"))
            sock.sendall(
                f"PAYLOAD {args.encoding} {len(payload_bytes)}\n".encode("utf-8")
            )
            if payload_bytes:
                sock.sendall(payload_bytes)
        else:
            encoded_arg = base64.b64encode(args.arg.encode("utf-8")).decode("ascii")
            sock.sendall(f"CALL {iface} {args.method} {encoded_arg}\n".encode("utf-8"))

        status_code, message = parse_status(read_line(sock))

    decoded_message = ""
    if args.decode_output and status_code == 200 and message:
        try:
            decoded_message = base64.b64decode(message).decode("utf-8", errors="replace")
        except Exception:
            decoded_message = ""

    print(f"status:{status_code}")
    print(f"message:{message}")
    print(f"decoded:{decoded_message}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
