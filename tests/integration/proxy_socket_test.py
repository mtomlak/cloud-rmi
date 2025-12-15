import base64
import socket
import sys


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
        parse_status(read_line(sock))
        sock.sendall(f"AUTH TOKEN {token}\n".encode())
        code, _ = parse_status(read_line(sock))
        if code != 200:
            raise SystemExit("auth failed")

        arg = base64.b64encode(b"conn-test").decode()
        sock.sendall(f"CALL {namespace}\\EchoInterface echo {arg}\n".encode())
        code1, _ = parse_status(read_line(sock))

        sock.sendall(f"CALL {namespace}\\PayloadInterface repeat\n".encode())
        sock.sendall(b"PAYLOAD RAW 3\nabc")
        code2, msg2 = parse_status(read_line(sock))

        if code1 != 200 or code2 != 200:
            raise SystemExit("unexpected status")
        if base64.b64decode(msg2).decode() != "RAW:abc:abc":
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
