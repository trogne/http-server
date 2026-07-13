FROM gcc:14-bookworm AS build
WORKDIR /src
COPY server.c Makefile ./
COPY tests/ tests/
COPY scripts/ scripts/
COPY public/ public/
COPY server.conf ./
RUN make

FROM build AS test
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
CMD ["make", "test"]

FROM build AS benchmark
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl wrk \
    && rm -rf /var/lib/apt/lists/*
CMD ["make", "benchmark"]

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --no-create-home server
COPY --from=build /src/http_server /usr/local/bin/http_server
COPY --from=build /src/public /srv/http/public
COPY --from=build /src/server.conf /etc/dense-http.conf
WORKDIR /srv/http
USER server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/http_server"]
CMD ["--config", "/etc/dense-http.conf"]
