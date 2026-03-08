FROM gcc:13-bookworm AS build
WORKDIR /build
COPY common/ common/
COPY attend_service/main.c .
RUN gcc -O2 -Wall -Wextra -Icommon -o server main.c common/*.c

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /build/server .
COPY attend_service/data/ data/
ENV PORT=8080
ENV DATA_FILE=data/attendance.json
EXPOSE 8080
CMD ["./server"]
