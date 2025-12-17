import base64
import socket
import sys


def parse_max_proto(ext: str) -> int:
    proto = ext.split("PROTO=", 1)
    if len(proto) == 1:
        return 1

    digits = []
    for ch in proto[1]:
        if ch.isdigit():
            digits.append(ch)
        else:
            break

    return int("".join(digits)) if digits else 1


def read_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            break
        data.extend(chunk)
        if chunk == b"\n":
            break
    return data.decode("utf-8", errors="replace").strip("\r\n")


def parse_status(line: str) -> tuple[int, str]:
    parts = line.split(" ", 1)
    try:
        code = int(parts[0])
    except Exception:
        return 0, ""
    return code, parts[1] if len(parts) > 1 else ""


def main(host: str, port: int, token: str, namespace: str) -> None:
    with socket.create_connection((host, port)) as sock:
        banner_line = read_line(sock)
        server_max_proto = 1

        if " EXT " in banner_line:
            server_max_proto = parse_max_proto(banner_line.split(" EXT ", 1)[1])

        parse_status(banner_line)
        sock.sendall(f"AUTH TOKEN {token}\n".encode())
        code, _ = parse_status(read_line(sock))
        if code != 200:
            raise SystemExit("auth failed")

        if server_max_proto >= 2:
            sock.sendall(b"PROTO 2\n")
            code, _ = parse_status(read_line(sock))
            if code != 200:
                raise SystemExit("proto negotiation failed")

        arg = base64.b64encode(b"conn-test").decode()
        sock.sendall(f"CALL {namespace}\\EchoInterface echo {arg}\n".encode())
        code1, _ = parse_status(read_line(sock))

        sock.sendall(f"CALL {namespace}\\PayloadInterface repeat\n".encode())
        sock.sendall(b"PAYLOAD RAW 3\nabc")
        code2, msg2 = parse_status(read_line(sock))
        payload_resp = b""

        if code1 != 200 or code2 != 200:
            raise SystemExit("unexpected status")

        parts = msg2.split(" ", 1)
        if len(parts) == 2:
            try:
                payload_len = int(parts[1])
            except ValueError:
                payload_len = -1

            if payload_len >= 0:
                remaining = payload_len
                while remaining > 0:
                    chunk = sock.recv(remaining)
                    if not chunk:
                        break
                    payload_resp += chunk
                    remaining -= len(chunk)

                sock.recv(1)

        payload_text = (
            payload_resp.decode("utf-8", errors="replace")
            if payload_resp
            else base64.b64decode(msg2).decode()
        )

        if payload_text != "RAW:abc:abc":
            raise SystemExit("payload mismatch")

    print("proxy-multi-ok")


if __name__ == "__main__":
    try:
        port_arg = int(sys.argv[2])
    except (IndexError, ValueError):
        raise SystemExit("usage: proxy_socket_test.py <host> <port> <token> <namespace>")

    try:
        main(sys.argv[1], port_arg, sys.argv[3], sys.argv[4])
    except IndexError:
        raise SystemExit("usage: proxy_socket_test.py <host> <port> <token> <namespace>")
