# Multithreaded HTTP server in C

A small Linux HTTP/1.1 server using a bounded connection queue and a fixed
`pthread` worker pool. It supports `GET` and `HEAD`, generates personalized
request-time HTML at `/?name=Ada`, and exposes live process data at
`/api/stats`. The stable `/health` endpoint remains available for health
checks. It also returns useful HTTP errors, applies socket timeouts, and shuts
down on `SIGINT` or `SIGTERM`.

## Test it from Windows with Docker Desktop

Docker compiles and runs the program inside Linux, so no Windows C toolchain is
used.

Run the complete Docker build and test suite with:

```powershell
.\scripts\test-docker.ps1
```

The equivalent commands are:

```powershell
docker build --target test -t pthread-http-test .
docker run --rm pthread-http-test
```

If Docker reports an internal server error, restart Docker Desktop. If that
continues, update Docker Desktop before retrying.

To run the server after the tests:

```powershell
docker compose up --build
```

Open <http://localhost:8080> or, in another PowerShell window, run:

```powershell
Invoke-RestMethod http://localhost:8080/health
```

Stop it with `Ctrl+C`, then remove the stopped container with:

```powershell
docker compose down
```

The suite checks normal responses, `HEAD`, error responses, and 200 concurrent
requests.

## Alternative: WSL

Run the complete build and test suite in your existing Ubuntu distro:

```powershell
.\scripts\test-wsl.ps1
```

The equivalent commands inside Ubuntu are:

```bash
cd /mnt/c/Users/patri/codex
make test
./http_server 8080 4
```

If the compiler or Python is missing, install them once with:

```bash
sudo apt update && sudo apt install -y build-essential python3
```

## Native Linux usage

```bash
make
./http_server                 # defaults: port 8080, 4 worker threads
./http_server 9000 16         # custom port and worker count
make test
```

The server intentionally closes each connection after one response. It is an
educational, robust baseline—not a replacement for a production proxy such as
nginx or Caddy.
