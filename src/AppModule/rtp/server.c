#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rxi/log.h"
#include "SchedulerModule/protothreads.h"
#include "SchedulerModule/scheduler.h"
#include "common/socket_util.h"
#include "config.h"
#include "tidwall/hashmap.h"
#include "server.h"
#include "AppModule/api/server.h"

#define RTP_SESSION_HASH_SIZE 256
#define RTP_BUFFER_SIZE 4096
#define DEFAULT_IDLE_EXPIRY 60

typedef struct rtp_socket {
  char *socket_id;
  int *fds;
  int local_port;
  int mode;
  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen;
  int learned_valid;
  struct sockaddr_storage learned_addr;
  socklen_t learned_addrlen;
} rtp_socket_t;

typedef struct rtp_forward {
  char *src_socket_id;
  char *dst_socket_id;
} rtp_forward_t;

typedef struct rtp_session {
  char *session_id;
  time_t idle_expiry;
  time_t created;
  time_t last_activity;
  rtp_socket_t **sockets;
  size_t sockets_count;
  rtp_forward_t *forwards;
  size_t forwards_count;
  int marked_for_deletion;
  int *ready_fds;
  int *all_fds;
  struct pt pt;
  struct pt_task *task;
} rtp_session_t;

static rtp_session_t **sessions = NULL;
static size_t sessions_count = 0;
static char *advertise_addr = NULL;
static int port_low = 7000;
static int port_high = 7999;
static int port_cur = 7000;
static int running = 0;

static rtp_session_t *find_session(const char *session_id) {
  for (size_t i = 0; i < sessions_count; i++) {
    if (strcmp(sessions[i]->session_id, session_id) == 0) {
      return sessions[i];
    }
  }
  return NULL;
}

static uint64_t socket_hash(const void *item, uint64_t seed0, uint64_t seed1) {
  const rtp_socket_t *s = item;
  return hashmap_sip(s->socket_id, strlen(s->socket_id), seed0, seed1);
}

static int socket_compare(const void *a, const void *b, void *udata) {
  (void)udata;
  const rtp_socket_t *sa = a;
  const rtp_socket_t *sb = b;
  return strcmp(sa->socket_id, sb->socket_id);
}

static rtp_socket_t *find_socket(rtp_session_t *s, const char *socket_id) {
  if (!s || !s->sockets || !socket_id) return NULL;
  for (size_t i = 0; i < s->sockets_count; i++) {
    if (s->sockets[i] && strcmp(s->sockets[i]->socket_id, socket_id) == 0) {
      return s->sockets[i];
    }
  }
  return NULL;
}

static int alloc_port(void) {
  for (int i = 0; i < port_high - port_low; i++) {
    int port = port_cur + i;
    if (port > port_high) port = port_low;
    port_cur = port + 1;
    if (port_cur > port_high) port_cur = port_low;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) continue;
    int ok = (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(udp_fd);
    if (!ok) continue;

    return port;
  }
  return 0;
}

static int parse_ip_addr(const char *ip_str, int port, struct sockaddr_storage *addr, socklen_t *addrlen) {
  memset(addr, 0, sizeof(*addr));

  struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
  if (inet_pton(AF_INET, ip_str, &addr4->sin_addr) == 1) {
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    *addrlen = sizeof(*addr4);
    return 0;
  }

  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, ip_str, &addr6->sin6_addr) == 1) {
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
    *addrlen = sizeof(*addr6);
    return 0;
  }

  return -1;
}

static void close_socket(rtp_socket_t *sock) {
  if (!sock || !sock->fds) return;
  for (int i = 1; i <= sock->fds[0]; i++) {
    if (sock->fds[i] >= 0) {
      close(sock->fds[i]);
    }
  }
  free(sock->fds);
  sock->fds = NULL;
}

static void free_socket(rtp_socket_t *sock) {
  if (!sock) return;
  close_socket(sock);
  free(sock->socket_id);
  free(sock);
}

static void destroy_session(rtp_session_t *s) {
  if (!s) return;
  s->marked_for_deletion = 1;
  for (size_t i = 0; i < sessions_count; i++) {
    if (sessions[i] == s) {
      sessions[i] = NULL;
      break;
    }
  }
}

