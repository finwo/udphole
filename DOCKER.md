# UDPHOLE Docker

Run udphole as a Docker container.

## Quick Start

```bash
docker run -p 6379:6379 -p 7000-7999:7000-7999/udp finwo/udphole
```

The entrypoint always adds `--no-daemonize` to ensure the container stays running.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `API_PORT` | `6379` | TCP port for the RESP2 API server |
| `UDP_PORTS` | `7000-7999` | UDP port range for UDP sockets |
| `LOG_LEVEL` | `info` | Log verbosity: fatal, error, warn, info, debug, trace |
| `API_ADMIN_USER` | `admin` | Username for admin user |
| `API_ADMIN_PASS` | `supers3cret` | Password for admin user |
| `CLUSTER` | | Comma-separated list of cluster node addresses (enables cluster mode, e.g., `tcp://user:pass@host1:6379,tcp://user:pass@host2:6379`) |

## Auto-generated Config

If no config file is mounted, the container auto-generates `/etc/udphole.conf` from environment variables:

```ini
[udphole]
ports = 7000-7999
listen = :6379

[user:admin]
permit = *
secret = supers3cret
```

### Custom Config

Mount your own config file to override defaults:

```bash
docker run -p 6379:6379 -p 7000-7999:7000-7999/udp \
  -v /path/to/udphole.conf:/etc/udphole.conf:ro \
  finwo/udphole
```

Note: Even with cluster fields defined in a mounted config, you must set `CLUSTER` to any value to run in cluster mode (the entrypoint uses this to decide between `daemon` and `cluster` commands).

### Listen Address Formats

The `listen` directive supports multiple addresses:

| Format | Description |
|--------|-------------|
| `:6379` | All interfaces, port 6379 |
| `6379` | All interfaces, port 6379 |
| `localhost:6379` | Loopback only |
| `192.168.1.1:6379` | Specific IPv4 address |
| `tcp://:6379` | Explicit TCP, all interfaces |
| `tcp://localhost:6379` | Explicit TCP, loopback |
| `unix:///tmp/udphole.sock` | Unix socket |

Multiple listen addresses can be specified:

```ini
[udphole]
listen = :6379
listen = 192.168.1.1:6380
listen = unix:///tmp/udphole.sock
```

## Cluster Mode

To run in cluster mode, set the `CLUSTER` environment variable with comma-separated node addresses:

```bash
docker run -p 6379:6379 -p 7000-7999:7000-7999/udp \
  -e CLUSTER="tcp://user:pass@192.168.1.10:6379,tcp://user:pass@192.168.1.11:6379" \
  finwo/udphole
```

This generates:

```ini
[udphole]
ports = 7000-7999
listen = :6379
cluster = tcp://user:pass@192.168.1.10:6379
cluster = tcp://user:pass@192.168.1.11:6379

[user:admin]
permit = *
secret = supers3cret
```

### Cluster Node Address Formats

| Format | Description |
|--------|-------------|
| `tcp://host:port` | TCP connection |
| `tcp://user:pass@host:port` | TCP with authentication |
| `unix:///path/to/socket` | Unix socket |

## Docker Compose

```yaml
services:
  udphole:
    image: finwo/udphole
    ports:
      - "6379:6379"
      - "7000-7999:7000-7999/udp"
    environment:
      - API_PORT=6379
      - UDP_PORTS=7000-7999
      - LOG_LEVEL=info
      - API_ADMIN_USER=admin
      - API_ADMIN_PASS=supers3cret
    healthcheck:
      test: ["CMD", "nc", "-z", "localhost", "6379"]
      interval: 10s
      timeout: 5s
      retries: 3
```

## Healthcheck

The container includes a healthcheck using `nc` to verify the API port is listening:

```bash
nc -z localhost 6379
```

## Architecture

The container is available for:
- `linux/amd64`
- `linux/arm64/v8` (e.g., Raspberry Pi 4+, M1 Mac)
- `linux/riscv64`

Docker automatically selects the correct variant for your host architecture.
