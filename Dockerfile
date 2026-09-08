# Linux toolchain for the test/asan/memcheck gate. The host is arm64 macOS,
# where Valgrind has no port at all, so `make memcheck` can only run here.
# Trixie is pinned because -std=c23 needs GCC 14+ (GCC 13 only knows -std=c2x).
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        make \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

CMD ["make", "check"]