static rtp_session_t *create_session(const char *session_id, int idle_expiry) {
  const rtp_session_t *cs = find_session(session_id);
  if (cs) return (rtp_session_t *)cs;

  rtp_session_t *s = calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->session_id = strdup(session_id);
  s->created = time(NULL);
  s->last_activity = s->created;
  s->idle_expiry = idle_expiry > 0 ? idle_expiry : DEFAULT_IDLE_EXPIRY;

  sessions = realloc(sessions, sizeof(rtp_session_t *) * (sessions_count + 1));
  sessions[sessions_count++] = s;

  return s;
}

static void cleanup_expired_sessions(void) {
  if (!sessions) return;
  time_t now = time(NULL);

  for (size_t i = 0; i < sessions_count; i++) {
    rtp_session_t *s = sessions[i];
    if (!s) continue;
    if (now - s->last_activity > s->idle_expiry) {
      log_debug("udphole: session %s expired (idle %ld > expiry %ld)",
                s->session_id, (long)(now - s->last_activity), (long)s->idle_expiry);
      destroy_session(s);
    }
  }
}

static int add_forward(rtp_session_t *s, const char *src_id, const char *dst_id) {
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_id) == 0 &&
        strcmp(s->forwards[i].dst_socket_id, dst_id) == 0) {
      return 0;
    }
  }

  rtp_forward_t *new_forwards = realloc(s->forwards, sizeof(rtp_forward_t) * (s->forwards_count + 1));
  if (!new_forwards) return -1;
  s->forwards = new_forwards;

  s->forwards[s->forwards_count].src_socket_id = strdup(src_id);
  s->forwards[s->forwards_count].dst_socket_id = strdup(dst_id);
  s->forwards_count++;

  return 0;
}

static int remove_forward(rtp_session_t *s, const char *src_id, const char *dst_id) {
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_id) == 0 &&
        strcmp(s->forwards[i].dst_socket_id, dst_id) == 0) {
      free(s->forwards[i].src_socket_id);
      free(s->forwards[i].dst_socket_id);
      for (size_t j = i; j < s->forwards_count - 1; j++) {
        s->forwards[j] = s->forwards[j + 1];
      }
      s->forwards_count--;
      return 0;
    }
  }
  return -1;
}

static rtp_socket_t *create_listen_socket(rtp_session_t *sess, const char *socket_id) {
  rtp_socket_t *existing = find_socket(sess, socket_id);
  if (existing) return existing;

  int port = alloc_port();
  if (!port) {
    log_error("udphole: no ports available");
    return NULL;
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);
  int *fds = udp_recv(port_str, NULL, NULL);
  if (!fds || fds[0] == 0) {
    log_error("udphole: failed to create UDP socket on port %d", port);
    free(fds);
    return NULL;
  }

  rtp_socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id = strdup(socket_id);
  sock->fds = fds;
  sock->local_port = port;
  sock->mode = 0;
  sock->learned_valid = 0;

  sess->sockets = realloc(sess->sockets, sizeof(rtp_socket_t *) * (sess->sockets_count + 1));
  sess->sockets[sess->sockets_count++] = sock;

  log_debug("udphole: created listen socket %s in session %s on port %d",
            socket_id, sess->session_id, port);
  return sock;
}

