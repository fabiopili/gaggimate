"""One-shot diagnostic: query a GaggiMate display's websocket for OTA settings.

Prints display/controller firmware versions and hardware. Run from a machine
that can reach the display, for example:

    python3 scripts/probe_display_ws.py 192.168.1.85
"""

import base64
import json
import os
import socket
import struct
import sys


def ws_connect(host, port, path):
    sock = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
    )
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed during handshake")
        resp += chunk
    status = resp.split(b"\r\n", 1)[0].decode(errors="replace")
    if " 101" not in status:
        raise ConnectionError(f"handshake failed: {status}")
    return sock


def send_text(sock, text):
    payload = text.encode()
    mask = os.urandom(4)
    header = bytearray([0x81])
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    else:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(header) + mask + masked)


def recv_frame(sock):
    header = b""
    while len(header) < 2:
        chunk = sock.recv(2 - len(header))
        if not chunk:
            return None
        header += chunk
    opcode = header[0] & 0x0F
    length = header[1] & 0x7F
    if length == 126:
        while len(header) < 4:
            header += sock.recv(4 - len(header))
        length = struct.unpack(">H", header[2:4])[0]
    elif length == 127:
        while len(header) < 10:
            header += sock.recv(10 - len(header))
        length = struct.unpack(">Q", header[2:10])[0]
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    return opcode, data


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "gaggimate.local"
    sock = ws_connect(host, 80, "/ws")
    try:
        send_text(sock, json.dumps({"tp": "req:ota-settings", "rid": "probe"}))
        for _ in range(20):
            frame = recv_frame(sock)
            if frame is None:
                break
            opcode, data = frame
            if opcode != 1:
                continue
            try:
                message = json.loads(data)
            except ValueError:
                continue
            if "displayVersion" in message:
                print(json.dumps(message, indent=2))
                return
            if str(message.get("tp", "")).endswith(":reject"):
                print("rejected:", json.dumps(message))
                return
        print("no ota-settings response received")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
