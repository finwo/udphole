#!/bin/sh
set -e

CONFIG_PATH="/etc/udphole.conf"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  ARCH=amd64 ;;
    aarch64) ARCH=arm64 ;;
    riscv64) ARCH=riscv64 ;;
esac

UDPHBIN="/usr/bin/udphole-linux-${ARCH}"

if [ ! -f "$UDPHBIN" ]; then
    UDPHBIN="/usr/bin/udphole-linux-amd64"
fi

if [ -f "$CONFIG_PATH" ]; then
    echo "Using mounted config: $CONFIG_PATH"
else
    echo "Generating config from environment variables..."

    {
        echo "[udphole]"
        echo "ports = ${UDP_PORTS:-7000-7999}"
        echo "listen = :${API_PORT:-6379}"

        if [ -n "$CLUSTER" ]; then
            for name in $(echo "$CLUSTER" | tr ',' ' '); do
                echo "cluster = $name"
            done
        fi

        echo ""
        echo "[user:${API_ADMIN_USER:-admin}]"
        echo "permit = *"
        echo "secret = ${API_ADMIN_PASS:-supers3cret}"

        if [ -n "$CLUSTER" ]; then
            for name in $(echo "$CLUSTER" | tr ',' ' '); do
                env_var="CLUSTER_$name"
                eval "value=\$$env_var"
                if [ -n "$value" ]; then
                    proto="${value%%://*}"
                    rest="${value#*://}"

                    # Check if URL contains user:pass@ (has credentials)
                    case "$rest" in
                        *@*)
                            user="${rest%%:*}"
                            rest="${rest#*:}"
                            pass="${rest%%@*}"
                            rest="${rest#*@}"
                            has_creds=1
                            ;;
                        *)
                            user=""
                            pass=""
                            has_creds=0
                            ;;
                    esac

                    host="${rest%%:*}"
                    port="${rest#*:}"

                    echo ""
                    echo "[cluster:$name]"
                    echo "address = $value"
                    if [ "$has_creds" -eq 1 ]; then
                        echo "username = $user"
                        echo "password = $pass"
                    fi
                fi
            done
        fi
    } > "$CONFIG_PATH"

    echo "Generated config:"
    cat "$CONFIG_PATH"
fi

CMD="$UDPHBIN"

if [ -n "$LOG_LEVEL" ]; then
    CMD="$CMD --verbosity $LOG_LEVEL"
fi

if [ -n "$CLUSTER" ]; then
    CMD="$CMD cluster"
else
    CMD="$CMD daemon"
fi

CMD="$CMD --no-daemonize"

echo "Running: $CMD"
exec $CMD