static rtp_socket_t *create_connect_socket(rtp_session_t *sess, const char *socket_id,
                                           const char *ip, int port) {
  rtp_socket_t *existing = find_socket(sess, socket_id);
  if (existing) return existing;

  int local_port = alloc_port();
  if (!local_port) {
    log_error("udphole: no ports available");
    return NULL;
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", local_port);
  int *fds = udp_recv(port_str, NULL, NULL);
  if (!fds || fds[0] == 0) {
    log_error("udphole: failed to create UDP socket on port %d", local_port);
    free(fds);
    return NULL;
  }

  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen;
  if (parse_ip_addr(ip, port, &remote_addr, &remote_addrlen) != 0) {
    log_error("udphole: invalid remote address %s:%d", ip, port);
    free(fds);
    return NULL;
  }

  rtp_socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id = strdup(socket_id);
  sock->fds = fds;
  sock->local_port = local_port;
  sock->mode = 1;
  sock->remote_addr = remote_addr;
  sock->remote_addrlen = remote_addrlen;
  sock->learned_valid = 0;

  sess->sockets = realloc(sess->sockets, sizeof(rtp_socket_t *) * (sess->sockets_count + 1));
  sess->sockets[sess->sockets_count++] = sock;

  log_debug("udphole: created connect socket %s in session %s on port %d -> %s:%d",
            socket_id, sess->session_id, local_port, ip, port);
  return sock;
}

static int destroy_socket(rtp_session_t *sess, const char *socket_id) {
  rtp_socket_t *sock = find_socket(sess, socket_id);
  if (!sock) return -1;

  for (size_t i = 0; i < sess->sockets_count; i++) {
    if (sess->sockets[i] == sock) {
      sess->sockets[i] = NULL;
      break;
    }
  }
  free_socket(sock);

  for (size_t i = 0; i < sess->forwards_count; ) {
    if (strcmp(sess->forwards[i].src_socket_id, socket_id) == 0 ||
        strcmp(sess->forwards[i].dst_socket_id, socket_id) == 0) {
      free(sess->forwards[i].src_socket_id);
      free(sess->forwards[i].dst_socket_id);
      for (size_t j = i; j < sess->forwards_count - 1; j++) {
        sess->forwards[j] = sess->forwards[j + 1];
      }
      sess->forwards_count--;
    } else {
      i++;
    }
  }

  return 0;
}

static rtp_socket_t *find_socket_by_fd(rtp_session_t *s, int fd) {
  if (!s || !s->sockets) return NULL;
  for (size_t j = 0; j < s->sockets_count; j++) {
    rtp_socket_t *sock = s->sockets[j];
    if (!sock || !sock->fds) continue;
    for (int i = 1; i <= sock->fds[0]; i++) {
      if (sock->fds[i] == fd) {
        return sock;
      }
    }
  }
  return NULL;
}

PT_THREAD(rtp_session_pt(struct pt *pt, int64_t timestamp, struct pt_task *task)) {
  rtp_session_t *s = task->udata;

  (void)timestamp;
  PT_BEGIN(pt);

  char buffer[RTP_BUFFER_SIZE];

  for (;;) {
    if (s->marked_for_deletion) {
      break;
    }

    if (!s->sockets || s->sockets_count == 0) {
      PT_YIELD(pt);
      continue;
    }

    s->all_fds = realloc(s->all_fds, sizeof(int) * (s->sockets_count * 2 + 1));
    if (!s->all_fds) {
      PT_YIELD(pt);
      continue;
    }
    s->all_fds[0] = 0;

    for (size_t j = 0; j < s->sockets_count; j++) {
      rtp_socket_t *sock = s->sockets[j];
      if (!sock || !sock->fds) continue;
      for (int i = 1; i <= sock->fds[0]; i++) {
        s->all_fds[++s->all_fds[0]] = sock->fds[i];
      }
    }

    if (s->all_fds[0] == 0) {
      PT_YIELD(pt);
      continue;
    }

    PT_WAIT_UNTIL(pt, schedmod_has_data(s->all_fds, &s->ready_fds) > 0);

    if (!s->ready_fds || s->ready_fds[0] == 0) {
      PT_YIELD(pt);
      continue;
    }

    for (int r = 1; r <= s->ready_fds[0]; r++) {
      int ready_fd = s->ready_fds[r];

      rtp_socket_t *src_sock = find_socket_by_fd(s, ready_fd);
      if (!src_sock) continue;

      struct sockaddr_storage from_addr;
      socklen_t from_len = sizeof(from_addr);
      ssize_t n = recvfrom(ready_fd, buffer, sizeof(buffer) - 1, 0,
                           (struct sockaddr *)&from_addr, &from_len);

      if (n <= 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          log_warn("udphole: recvfrom error on socket %s: %s",
                   src_sock->socket_id, strerror(errno));
        }
        continue;
      }

      s->last_activity = time(NULL);

      if (src_sock->mode == 0 && !src_sock->learned_valid) {
        src_sock->learned_addr = from_addr;
        src_sock->learned_addrlen = from_len;
        src_sock->learned_valid = 1;
        log_debug("udphole: socket %s learned remote address", src_sock->socket_id);
      }

      for (size_t i = 0; i < s->forwards_count; i++) {
        if (strcmp(s->forwards[i].src_socket_id, src_sock->socket_id) != 0) {
          continue;
        }

        rtp_socket_t *dst_sock = find_socket(s, s->forwards[i].dst_socket_id);
        if (!dst_sock || !dst_sock->fds || dst_sock->fds[0] == 0) continue;

        struct sockaddr *dest_addr = NULL;
        socklen_t dest_addrlen = 0;

        if (dst_sock->mode == 1) {
          dest_addr = (struct sockaddr *)&dst_sock->remote_addr;
          dest_addrlen = dst_sock->remote_addrlen;
        } else if (dst_sock->learned_valid) {
          dest_addr = (struct sockaddr *)&dst_sock->learned_addr;
          dest_addrlen = dst_sock->learned_addrlen;
        }

        if (dest_addr && dest_addrlen > 0) {
          int dst_fd = dst_sock->fds[1];
          ssize_t sent = sendto(dst_fd, buffer, n, 0, dest_addr, dest_addrlen);
          if (sent < 0) {
            log_warn("udphole: forward failed %s -> %s: %s",
                     src_sock->socket_id, dst_sock->socket_id, strerror(errno));
          }
        }
      }
    }

  }

  log_debug("udphole: session %s protothread exiting", s->session_id);

  if (s->all_fds) {
    free(s->all_fds);
    s->all_fds = NULL;
  }
  if (s->ready_fds) {
    free(s->ready_fds);
    s->ready_fds = NULL;
  }

  PT_END(pt);
}

