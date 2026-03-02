FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    wget \
    crossbuild-essential-amd64 \
    crossbuild-essential-arm64 \
    crossbuild-essential-riscv64

RUN wget -O /usr/local/bin/dep https://raw.githubusercontent.com/finwo/dep/master/dep && \
    chmod +x /usr/local/bin/dep

WORKDIR /src
COPY . .

RUN dep ensure && \
    make -j \
    CC=aarch64-linux-gnu-gcc   CFLAGS="-static-libgcc" TARGETARCH=arm64 && \
    mv udphole udphole-arm64 && \
    make clean && \
    make -j \
    CC=riscv64-linux-gnu-gcc   CFLAGS="-static-libgcc" TARGETARCH=riscv64 && \
    mv udphole udphole-riscv64 && \
    make clean && \
    make -j \
    CC=gcc                     CFLAGS="-static-libgcc" TARGETARCH=amd64 && \
    mv udphole udphole-amd64

FROM --platform=${TARGETPLATFORM} busybox:latest

COPY --from=builder /src/udphole-${TARGETARCH} /usr/bin/udphole
COPY entrypoint.sh /etc/rc.local

RUN chmod +x /etc/rc.local

ENTRYPOINT ["/bin/ash", "/etc/rc.local"]
