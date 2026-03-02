ARG VERSION=latest

FROM busybox:latest

ARG TARGETARCH

COPY udphole-linux-${TARGETARCH} /usr/bin/udphole

COPY entrypoint.sh /etc/rc.local
RUN chmod +x /etc/rc.local

ENTRYPOINT ["/bin/ash", "/etc/rc.local"]
