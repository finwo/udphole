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
#include "AppModule/udphole.h"
#include "ApiModule/server.h"

#define UDPHOLE_SESSION_HASH_SIZE 256
#define UDPHOLE_BUFFER_SIZE 4096
#define DEFAULT_IDLE_EXPIRY 60

typedef struct udphole_socket {
  char *socket_id;
  int fd;
  int local_port;
  int mode;
  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen;
  int learned_valid;
  struct sockaddr_storage learned_addr;
  socklen_t learned_addrlen;
} udphole_socket_t;

typedef struct udphole_forward {
  char *src_socket_id;
  char *dst_socket_id;
} udphole_forward_t;

typedef struct udphole_session {
  char *session_id;
  time_t idle_expiry;
  time_t created;
  time_t last_activity;
  struct hashmap *sockets;
  udphole_forward_t *forwards;
  size_t forwards_count;
  int *fds;
  int fd_count;
  int marked_for_deletion;
  int *ready_fds;
  struct pt pt;
  struct pt_task *task;
} udphole_session_t;

static struct hashmap *sessions = NULL;
static char *advertise_addr = NULL;
static int port_low = 7000;
static int port_high = 7999;
static int port_cur = 7000;
static int running = 0;

static uint64_t session_hash(const void *item, uint64_t seed0, uint64_t seed1) {
  const udphole_session_t *s = item;
  return hashmap_sip(s->session_id, strlen(s->session_id), seed0, seed1);
}

static int session_compare(const void *a, const void *b, void *udata) {
  (void)udata;
  const udphole_session_t *sa = a;
  const udphole_session_t *sb = b;
  return strcmp(sa->session_id, sb->session_id);
}

static uint64_t socket_hash(const void *item, uint64_t seed0, uint64_t seed1) {
  const udphole_socket_t *s = item;
  return hashmap_sip(s->socket_id, strlen(s->socket_id), seed0, seed1);
}

static int socket_compare(const void *a, const void *b, void *udata) {
  (void)udata;
  const udphole_socket_t *sa = a;
  const udphole_socket_t *sb = b;
  return strcmp(sa->socket_id, sb->socket_id);
}

static udphole_session_t *find_session(const char *session_id) {
  if (!sessions || !session_id) return NULL;
  udphole_session_t key = { .session_id = (char *)session_id };
  return (udphole_session_t *)hashmap_get(sessions, &key);
}

