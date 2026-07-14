FROM gcc:14-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-dev && rm -rf /var/lib/apt/lists/*
COPY server.c Makefile ./
COPY client.c ./
COPY tests/ tests/
COPY scripts/ scripts/
COPY public/ public/
COPY templates/ templates/
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
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home server
COPY --from=build /src/http_server /usr/local/bin/http_server
COPY --from=build /src/tcp_client /usr/local/bin/tcp_client
COPY --from=build /src/public /srv/http/public
COPY --from=build /src/templates /srv/http/templates
COPY --from=build /src/server.conf /etc/dense-http.conf
WORKDIR /srv/http
USER server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/http_server"]
CMD ["--config", "/etc/dense-http.conf"]