static void spawn_session_pt(rtp_session_t *s) {
  s->task = (struct pt_task *)(intptr_t)schedmod_pt_create(rtp_session_pt, s);
}

static char cmd_session_create(api_client_t *c, char **args, int nargs) {
  if (nargs < 2) {
    return api_write_err(c, "wrong number of arguments for 'session.create'") ? 1 : 0;
  }

  const char *session_id = args[1];
  int idle_expiry = 0;
  if (nargs >= 3 && args[2] && args[2][0] != '\0') {
    idle_expiry = atoi(args[2]);
  }

  rtp_session_t *s = create_session(session_id, idle_expiry);
  if (!s) {
    return api_write_err(c, "failed to create session") ? 1 : 0;
  }

  spawn_session_pt(s);
  return api_write_ok(c) ? 1 : 0;
}

static char cmd_session_list(api_client_t *c, char **args, int nargs) {
  (void)args;
  if (nargs != 1) {
    return api_write_err(c, "wrong number of arguments for 'session.list'") ? 1 : 0;
  }

  if (!sessions) {
    return api_write_array(c, 0) ? 1 : 0;
  }

  if (!api_write_array(c, sessions_count)) return 0;

  for (size_t i = 0; i < sessions_count; i++) {
    rtp_session_t *s = sessions[i];
    if (!s) continue;
    if (!api_write_bulk_cstr(c, s->session_id)) return 0;
  }

  return 1;
}