static udphole_socket_t *find_socket(udphole_session_t *s, const char *socket_id) {
  if (!s || !s->sockets || !socket_id) return NULL;
  udphole_socket_t key = { .socket_id = (char *)socket_id };
  return (udphole_socket_t *)hashmap_get(s->sockets, &key);
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

static void close_socket(udphole_socket_t *sock) {
  if (!sock) return;
  if (sock->fd >= 0) {
    close(sock->fd);
    sock->fd = -1;
  }
}

static void free_socket(udphole_socket_t *sock) {
  if (!sock) return;
  close_socket(sock);
  free(sock->socket_id);
  free(sock);
}

static void destroy_session(udphole_session_t *s);

static void session_remove_fds(udphole_session_t *s) {
  if (!s) return;
  if (s->fds) {
    free(s->fds);
    s->fds = NULL;
  }
  s->fd_count = 0;
}

static void session_update_fds(udphole_session_t *s) {
  session_remove_fds(s);
  if (!s || !s->sockets) return;

  int count = (int)hashmap_count(s->sockets);
  if (count == 0) return;

  s->fds = malloc(sizeof(int) * (count + 1));
  if (!s->fds) return;
  s->fds[0] = 0;

  size_t iter = 0;
  void *item;
  while (hashmap_iter(s->sockets, &iter, &item)) {
    udphole_socket_t *sock = item;
    if (sock->fd >= 0) {
      s->fds[++s->fds[0]] = sock->fd;
    }
  }
}

static void destroy_session(udphole_session_t *s) {
  if (!s) return;
  s->marked_for_deletion = 1;
  if (sessions) {
    hashmap_delete(sessions, s);
  }
}

static udphole_session_t *create_session(const char *session_id, int idle_expiry) {
  const udphole_session_t *cs = find_session(session_id);
  if (cs) return (udphole_session_t *)cs;

  udphole_session_t *s = calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->session_id = strdup(session_id);
  s->created = time(NULL);
  s->last_activity = s->created;
  s->idle_expiry = idle_expiry > 0 ? idle_expiry : DEFAULT_IDLE_EXPIRY;

  s->sockets = hashmap_new(sizeof(udphole_socket_t), 0, 0, 0, socket_hash, socket_compare, NULL, NULL);

  if (!sessions) {
    sessions = hashmap_new(sizeof(udphole_session_t), 0, 0, 0, session_hash, session_compare, NULL, NULL);
  }
  hashmap_set(sessions, s);

  log_debug("udphole: created session %s with idle_expiry %ld", session_id, (long)s->idle_expiry);
  return s;
}

static void cleanup_expired_sessions(void) {
  if (!sessions) return;
  time_t now = time(NULL);

  size_t iter = 0;
  void *item;
  while (hashmap_iter(sessions, &iter, &item)) {
    udphole_session_t *s = item;
    if (now - s->last_activity > s->idle_expiry) {
      log_debug("udphole: session %s expired (idle %ld > expiry %ld)",
                s->session_id, (long)(now - s->last_activity), (long)s->idle_expiry);
      destroy_session(s);
    }
  }
}

static int add_forward(udphole_session_t *s, const char *src_id, const char *dst_id) {
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_id) == 0 &&
        strcmp(s->forwards[i].dst_socket_id, dst_id) == 0) {
      return 0;
    }
  }

  udphole_forward_t *new_forwards = realloc(s->forwards, sizeof(udphole_forward_t) * (s->forwards_count + 1));
  if (!new_forwards) return -1;
  s->forwards = new_forwards;

  s->forwards[s->forwards_count].src_socket_id = strdup(src_id);
  s->forwards[s->forwards_count].dst_socket_id = strdup(dst_id);
  s->forwards_count++;

  return 0;
}

