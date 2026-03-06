#!/bin/sh
set -e

CONFIG_PATH="/etc/udphole.conf"

UDPHBIN="/usr/bin/udphole"

if [ -f "$CONFIG_PATH" ]; then
    echo "Using mounted config: $CONFIG_PATH"
else
    echo "Generating config from environment variables..."

    {
        echo "[udphole]"
        echo "ports = ${UDP_PORTS:-7000-7999}"
        echo "listen = :${API_PORT:-6379}"

        if [ -n "$CLUSTER" ]; then
            for addr in $(echo "$CLUSTER" | tr ',' ' '); do
                echo "cluster = $addr"
            done
        fi

        echo ""
        echo "[user:${API_ADMIN_USER:-admin}]"
        echo "permit = *"
        echo "secret = ${API_ADMIN_PASS:-supers3cret}"
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
