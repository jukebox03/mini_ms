FROM gcc:13-bookworm AS build
WORKDIR /build
COPY common/ common/
COPY frontend/main.c .
RUN gcc -O2 -Wall -Wextra -Icommon -o server main.c common/*.c

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build/server .
ENV PORT=8080
EXPOSE 8080
CMD ["./server"]