static int remove_forward(udphole_session_t *s, const char *src_id, const char *dst_id) {
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

static udphole_socket_t *create_listen_socket(udphole_session_t *sess, const char *socket_id) {
  udphole_socket_t *existing = find_socket(sess, socket_id);
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

  udphole_socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id = strdup(socket_id);
  sock->fd = fds[1];
  sock->local_port = port;
  sock->mode = 0;
  sock->learned_valid = 0;
  free(fds);

  hashmap_set(sess->sockets, sock);
  session_update_fds(sess);

  log_debug("udphole: created listen socket %s in session %s on port %d",
            socket_id, sess->session_id, port);
  return sock;
}

static udphole_socket_t *create_connect_socket(udphole_session_t *sess, const char *socket_id,
                                           const char *ip, int port) {
  udphole_socket_t *existing = find_socket(sess, socket_id);
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

  udphole_socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id = strdup(socket_id);
  sock->fd = fds[1];
  sock->local_port = local_port;
  sock->mode = 1;
  sock->remote_addr = remote_addr;
  sock->remote_addrlen = remote_addrlen;
  sock->learned_valid = 0;
  free(fds);

  hashmap_set(sess->sockets, sock);
  session_update_fds(sess);

  log_debug("udphole: created connect socket %s in session %s on port %d -> %s:%d",
            socket_id, sess->session_id, local_port, ip, port);
  return sock;
}

static int destroy_socket(udphole_session_t *sess, const char *socket_id) {
  udphole_socket_t *sock = find_socket(sess, socket_id);
  if (!sock) return -1;

  hashmap_delete(sess->sockets, sock);
  free_socket(sock);
  session_update_fds(sess);

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

PT_THREAD(udphole_session_pt(struct pt *pt, int64_t timestamp, struct pt_task *task)) {
  udphole_session_t *s = task->udata;

  (void)timestamp;
  log_trace("udphole_session: protothread entry session=%s", s->session_id);
  PT_BEGIN(pt);

  char buffer[UDPHOLE_BUFFER_SIZE];

  for (;;) {
    if (s->marked_for_deletion) {
      break;
    }

    if (!s->fds || s->fd_count == 0) {
      PT_YIELD(pt);
      continue;
    }

    PT_WAIT_UNTIL(pt, schedmod_has_data(s->fds, &s->ready_fds) > 0);

    if (!s->ready_fds || s->ready_fds[0] == 0) {
      PT_YIELD(pt);
      continue;
    }

    for (int r = 1; r <= s->ready_fds[0]; r++) {
      int ready_fd = s->ready_fds[r];

      udphole_socket_t *src_sock = NULL;
      size_t iter = 0;
      void *item;
      while (hashmap_iter(s->sockets, &iter, &item)) {
        udphole_socket_t *sock = item;
        if (sock->fd == ready_fd) {
          src_sock = sock;
          break;
        }
      }

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

        udphole_socket_t *dst_sock = find_socket(s, s->forwards[i].dst_socket_id);
        if (!dst_sock || dst_sock->fd < 0) continue;

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
          ssize_t sent = sendto(dst_sock->fd, buffer, n, 0, dest_addr, dest_addrlen);
          if (sent < 0) {
            log_warn("udphole: forward failed %s -> %s: %s",
                     src_sock->socket_id, dst_sock->socket_id, strerror(errno));
          }
        }
      }
    }
  }

  log_debug("udphole: session %s protothread exiting", s->session_id);

  if (s->ready_fds) {
    free(s->ready_fds);
    s->ready_fds = NULL;
  }

  if (s->fds) {
    free(s->fds);
    s->fds = NULL;
  }

  if (s->sockets) {
    size_t iter = 0;
    void *item;
    while (hashmap_iter(s->sockets, &iter, &item)) {
      udphole_socket_t *sock = item;
      free_socket(sock);
    }
    hashmap_free(s->sockets);
    s->sockets = NULL;
  }

  if (s->forwards) {
    for (size_t i = 0; i < s->forwards_count; i++) {
      free(s->forwards[i].src_socket_id);
      free(s->forwards[i].dst_socket_id);
    }
    free(s->forwards);
    s->forwards = NULL;
    s->forwards_count = 0;
  }

  free(s->session_id);
  free(s);

  PT_END(pt);
}

static void spawn_session_pt(udphole_session_t *s) {
  s->task = (struct pt_task *)(intptr_t)schedmod_pt_create(udphole_session_pt, s);
}

static char cmd_session_create(api_client_t *c, char **args, int nargs) {
  if (nargs != 3) {
    return api_write_err(c, "wrong number of arguments for 'session.create'") ? 1 : 0;
  }

  const char *session_id = args[1];
  int idle_expiry = 0;
  if (nargs >= 4 && args[2] && args[2][0] != '\0') {
    idle_expiry = atoi(args[2]);
  }

  udphole_session_t *s = create_session(session_id, idle_expiry);
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

  size_t count = hashmap_count(sessions);
  if (!api_write_array(c, count)) return 0;

  size_t iter = 0;
  void *item;
  while (hashmap_iter(sessions, &iter, &item)) {
    udphole_session_t *s = item;
    if (!api_write_bulk_cstr(c, s->session_id)) return 0;
  }

  return 1;
}

static char cmd_session_info(api_client_t *c, char **args, int nargs) {
  if (nargs != 2) {
    return api_write_err(c, "wrong number of arguments for 'session.info'") ? 1 : 0;
  }

  const char *session_id = args[1];
  udphole_session_t *s = find_session(session_id);
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
  size_t socket_count = s->sockets ? hashmap_count(s->sockets) : 0;
  if (!api_write_array(c, socket_count)) return 0;
  if (s->sockets) {
    size_t iter = 0;
    void *item;
    while (hashmap_iter(s->sockets, &iter, &item)) {
      udphole_socket_t *sock = item;
      if (!api_write_bulk_cstr(c, sock->socket_id)) return 0;
    }
  }

  if (!api_write_bulk_cstr(c, "forwards")) return 0;
  if (!api_write_array(c, s->forwards_count * 2)) return 0;
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (!api_write_bulk_cstr(c, s->forwards[i].src_socket_id)) return 0;
    if (!api_write_bulk_cstr(c, s->forwards[i].dst_socket_id)) return 0;
  }

  if (!api_write_bulk_cstr(c, "fd_count")) return 0;
  if (!api_write_int(c, s->fd_count)) return 0;

  if (!api_write_bulk_cstr(c, "marked_for_deletion")) return 0;
  if (!api_write_int(c, s->marked_for_deletion)) return 0;

  return 1;
}

