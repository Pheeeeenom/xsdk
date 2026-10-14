#!/usr/bin/env python3
"""The PC's side of the console's network checks (docs/scope/networking.md): echoes what arrives over TCP and UDP on
one port, and on a TCP line "back <port>" answers "ok", then connects to the sender on that port and sends
"hello from the PC". Logs each event; runs until stopped, or for --seconds."""

import argparse
import socket
import threading
import time

LINE_BYTES = 1024
CONNECT_BACK_DELAY_S = 0.2
CONNECT_TIMEOUT_S = 5


def log(text):
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def connect_back(host, port):
    # Lets the console reach its accept before the connection comes.
    time.sleep(CONNECT_BACK_DELAY_S)
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S) as back:
            back.sendall(b"hello from the PC\n")
        log(f"tcp: connected back to {host}:{port}")
    except OSError as error:
        log(f"tcp: couldn't connect back to {host}:{port}: {error}")


def serve_tcp_client(client, address):
    with client:
        try:
            data = client.recv(LINE_BYTES)
        except OSError as error:
            log(f"tcp {address[0]}:{address[1]}: {error}")
            return
        text = data.decode("ascii", "replace").strip()
        log(f"tcp {address[0]}:{address[1]}: {text!r}")
        if text.startswith("back "):
            client.sendall(b"ok\n")
            threading.Thread(target=connect_back, args=(address[0], int(text.split()[1])), daemon=True).start()
        else:
            client.sendall(data)


def serve_tcp(port):
    server = socket.create_server(("", port))
    while True:
        client, address = server.accept()
        threading.Thread(target=serve_tcp_client, args=(client, address), daemon=True).start()


def serve_udp(port):
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("", port))
    while True:
        data, address = server.recvfrom(LINE_BYTES)
        log(f"udp {address[0]}:{address[1]}: {data.decode('ascii', 'replace').strip()!r}")
        server.sendto(data, address)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=7370, help="the TCP and UDP port, 7370 by default")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    arguments = parser.parse_args()
    for serve in (serve_tcp, serve_udp):
        threading.Thread(target=serve, args=(arguments.port,), daemon=True).start()
    log(f"listening on TCP and UDP port {arguments.port}")
    try:
        if arguments.seconds:
            time.sleep(arguments.seconds)
        else:
            threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
