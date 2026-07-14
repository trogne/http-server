# dense-http

A compact HTTP/1.1 web framework for Linux, written in C directly on TCP sockets.
It includes routing, an escaped template engine, SQLite persistence, static files,
a bounded accept queue, and a fixed pthread worker pool. No HTTP library is used.

It is suitable for learning, internal services, and controlled deployments. For
public Internet exposure, put it behind a TLS-terminating reverse proxy and apply
the resource limits appropriate to your environment.

## Features

- `GET` and `HEAD`, HTTP/1.0 and HTTP/1.1 request parsing
- HTTP/1.1 keep-alive with configurable idle timeout and request limit
- Static files rooted beneath a configured directory
- Traversal and symlink escape protection
- MIME types for common web, image, font, media, and WebAssembly files
- `Content-Length`, `Date`, `Last-Modified`, and defensive response headers
- Bounded connection queue and fixed-size pthread worker pool
- Common-style access logs with response time in microseconds
- Strict startup validation and graceful `SIGINT`/`SIGTERM` shutdown
- `/health` and `/api/stats` operational endpoints
- Dynamic `GET`, `HEAD`, and form `POST` routes
- `{{value}}` escaped and `{{{trusted_html}}}` raw template substitutions
- SQLite-backed notes example at `/notes`
- SQLite-backed JSON Users API at `/users`, with CRUD, search, sorting, and pagination
- A raw TCP protocol client (`tcp_client`)

## Build and run

On Linux:

```sh
make
./http_server --config server.conf
```

The checked-in `server.conf` expects to run from the repository root. Open
<http://localhost:8080> after startup. Command-line overrides are available:

```sh
./http_server --config server.conf --port 9000 --threads 16
```

The example application is at <http://localhost:8080/notes>. Talk to it without
curl or an HTTP client library:

```sh
./tcp_client 127.0.0.1 8080 /notes
```

The home page provides a browser UI for the Users API. Its JSON endpoints are
`GET /users`, `POST /users`, and `GET`, `PUT`, or `DELETE /users/{id}`. List
requests accept `page`, `limit`, `search`, `sort` (`id`, `name`, or `email`),
and `order` (`asc` or `desc`) query parameters.

User reads are public. `POST`, `PUT`, and `DELETE` require an API token supplied
as `Authorization: Bearer TOKEN`. Set a secret of at least 16 characters in the
`DENSE_HTTP_API_TOKEN` environment variable. If it is unset, user writes fail
closed with `503 Service Unavailable`. The browser UI keeps the entered token in
memory only; it is not committed or saved to local storage.

With Docker Desktop on Windows:

```powershell
docker compose up --build
```

Stop with `Ctrl+C`, then run `docker compose down` if needed.

## Configuration

Configuration uses one `key = value` per line; blank lines and lines beginning
with `#` are ignored. Unknown keys and invalid values prevent startup.

| Key | Default | Meaning |
| --- | ---: | --- |
| `port` | `8080` | TCP listen port |
| `threads` | `4` | Worker threads, 1–256 |
| `queue_capacity` | `256` | Accepted connections waiting for a worker |
| `keepalive_timeout` | `5` | Per-socket idle timeout in seconds |
| `keepalive_requests` | `100` | Maximum requests per connection |
| `document_root` | `./public` | Directory exposed as `/` |
| `log_file` | `-` | Append-only access log; `-` means stdout |
| `database_path` | `./app.db` | SQLite database file |
| `template_root` | `./templates` | Directory containing templates |

## Framework anatomy

`parse_request` is the protocol boundary: it parses the request line and headers,
enforces size limits, and describes the body. `serve_request` is the router: add
dynamic routes before its final static-file fallback. `notes_route` demonstrates
method dispatch, form decoding, prepared SQLite statements, redirects, and
template rendering. Template values are escaped by default; triple braces are
only for HTML produced by trusted application code.

CLI `--port` and `--threads` values override their configuration-file values,
regardless of argument order.

## Test

The test suite covers static content, MIME types, `HEAD`, keep-alive reuse,
dynamic endpoints, malformed requests, traversal attempts, logging, and 200
concurrent requests:

```sh
make test
```

## Deploy on Render

The included `render.yaml` deploys the Docker image as a Render web service.
Create a Render Blueprint from this repository to publish it. The free plan uses
ephemeral storage, so SQLite records can be lost on a restart or redeploy. For
durable records, use a paid service and attach a persistent disk at
`/srv/http/data`.

From Windows, either use Docker or an installed WSL distribution:

```powershell
.\scripts\test-docker.ps1
.\scripts\test-wsl.ps1 -Distro Ubuntu
```

## Benchmark with wrk

Install `wrk` and `curl`, then run:

```sh
make benchmark
```

The script starts an isolated server, waits for readiness, runs `wrk`, and
cleans up. Tune it through environment variables:

```sh
SERVER_THREADS=400 WRK_THREADS=8 CONNECTIONS=400 DURATION=30s PORT=18080 make benchmark
```

For a fully reproducible container benchmark:

```sh
docker build --target benchmark -t dense-http-benchmark .
docker run --rm dense-http-benchmark
```

`wrk` results depend heavily on CPU allocation, virtualization, logging, and
the host network stack. Compare changes on the same machine with the same
container/CPU limits rather than treating a single number as universal.

## Operational notes

- Overload is rejected with `503 Service Unavailable` instead of allowing an
  unbounded queue.
- Slow or idle clients are bounded by `keepalive_timeout`.
- A blocking worker owns each active keep-alive connection. Size `threads` for
  expected concurrent connections and keep the idle timeout conservative; the
  benchmark defaults server workers to its connection count.
- Access logging is synchronized; point `log_file` at durable storage or `-`
  for container log collection. The benchmark uses `/dev/null` so log I/O does
  not dominate the server throughput measurement.
- Only regular files are served. Directory requests map to `index.html`; there
  is no directory listing.
- Request bodies and transfer encodings are intentionally unsupported. Methods
  other than `GET` and `HEAD` receive `405`.
