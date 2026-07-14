#!/usr/bin/env python3
import concurrent.futures
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, method="GET", path="/"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request(method, path)
    response = connection.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    connection.close()
    return result


def wait_ready(process, port):
    for _ in range(160):
        if process.poll() is not None:
            out, err = process.communicate()
            raise RuntimeError(f"server exited early\nstdout: {out}\nstderr: {err}")
        try:
            if request(port, path="/health")[0] == 200:
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def raw_request(port, payload):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(payload)
        return sock.recv(4096)


def json_request(port, method, path, payload=None, token=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    headers = {}
    if payload is not None:
        headers["Content-Type"] = "application/json"
        headers["Content-Length"] = str(len(payload))
    if token is not None:
        headers["Authorization"] = f"Bearer {token}"
    connection.request(method, path, body=payload, headers=headers)
    response = connection.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    connection.close()
    return result


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./http_server")
    port = free_port()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "public"
        root.mkdir()
        (root / "index.html").write_text("<h1>static home</h1>\n", encoding="utf-8")
        (root / "app.css").write_text("body { color: green; }\n", encoding="utf-8")
        (root / "data.bin").write_bytes(b"\x00\x01\x02")
        log = Path(temp) / "access.log"
        config = Path(temp) / "server.conf"
        config.write_text(
            f"port = {port}\nthreads = 8\nqueue_capacity = 64\n"
            f"keepalive_timeout = 2\nkeepalive_requests = 10\n"
            f"document_root = {root}\nlog_file = {log}\n"
            f"database_path = {Path(temp) / 'app.db'}\n"
            f"template_root = {Path.cwd() / 'templates'}\n",
            encoding="utf-8",
        )
        token = "test-api-token-123456789"
        environment = os.environ.copy()
        environment["DENSE_HTTP_API_TOKEN"] = token
        process = subprocess.Popen(
            [binary, "--config", str(config)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=environment,
        )
        try:
            wait_ready(process, port)

            status, headers, body = request(port)
            assert status == 200 and body == b"<h1>static home</h1>\n"
            assert headers["Content-Type"].startswith("text/html")
            assert headers["X-Content-Type-Options"] == "nosniff"

            status, headers, body = request(port, path="/app.css?cache=1")
            assert status == 200 and body.startswith(b"body")
            assert headers["Content-Type"].startswith("text/css")

            status, headers, body = request(port, "HEAD", "/data.bin")
            assert status == 200 and body == b"" and headers["Content-Length"] == "3"
            assert headers["Content-Type"] == "application/octet-stream"

            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            connection.request("GET", "/health")
            first = connection.getresponse()
            health = json.loads(first.read())
            assert first.status == 200 and health["status"] == "ok"
            assert isinstance(health["uptime_seconds"], int)
            socket_id = connection.sock.fileno()
            connection.request("GET", "/api/stats")
            second = connection.getresponse()
            stats = json.loads(second.read())
            assert second.status == 200 and connection.sock.fileno() == socket_id
            assert stats["worker_threads"] == 8 and stats["active_connections"] >= 1
            connection.close()

            assert request(port, path="/missing")[0] == 404
            assert request(port, method="POST")[0] == 405

            status, _, body = request(port, path="/notes")
            assert status == 200 and b"<h1>Notes</h1>" in body
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            payload = "text=%3Cscript%3Ealert%281%29%3C%2Fscript%3E"
            connection.request("POST", "/notes", payload, {
                "Content-Type": "application/x-www-form-urlencoded",
                "Content-Length": str(len(payload)),
            })
            response = connection.getresponse()
            assert response.status == 303 and response.getheader("Location") == "/notes"
            response.read(); connection.close()
            status, _, body = request(port, path="/notes")
            assert status == 200 and b"&lt;script&gt;alert(1)&lt;/script&gt;" in body
            assert b"<script>alert(1)</script>" not in body
            assert raw_request(port, b"GET / HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 400")
            assert raw_request(port, b"GET /%2e%2e/secret HTTP/1.1\r\nHost: x\r\n\r\n").startswith(
                b"HTTP/1.1 400"
            )

            assert json_request(port, "POST", "/users", b'{"name":"Ada","email":"ada@example.com"}')[0] == 401
            status, headers, body = json_request(port, "POST", "/users", b'{"name":"Ada","email":"ada@example.com"}', token)
            assert status == 201 and headers["Content-Type"].startswith("application/json")
            created = json.loads(body)
            assert created["name"] == "Ada" and created["email"] == "ada@example.com"
            user_id = created["id"]

            status, _, body = json_request(port, "GET", f"/users/{user_id}")
            assert status == 200 and json.loads(body)["id"] == user_id

            status, _, body = json_request(port, "POST", "/users", b'{"name":"Grace","email":"grace@example.com"}', token)
            assert status == 201

            status, _, body = json_request(port, "GET", "/users")
            payload = json.loads(body)
            assert status == 200 and any(item["id"] == user_id for item in payload["users"])

            status, _, body = json_request(port, "GET", "/users?sort=name&order=desc&limit=10")
            payload = json.loads(body)
            assert status == 200 and payload["users"][0]["name"] == "Grace"

            status, _, body = json_request(port, "GET", "/users?page=1&limit=1")
            payload = json.loads(body)
            assert status == 200 and payload["page"] == 1 and payload["limit"] == 1 and len(payload["users"]) <= 1

            status, _, body = json_request(port, "GET", "/users?search=ada")
            payload = json.loads(body)
            assert status == 200 and any(item["email"] == "ada@example.com" for item in payload["users"])

            status, _, body = json_request(port, "POST", "/users", b'{"name":"Ada Lovelace","email":"ada.l@example.com"}', token)
            assert status == 201
            status, _, body = json_request(port, "GET", "/users?search=Ada%20Lovelace")
            assert status == 200 and any(item["name"] == "Ada Lovelace" for item in json.loads(body)["users"])

            status, _, body = json_request(port, "PUT", f"/users/{user_id}", b'{"name":"Grace","email":"grace@example.com"}', token)
            assert status == 200 and json.loads(body)["name"] == "Grace"

            status, _, body = json_request(port, "DELETE", f"/users/{user_id}", token=token)
            assert status == 200 and json.loads(body)["deleted"] is True

            status, _, body = json_request(port, "GET", f"/users/{user_id}")
            assert status == 404 and b"not found" in body.lower()

            assert json_request(port, "PUT", "/users/2147483647", b'{"name":"Nobody","email":"nobody@example.com"}', token)[0] == 404
            assert json_request(port, "DELETE", "/users/2147483647", token=token)[0] == 404
            assert request(port, method="DELETE", path="/index.html")[0] == 405
            assert request(port, method="DELETE", path="/notes")[0] == 405
            assert json_request(port, "PUT", "/health", b'{}')[0] == 405
            assert request(port, method="DELETE", path="/api/stats")[0] == 405

            status, _, body = json_request(port, "POST", "/users", b'{bad json}', token)
            assert status == 400

            status, _, body = json_request(port, "POST", "/users", b'{"name":"","email":"ada@example.com"}', token)
            assert status == 400 and b"invalid" in body.lower()

            status, _, body = json_request(port, "POST", "/users", b'{"name":"Ada","email":"not-an-email"}', token)
            assert status == 400 and b"invalid" in body.lower()

            with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
                results = list(pool.map(lambda _: request(port, path="/health"), range(200)))
            assert all(s == 200 for s, _, _ in results)
            assert all(b.startswith(b'{"status":"ok"') for _, _, b in results)

            time.sleep(0.1)
            lines = log.read_text(encoding="utf-8").splitlines()
            assert len(lines) >= 209 and any('"GET /app.css?cache=1 HTTP/1.1" 200' in x for x in lines)
            print("All HTTP, static-file, keep-alive, parser, logging, and concurrency tests passed.")
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if process.returncode not in (0, -signal.SIGTERM):
                out, err = process.communicate()
                print(f"server output:\n{out}\n{err}", file=sys.stderr)


if __name__ == "__main__":
    main()