static char cmd_session_destroy(api_client_t *c, char **args, int nargs) {
  if (nargs != 2) {
    return api_write_err(c, "wrong number of arguments for 'session.destroy'") ? 1 : 0;
  }

  const char *session_id = args[1];
  udphole_session_t *s = find_session(session_id);
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

  udphole_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  udphole_socket_t *sock = create_listen_socket(s, socket_id);
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

  udphole_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  udphole_socket_t *sock = create_connect_socket(s, socket_id, ip, port);
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

  udphole_session_t *s = find_session(session_id);
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
  udphole_session_t *s = find_session(session_id);
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

  udphole_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  udphole_socket_t *src = find_socket(s, src_socket_id);
  if (!src) {
    return api_write_err(c, "source socket not found") ? 1 : 0;
  }

  udphole_socket_t *dst = find_socket(s, dst_socket_id);
  if (!dst) {
    return api_write_err(c, "destination socket not found") ? 1 : 0;
  }

  if (add_forward(s, src_socket_id, dst_socket_id) != 0) {
    return api_write_err(c, "failed to add forward") ? 1 : 0;
  }

  return api_write_ok(c) ? 1 : 0;
}

static char cmd_forward_destroy(api_client_t *c, char **args, int nargs) {
  if (nargs != 4) {
    return api_write_err(c, "wrong number of arguments for 'session.forward.destroy'") ? 1 : 0;
  }

  const char *session_id = args[1];
  const char *src_socket_id = args[2];
  const char *dst_socket_id = args[3];

  udphole_session_t *s = find_session(session_id);
  if (!s) {
    return api_write_err(c, "session not found") ? 1 : 0;
  }

  if (remove_forward(s, src_socket_id, dst_socket_id) != 0) {
    return api_write_err(c, "forward not found") ? 1 : 0;
  }

  return api_write_ok(c) ? 1 : 0;
}

static void register_udphole_commands(void) {
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

  resp_object *udphole_sec = resp_map_get(global_cfg, "udphole");
  if (!udphole_sec) {
    log_info("udphole: no [udphole] section in config, not starting");
    PT_EXIT(pt);
  }

  const char *mode = resp_map_get_string(udphole_sec, "mode");
  if (!mode || strcmp(mode, "builtin") != 0) {
    log_info("udphole: mode is '%s', not starting builtin server", mode ? mode : "(null)");
    PT_EXIT(pt);
  }

  const char *ports_str = resp_map_get_string(udphole_sec, "ports");
  if (ports_str) {
    sscanf(ports_str, "%d-%d", &port_low, &port_high);
    if (port_low <= 0) port_low = 7000;
    if (port_high <= port_low) port_high = port_low + 999;
  }
  port_cur = port_low;

  const char *advertise_cfg = resp_map_get_string(udphole_sec, "advertise");
  if (advertise_cfg) {
    advertise_addr = strdup(advertise_cfg);
  }

  register_udphole_commands();
  running = 1;
  log_info("udphole: manager started with port range %d-%d", port_low, port_high);

  int64_t last_cleanup = 0;

  for (;;) {
    if (global_cfg) {
      resp_object *new_udphole_sec = resp_map_get(global_cfg, "udphole");
      if (new_udphole_sec) {
        const char *new_mode = resp_map_get_string(new_udphole_sec, "mode");
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

  if (sessions) {
    size_t iter = 0;
    void *item;
    while (hashmap_iter(sessions, &iter, &item)) {
      udphole_session_t *s = item;
      s->marked_for_deletion = 1;
    }
  }

  free(advertise_addr);
  advertise_addr = NULL;

  PT_END(pt);
}
