# UDPHOLE

A standalone UDP wormhole proxy: forward UDP packets between sockets using a simple API

---

## Building

Requirements: [dep](https://github.com/finwo/dep), C compiler.

```bash
make
```

The binary is `udphole`.

---

## Global options

These options apply to all commands and must appear **before** the command name.

| Option | Short | Description |
|--------|-------|-------------|
| `--config <path>` | `-f` | Config file path. If omitted, the following are tried in order: `$HOME/.config/udphole.conf`, `$HOME/.udphole.conf`, `/etc/udphole/udphole.conf`, `/etc/udphole.conf`. |
| `--verbosity <level>` | `-v` | Log verbosity: fatal, error, warn, info, debug, trace (default: info). |
| `--log <path>` | | Also write log to file (SIGHUP reopens for logrotate). |

---

## Running the daemon

The main entry point is the **daemon** command.

```bash
# Foreground (default, config auto-detected)
./udphole daemon

# Explicit config file
./udphole -f /etc/udphole.conf daemon

# Background (daemonize)
./udphole -f /etc/udphole.conf daemon -d

# Force foreground even if config has daemonize=1
./udphole -f /etc/udphole.conf daemon -D
```

| Option | Short | Description |
|--------|--------|--------------|
| `--daemonize` | `-d` | Run in background (double fork, detach from terminal). |
| `--no-daemonize` | `-D` | Force foreground; overrides `daemonize=1` in config. |

Daemonize behaviour:

- By default the daemon runs in the **foreground**.
- It goes to the **background** only if `daemonize=1` is set in `[udphole]` **or** you pass `-d`/`--daemonize`.
- `-D`/`--no-daemonize` always forces foreground.

After starting, the daemon loads config, starts the UDP proxy manager, binds the API socket, and handles session/socket/forward management via RESP2 protocol. Logging goes to stderr (and optionally to a file if you use global `--log`).

---

## How it works

UDP hole is a simple UDP packet forwarder. It creates "sessions" that contain "sockets" and "forwards":

1. **Session**: A container for sockets and forwards. Has an idle timeout.
2. **Socket**: A UDP socket that can either:
   - **Listen**: Binds to a port and learns the remote address from the first packet received (NAT traversal)
   - **Connect**: Binds to a port and connects to a fixed remote address
3. **Forward**: Routes packets from a source socket to a destination socket

### Use cases

- **NAT traversal**: A socket in listen mode learns the remote address from the first incoming packet, enabling symmetric NAT traversal
- **Fixed forwarding**: Connect sockets to fixed IP:port for simple relay
- **Session management**: Idle sessions expire automatically, useful for temporary forwards

---

## Example: simple UDP echo

```bash
# Create a session with 60 second idle timeout
session.create mysession 60

# Create a listen socket (will learn remote from first packet)
session.socket.create.listen mysession socket1

# Get the port assigned to socket1 (response: [port, advertise_addr])
# The port will be in range 7000-7999

# From a remote client, send UDP packet to that port
# The socket now "knows" the remote address

# Create another socket to forward to
session.socket.create.connect mysession socket2 8.8.8.8 53

# Create forward: packets from socket1 -> socket2
session.forward.create mysession socket1 socket2

# Now packets received on socket1 are forwarded to 8.8.8.8:53
# And responses are sent back to the original sender
```

---

## Example: symmetric NAT traversal

```bash
# Create session
session.create nat-traversal 300

# Client A: create listen socket, gets port 7012
session.socket.create.listen nat-traversal client-a

# Client B: connect to client A's port
session.socket.create.connect nat-traversal client-b <client-a-ip> 7012

# Client B now receives packets from client A
session.forward.create nat-traversal client-a client-b

# Bidirectional
session.forward.create nat-traversal client-b client-a
```

---

## Configuration

```ini
[udphole]
mode = builtin
ports = 7000-7999
listen = :12345

[api]
listen = :12345
```

### `[udphole]`

| Option | Description |
|--------|-------------|
| `mode` | Currently only `builtin` is supported. |
| `ports` | Port range for UDP sockets, as `low-high` (e.g. `7000-7999`). Default 7000–7999. |
| `listen` | API server listen address. If not set, API server is disabled. |
| `advertise` | Optional. IP address to advertise in API responses instead of the port number. Useful when behind NAT. |

---

## API commands

The API uses the RESP2 (Redis) protocol. Connect with `redis-cli` or any Redis client library.

| Command | Response |
|---------|----------|
| `auth username password` | `+OK` or `-ERR invalid credentials` |
| `ping` | `+PONG` |
| `quit` | `+OK`, closes connection |
| `command` | List of commands accessible to the current user |

### Session commands

| Command | Response |
|---------|----------|
| `session.create <id> [idle_expiry]` | `+OK` - creates session, optional idle expiry in seconds (default 60) |
| `session.list` | Array of session IDs |
| `session.info <id>` | Map with: session_id, created, last_activity, idle_expiry, sockets, forwards, fd_count, marked_for_deletion |
| `session.destroy <id>` | `+OK` - destroys session and all its sockets/forwards |

### Socket commands

| Command | Response |
|---------|----------|
| `session.socket.create.listen <session_id> <socket_id>` | Array: `[port, advertise_addr]` |
| `session.socket.create.connect <session_id> <socket_id> <ip> <port>` | Array: `[port, advertise_addr]` |
| `session.socket.destroy <session_id> <socket_id>` | `+OK` |

### Forward commands

| Command | Response |
|---------|----------|
| `session.forward.list <session_id>` | Array of `[src_socket_id, dst_socket_id]` pairs |
| `session.forward.create <session_id> <src_socket_id> <dst_socket_id>` | `+OK` |
| `session.forward.destroy <session_id> <src_socket_id> <dst_socket_id>` | `+OK` |

---

*UDP hole is extracted from the UPBX project as a standalone UDP proxy daemon.*
