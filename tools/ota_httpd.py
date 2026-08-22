#!/usr/bin/env python3
"""Tiny HTTP server for OTA that logs exactly how many bytes each client read,
so we can tell a full download from an early abort. Serves one file for any path.
  usage: ota_httpd.py <image> [port]
"""
import sys, socket, os, time

IMG = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
data = open(IMG, "rb").read()
print(f"serving {IMG} ({len(data)} bytes) on :{PORT}", flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT)); srv.listen(5)

while True:
    cli, addr = srv.accept()
    t0 = time.time()
    req = b""
    try:
        cli.settimeout(10)
        while b"\r\n\r\n" not in req:
            chunk = cli.recv(1024)
            if not chunk: break
            req += chunk
        line1 = req.split(b"\r\n", 1)[0].decode("latin1")
        print(f"[{addr[0]}] request: {line1}", flush=True)
        hdr = (f"HTTP/1.1 200 OK\r\nContent-Length: {len(data)}\r\n"
               f"Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n").encode()
        cli.sendall(hdr)
        sent = 0
        view = memoryview(data)
        while sent < len(data):
            try:
                n = cli.send(view[sent:sent + 4096])
                if n == 0: break
                sent += n
            except (BrokenPipeError, ConnectionResetError):
                print(f"[{addr[0]}] client closed after {sent}/{len(data)} bytes "
                      f"({100*sent//len(data)}%) in {time.time()-t0:.1f}s", flush=True)
                break
        else:
            print(f"[{addr[0]}] sent FULL {sent} bytes in {time.time()-t0:.1f}s", flush=True)
    except Exception as e:
        print(f"[{addr[0]}] error: {e!r}", flush=True)
    finally:
        cli.close()
