FROM gcc:13-bookworm AS build
WORKDIR /build
RUN apt-get update && apt-get install -y libevent-dev && rm -rf /var/lib/apt/lists/*
COPY common/ common/
COPY id_service/main.c .
RUN gcc -O2 -Wall -Wextra -Icommon -o server main.c \
    common/json_util.c common/http_parse.c common/tcp_handler.c \
    -levent

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y libevent-2.1-7 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /build/server .
COPY id_service/data/ data/
ENV PORT=8080
ENV DATA_FILE=data/ids.json
EXPOSE 8080
CMD ["./server"]