static char cmd_session_info(api_client_t *c, char **args, int nargs) {
  if (nargs != 2) {
    return api_write_err(c, "wrong number of arguments for 'session.info'") ? 1 : 0;
  }

  const char *session_id = args[1];
  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  size_t num_items = 8;
  if (!api_write_array(c, num_items)) return 0;

  if (!api_write_bulk_cstr(c, "session_id")) return 0;
  if (!api_write_bulk_cstr(c, s->session_id)) return 0;

  if (!api_write_bulk_cstr(c, "created")) return 0;
  if (!api_write_bulk_int(c, (int)s->created)) return 0;

  if (!api_write_bulk_cstr(c, "last_activity")) return 0;
  if (!api_write_bulk_int(c, (int)s->last_activity)) return 0;

  if (!api_write_bulk_cstr(c, "idle_expiry")) return 0;
  if (!api_write_bulk_int(c, (int)s->idle_expiry)) return 0;

  if (!api_write_bulk_cstr(c, "sockets")) return 0;
  if (!api_write_array(c, s->sockets_count)) return 0;
  for (size_t i = 0; i < s->sockets_count; i++) {
    rtp_socket_t *sock = s->sockets[i];
    if (!sock) continue;
    if (!api_write_bulk_cstr(c, sock->socket_id)) return 0;
  }

  if (!api_write_bulk_cstr(c, "forwards")) return 0;
  if (!api_write_array(c, s->forwards_count * 2)) return 0;
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (!api_write_bulk_cstr(c, s->forwards[i].src_socket_id)) return 0;
    if (!api_write_bulk_cstr(c, s->forwards[i].dst_socket_id)) return 0;
  }

  if (!api_write_bulk_cstr(c, "marked_for_deletion")) return 0;
  if (!api_write_int(c, s->marked_for_deletion)) return 0;

  return 1;
}

static char cmd_session_destroy(api_client_t *c, char **args, int nargs) {
  if (nargs != 2) {
    return api_write_err(c, "wrong number of arguments for 'session.destroy'") ? 1 : 0;
  }

  const char *session_id = args[1];
  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  destroy_session(s);
  return api_write_ok(c) ? 1 : 0;
}

