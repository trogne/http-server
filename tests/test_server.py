#!/usr/bin/env python3
import concurrent.futures
import http.client
import json
import signal
import socket
import subprocess
import sys
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, method="GET", path="/"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request(method, path)
    response = connection.getresponse()
    body = response.read()
    result = response.status, dict(response.getheaders()), body
    connection.close()
    return result


def wait_until_ready(process, port):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(f"server exited early\nstdout: {stdout}\nstderr: {stderr}")
        try:
            if request(port, path="/health")[0] == 200:
                return
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./http_server"
    port = free_port()
    process = subprocess.Popen(
        [binary, str(port), "8"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        wait_until_ready(process, port)

        status, headers, body = request(port)
        assert status == 200
        assert b"It works!" in body
        assert headers["Content-Type"].startswith("text/html")

        status, _, body = request(port, path="/?name=Ada%20Lovelace")
        assert status == 200 and b"Hello, Ada Lovelace!" in body

        status, _, body = request(port, path="/?name=%3Cscript%3E")
        assert status == 200
        assert b"&lt;script&gt;" in body and b"<script>" not in body

        status, headers, body = request(port, "HEAD", "/")
        assert status == 200
        assert body == b""
        assert int(headers["Content-Length"]) > 0

        status, _, body = request(port, path="/health")
        assert status == 200 and body == b'{"status":"ok"}\n'

        status, headers, body = request(port, path="/api/stats")
        stats = json.loads(body)
        assert status == 200
        assert headers["Content-Type"] == "application/json"
        assert stats["status"] == "ok"
        assert stats["requests"] >= 6
        assert stats["active_connections"] >= 1
        assert stats["worker_threads"] == 8

        assert request(port, path="/missing")[0] == 404
        assert request(port, method="POST")[0] == 405

        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            futures = [pool.submit(request, port, "GET", "/health") for _ in range(200)]
            results = [future.result() for future in futures]
        assert all(status == 200 and body == b'{"status":"ok"}\n'
                   for status, _, body in results)
        print("All HTTP and concurrency tests passed (200 requests, 32 clients).")
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if process.returncode not in (0, -signal.SIGTERM):
            stdout, stderr = process.communicate()
            print(f"server output:\n{stdout}\n{stderr}", file=sys.stderr)


if __name__ == "__main__":
    main()
