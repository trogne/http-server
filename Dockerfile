FROM gcc:14-bookworm AS build
WORKDIR /src
COPY server.c Makefile ./
COPY tests/ tests/
RUN make

FROM build AS test
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
CMD ["make", "test"]

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --no-create-home server
COPY --from=build /src/http_server /usr/local/bin/http_server
USER server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/http_server"]
CMD ["8080", "4"]