static char cmd_socket_create_listen(api_client_t *c, char **args, int nargs) {
  if (nargs != 3) {
    return api_write_err(c, "wrong number of arguments for 'session.socket.create.listen'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *socket_id = args[2];

  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  rtp_socket_t *sock = create_listen_socket(s, socket_id);
  if (!sock) {
    return api_write_err(c, "failed to create socket") ? 1 : 0;
  }

  if (!api_write_array(c, 2)) return 0;
  if (!api_write_bulk_int(c, sock->local_port)) return 0;
  if (!api_write_bulk_cstr(c, advertise_addr)) return 0;

  return 1;
}

static char cmd_socket_create_connect(api_client_t *c, char **args, int nargs) {
  if (nargs != 5) {
    return api_write_err(c, "wrong number of arguments for 'session.socket.create.connect'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *socket_id = args[2];
  const char *ip = args[3];
  int port = atoi(args[4]);

  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  rtp_socket_t *sock = create_connect_socket(s, socket_id, ip, port);
  if (!sock) {
    return api_write_err(c, "failed to create socket") ? 1 : 0;
  }

  if (!api_write_array(c, 2)) return 0;
  if (!api_write_bulk_int(c, sock->local_port)) return 0;
  if (!api_write_bulk_cstr(c, advertise_addr)) return 0;

  return 1;
}

static char cmd_socket_destroy(api_client_t *c, char **args, int nargs) {
  if (nargs != 3) {
    return api_write_err(c, "wrong number of arguments for 'session.socket.destroy'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *socket_id = args[2];

  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  if (destroy_socket(s, socket_id) != 0) {
    return api_write_err(c, "socket not found") ? 1 : 0;
  }

  return api_write_ok(c) ? 1 : 0;
}

static char cmd_forward_list(api_client_t *c, char **args, int nargs) {
  if (nargs != 2) {
    return api_write_err(c, "wrong number of arguments for 'session.forward.list'") ? 1 : 0;
  }

  const char *session_id = args[1];
  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  if (!api_write_array(c, s->forwards_count * 2)) return 0;
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (!api_write_bulk_cstr(c, s->forwards[i].src_socket_id)) return 0;
    if (!api_write_bulk_cstr(c, s->forwards[i].dst_socket_id)) return 0;
  }

  return 1;
}

static char cmd_forward_create(api_client_t *c, char **args, int nargs) {
  if (nargs != 4) {
    return api_write_err(c, "wrong number of arguments for 'session.forward.create'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *src_socket_id = args[2];
  const char *dst_socket_id = args[3];

  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  rtp_socket_t *src = find_socket(s, src_socket_id);
  if (!src) {
    return api_write_err(c, "source socket not found") ? 1 : 0;
  }

  rtp_socket_t *dst = find_socket(s, dst_socket_id);
  if (!dst) {
    return api_write_err(c, "destination socket not found") ? 1 : 0;
  }

  if (add_forward(s, src_socket_id, dst_socket_id) != 0) {
    return api_write_err(c, "failed to add forward") ? 1 : 0;
  }

  log_debug("udphole: created forward %s -> %s in session %s", src_socket_id, dst_socket_id, session_id);
  return api_write_ok(c) ? 1 : 0;
}

static char cmd_forward_destroy(api_client_t *c, char **args, int nargs) {
  if (nargs != 4) {
    return api_write_err(c, "wrong number of arguments for 'session.forward.destroy'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *src_socket_id = args[2];
  const char *dst_socket_id = args[3];

  rtp_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  if (remove_forward(s, src_socket_id, dst_socket_id) != 0) {
    return api_write_err(c, "forward not found") ? 1 : 0;
  }

  return api_write_ok(c) ? 1 : 0;
}

static void register_rtp_commands(void) {
  api_register_cmd("session.create", cmd_session_create);
  api_register_cmd("session.list", cmd_session_list);
  api_register_cmd("session.info", cmd_session_info);
  api_register_cmd("session.destroy", cmd_session_destroy);
  api_register_cmd("session.socket.create.listen", cmd_socket_create_listen);
  api_register_cmd("session.socket.create.connect", cmd_socket_create_connect);
  api_register_cmd("session.socket.destroy", cmd_socket_destroy);
  api_register_cmd("session.forward.list", cmd_forward_list);
  api_register_cmd("session.forward.create", cmd_forward_create);
  api_register_cmd("session.forward.destroy", cmd_forward_destroy);
  log_info("udphole: registered session.* commands");
}

PT_THREAD(udphole_manager_pt(struct pt *pt, int64_t timestamp, struct pt_task *task)) {
  (void)timestamp;
  log_trace("udphole_manager: protothread entry");
  PT_BEGIN(pt);

  PT_WAIT_UNTIL(pt, global_cfg);

  resp_object *rtp_sec = resp_map_get(global_cfg, "udphole");
  if (!rtp_sec) {
    log_info("udphole: no [udphole] section in config, not starting");
    PT_EXIT(pt);
  }

  const char *mode = resp_map_get_string(rtp_sec, "mode");
  if (!mode || strcmp(mode, "builtin") != 0) {
    log_info("udphole: mode is '%s', not starting builtin server", mode ? mode : "(null)");
    PT_EXIT(pt);
  }

  const char *ports_str = resp_map_get_string(rtp_sec, "ports");
  if (ports_str) {
    sscanf(ports_str, "%d-%d", &port_low, &port_high);
    if (port_low <= 0) port_low = 7000;
    if (port_high <= port_low) port_high = port_low + 999;
  }
  port_cur = port_low;

  const char *advertise_cfg = resp_map_get_string(rtp_sec, "advertise");
  if (advertise_cfg) {
    advertise_addr = strdup(advertise_cfg);
  }

  register_rtp_commands();
  running = 1;
  log_info("udphole: manager started with port range %d-%d", port_low, port_high);

  int64_t last_cleanup = 0;

  for (;;) {
    if (global_cfg) {
      resp_object *new_rtp_sec = resp_map_get(global_cfg, "udphole");
      if (new_rtp_sec) {
        const char *new_mode = resp_map_get_string(new_rtp_sec, "mode");
        if (new_mode && strcmp(new_mode, "builtin") != 0) {
          log_info("udphole: mode changed to '%s', shutting down", new_mode);
          break;
        }
      }
    }

    int64_t now = (int64_t)(time(NULL));
    if (now - last_cleanup >= 1) {
      cleanup_expired_sessions();
      last_cleanup = now;
    }

    PT_YIELD(pt);
  }

  running = 0;

  for (size_t i = 0; i < sessions_count; i++) {
    if (sessions[i]) {
      sessions[i]->marked_for_deletion = 1;
    }
  }

  free(advertise_addr);
  advertise_addr = NULL;

  PT_END(pt);
}
