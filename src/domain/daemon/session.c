#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "common/resp.h"
#include "common/scheduler.h"
#include "common/socket_util.h"
#include "domain/config.h"
#include "rxi/log.h"
#include "tidwall/hashmap.h"

static resp_object *get_udphole_cfg(void) {
  return domain_cfg ? resp_map_get(domain_cfg, "udphole") : NULL;
}

#define SESSION_HASH_SIZE   256
#define BUFFER_SIZE         4096
#define DEFAULT_IDLE_EXPIRY 60

typedef struct socket {
  char                   *socket_id;
  int                    *fds;
  int                     local_port;
  int                     mode;
  struct sockaddr_storage remote_addr;
  socklen_t               remote_addrlen;
  int                     learned_valid;
  struct sockaddr_storage learned_addr;
  socklen_t               learned_addrlen;
} socket_t;

typedef struct forward {
  char *src_socket_id;
  char *dst_socket_id;
} forward_t;

typedef struct session {
  char           *session_id;
  time_t          idle_expiry;
  time_t          created;
  time_t          last_activity;
  socket_t      **sockets;
  size_t          sockets_count;
  forward_t      *forwards;
  size_t          forwards_count;
  int             marked_for_deletion;
  int            *ready_fds;
  int            *all_fds;
  struct pt_task *task;
} session_t;

static session_t **sessions       = NULL;
static size_t      sessions_count = 0;

static session_t *find_session(const char *session_id) {
  for (size_t i = 0; i < sessions_count; i++) {
    if (strcmp(sessions[i]->session_id, session_id) == 0) {
      return sessions[i];
    }
  }
  return NULL;
}

static socket_t *find_socket(session_t *s, const char *socket_id) {
  if (!s || !s->sockets || !socket_id) return NULL;
  for (size_t i = 0; i < s->sockets_count; i++) {
    if (s->sockets[i] && strcmp(s->sockets[i]->socket_id, socket_id) == 0) {
      return s->sockets[i];
    }
  }
  return NULL;
}

static int alloc_port(void) {
  resp_object *udphole = get_udphole_cfg();
  if (!udphole) return 0;
  const char *ports_str = resp_map_get_string(udphole, "ports");
  int         port_low = 7000, port_high = 7999, port_cur = 7000;
  if (ports_str) sscanf(ports_str, "%d-%d", &port_low, &port_high);
  const char *port_cur_str = resp_map_get_string(udphole, "_port_cur");
  if (port_cur_str) port_cur = atoi(port_cur_str);

  for (int i = 0; i < port_high - port_low; i++) {
    int port = port_cur + i;
    if (port > port_high) port = port_low;
    port_cur = port + 1;
    if (port_cur > port_high) port_cur = port_low;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

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
    addr4->sin_port   = htons(port);
    *addrlen          = sizeof(*addr4);
    return 0;
  }

  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, ip_str, &addr6->sin6_addr) == 1) {
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port   = htons(port);
    *addrlen           = sizeof(*addr6);
    return 0;
  }

  return -1;
}

static void close_socket(socket_t *sock) {
  if (!sock || !sock->fds) return;
  for (int i = 1; i <= sock->fds[0]; i++) {
    if (sock->fds[i] >= 0) {
      close(sock->fds[i]);
    }
  }
  free(sock->fds);
  sock->fds = NULL;
}

static void free_socket(socket_t *sock) {
  if (!sock) return;
  close_socket(sock);
  free(sock->socket_id);
  free(sock);
}

static void destroy_session(session_t *s) {
  if (!s) return;
  s->marked_for_deletion = 1;

  for (size_t i = 0; i < s->sockets_count; i++) {
    if (s->sockets[i]) {
      free_socket(s->sockets[i]);
    }
  }
  free(s->sockets);

  for (size_t i = 0; i < s->forwards_count; i++) {
    free(s->forwards[i].src_socket_id);
    free(s->forwards[i].dst_socket_id);
  }
  free(s->forwards);

  free(s->session_id);
  free(s);

  for (size_t i = 0; i < sessions_count; i++) {
    if (sessions[i] == s) {
      for (size_t j = i; j < sessions_count - 1; j++) {
        sessions[j] = sessions[j + 1];
      }
      sessions_count--;
      break;
    }
  }
}

static session_t *create_session(const char *session_id, int idle_expiry) {
  const session_t *cs = find_session(session_id);
  if (cs) return (session_t *)cs;

  session_t *s = calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->session_id    = strdup(session_id);
  s->created       = time(NULL);
  s->last_activity = s->created;
  s->idle_expiry   = idle_expiry > 0 ? idle_expiry : DEFAULT_IDLE_EXPIRY;

  sessions                   = realloc(sessions, sizeof(session_t *) * (sessions_count + 1));
  sessions[sessions_count++] = s;

  return s;
}

static void cleanup_expired_sessions(void) {
  if (!sessions) return;
  time_t now = time(NULL);

  for (size_t i = 0; i < sessions_count; i++) {
    session_t *s = sessions[i];
    if (!s) continue;
    if (now - s->last_activity > s->idle_expiry) {
      log_debug("udphole: session %s expired (idle %ld > expiry %ld)", s->session_id, (long)(now - s->last_activity),
                (long)s->idle_expiry);
      destroy_session(s);
    }
  }
}

static int add_forward(session_t *s, const char *src_id, const char *dst_id) {
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_id) == 0 && strcmp(s->forwards[i].dst_socket_id, dst_id) == 0) {
      return 0;
    }
  }

  forward_t *new_forwards = realloc(s->forwards, sizeof(forward_t) * (s->forwards_count + 1));
  if (!new_forwards) return -1;
  s->forwards = new_forwards;

  s->forwards[s->forwards_count].src_socket_id = strdup(src_id);
  s->forwards[s->forwards_count].dst_socket_id = strdup(dst_id);
  s->forwards_count++;

  return 0;
}

static int remove_forward(session_t *s, const char *src_id, const char *dst_id) {
  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_id) == 0 && strcmp(s->forwards[i].dst_socket_id, dst_id) == 0) {
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

static socket_t *create_listen_socket(session_t *sess, const char *socket_id) {
  socket_t *existing = find_socket(sess, socket_id);
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

  socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id     = strdup(socket_id);
  sock->fds           = fds;
  sock->local_port    = port;
  sock->mode          = 0;
  sock->learned_valid = 0;

  sess->sockets                        = realloc(sess->sockets, sizeof(socket_t *) * (sess->sockets_count + 1));
  sess->sockets[sess->sockets_count++] = sock;

  log_debug("udphole: created listen socket %s in session %s on port %d", socket_id, sess->session_id, port);
  return sock;
}

static socket_t *create_connect_socket(session_t *sess, const char *socket_id, const char *ip, int port) {
  socket_t *existing = find_socket(sess, socket_id);
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
  socklen_t               remote_addrlen;
  if (parse_ip_addr(ip, port, &remote_addr, &remote_addrlen) != 0) {
    log_error("udphole: invalid remote address %s:%d", ip, port);
    free(fds);
    return NULL;
  }

  socket_t *sock = calloc(1, sizeof(*sock));
  if (!sock) {
    free(fds);
    return NULL;
  }

  sock->socket_id      = strdup(socket_id);
  sock->fds            = fds;
  sock->local_port     = local_port;
  sock->mode           = 1;
  sock->remote_addr    = remote_addr;
  sock->remote_addrlen = remote_addrlen;
  sock->learned_valid  = 0;

  sess->sockets                        = realloc(sess->sockets, sizeof(socket_t *) * (sess->sockets_count + 1));
  sess->sockets[sess->sockets_count++] = sock;

  log_debug("udphole: created connect socket %s in session %s on port %d -> %s:%d", socket_id, sess->session_id,
            local_port, ip, port);
  return sock;
}

static int destroy_socket(session_t *sess, const char *socket_id) {
  socket_t *sock = find_socket(sess, socket_id);
  if (!sock) return -1;

  for (size_t i = 0; i < sess->sockets_count; i++) {
    if (sess->sockets[i] == sock) {
      sess->sockets[i] = NULL;
      break;
    }
  }
  free_socket(sock);

  for (size_t i = 0; i < sess->forwards_count;) {
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

static socket_t *find_socket_by_fd(session_t *s, int fd) {
  if (!s || !s->sockets) return NULL;
  for (size_t j = 0; j < s->sockets_count; j++) {
    socket_t *sock = s->sockets[j];
    if (!sock || !sock->fds) continue;
    for (int i = 1; i <= sock->fds[0]; i++) {
      if (sock->fds[i] == fd) {
        return sock;
      }
    }
  }
  return NULL;
}

int session_pt(int64_t timestamp, struct pt_task *task) {
  (void)timestamp;
  session_t *s = task->udata;

  if (s->marked_for_deletion) {
    goto cleanup;
  }

  if (!s->sockets || s->sockets_count == 0) {
    return SCHED_RUNNING;
  }

  s->all_fds = realloc(s->all_fds, sizeof(int) * (s->sockets_count * 2 + 1));
  if (!s->all_fds) {
    return SCHED_RUNNING;
  }
  s->all_fds[0] = 0;

  for (size_t j = 0; j < s->sockets_count; j++) {
    socket_t *sock = s->sockets[j];
    if (!sock || !sock->fds) continue;
    for (int i = 1; i <= sock->fds[0]; i++) {
      s->all_fds[++s->all_fds[0]] = sock->fds[i];
    }
  }

  if (s->all_fds[0] == 0) {
    return SCHED_RUNNING;
  }

  int ready_fd = sched_has_data(s->all_fds);
  if (ready_fd < 0) {
    return SCHED_RUNNING;
  }

  char      buffer[BUFFER_SIZE];
  socket_t *src_sock = find_socket_by_fd(s, ready_fd);
  if (!src_sock) {
    return SCHED_RUNNING;
  }

  struct sockaddr_storage from_addr;
  socklen_t               from_len = sizeof(from_addr);
  ssize_t n = recvfrom(ready_fd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from_addr, &from_len);

  if (n <= 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      log_warn("udphole: recvfrom error on socket %s: %s", src_sock->socket_id, strerror(errno));
    }
    return SCHED_RUNNING;
  }

  s->last_activity = time(NULL);

  if (src_sock->mode == 0 && !src_sock->learned_valid) {
    src_sock->learned_addr    = from_addr;
    src_sock->learned_addrlen = from_len;
    src_sock->learned_valid   = 1;
    log_debug("udphole: socket %s learned remote address", src_sock->socket_id);
  }

  for (size_t i = 0; i < s->forwards_count; i++) {
    if (strcmp(s->forwards[i].src_socket_id, src_sock->socket_id) != 0) {
      continue;
    }

    socket_t *dst_sock = find_socket(s, s->forwards[i].dst_socket_id);
    if (!dst_sock || !dst_sock->fds || dst_sock->fds[0] == 0) continue;

    struct sockaddr *dest_addr    = NULL;
    socklen_t        dest_addrlen = 0;

    if (dst_sock->mode == 1) {
      dest_addr    = (struct sockaddr *)&dst_sock->remote_addr;
      dest_addrlen = dst_sock->remote_addrlen;
    } else if (dst_sock->learned_valid) {
      dest_addr    = (struct sockaddr *)&dst_sock->learned_addr;
      dest_addrlen = dst_sock->learned_addrlen;
    }

    if (dest_addr && dest_addrlen > 0) {
      int     dst_fd = dst_sock->fds[1];
      ssize_t sent   = sendto(dst_fd, buffer, n, 0, dest_addr, dest_addrlen);
      if (sent < 0) {
        log_warn("udphole: forward failed %s -> %s: %s", src_sock->socket_id, dst_sock->socket_id, strerror(errno));
      }
    }
  }

  return SCHED_RUNNING;

cleanup:
  log_debug("udphole: session %s exiting", s->session_id);

  if (s->all_fds) {
    free(s->all_fds);
    s->all_fds = NULL;
  }
  if (s->ready_fds) {
    free(s->ready_fds);
    s->ready_fds = NULL;
  }

  return SCHED_DONE;
}

static void spawn_session_pt(session_t *s) {
  s->task = (struct pt_task *)(intptr_t)sched_create(session_pt, s);
}

resp_object *domain_session_create(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 2) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.create'");
    return err;
  }

  const char *session_id = NULL;
  if (args->u.arr.n > 1 && args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }

  if (!session_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.create'");
    return err;
  }

  int idle_expiry = 0;
  if (args->u.arr.n >= 3 && args->u.arr.elem[2].type == RESPT_BULK && args->u.arr.elem[2].u.s) {
    idle_expiry = atoi(args->u.arr.elem[2].u.s);
  }

  session_t *s = create_session(session_id, idle_expiry);
  if (!s) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR failed to create session");
    return err;
  }

  spawn_session_pt(s);

  resp_object *res = resp_simple_init("OK");
  return res;
}

resp_object *domain_session_list(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;

  resp_object *res = resp_array_init();
  if (!res) return NULL;

  for (size_t i = 0; i < sessions_count; i++) {
    session_t *s = sessions[i];
    if (!s) continue;
    resp_array_append_bulk(res, s->session_id);
  }

  return res;
}

resp_object *domain_session_info(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 2) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.info'");
    return err;
  }

  const char *session_id = NULL;
  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }

  if (!session_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.info'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  resp_object *res = resp_array_init();
  if (!res) return NULL;

  resp_array_append_bulk(res, "session_id");
  resp_array_append_bulk(res, s->session_id);

  resp_array_append_bulk(res, "created");
  resp_array_append_int(res, (long long)s->created);

  resp_array_append_bulk(res, "last_activity");
  resp_array_append_int(res, (long long)s->last_activity);

  resp_array_append_bulk(res, "idle_expiry");
  resp_array_append_int(res, (long long)s->idle_expiry);

  resp_array_append_bulk(res, "sockets");
  resp_object *sockets_arr = resp_array_init();
  for (size_t i = 0; i < s->sockets_count; i++) {
    socket_t *sock = s->sockets[i];
    if (!sock) continue;
    resp_array_append_bulk(sockets_arr, sock->socket_id);
  }
  resp_array_append_obj(res, sockets_arr);

  resp_array_append_bulk(res, "forwards");
  resp_object *forwards_arr = resp_array_init();
  for (size_t i = 0; i < s->forwards_count; i++) {
    resp_array_append_bulk(forwards_arr, s->forwards[i].src_socket_id);
    resp_array_append_bulk(forwards_arr, s->forwards[i].dst_socket_id);
  }
  resp_array_append_obj(res, forwards_arr);

  resp_array_append_bulk(res, "marked_for_deletion");
  resp_array_append_int(res, s->marked_for_deletion);

  return res;
}

resp_object *domain_session_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 2) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.destroy'");
    return err;
  }

  const char *session_id = NULL;
  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }

  if (!session_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.destroy'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  destroy_session(s);

  resp_object *res = resp_simple_init("OK");
  return res;
}

resp_object *domain_socket_create_listen(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 3) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.create.listen'");
    return err;
  }

  const char *session_id = NULL;
  const char *socket_id  = NULL;

  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }
  if (args->u.arr.elem[2].type == RESPT_BULK) {
    socket_id = args->u.arr.elem[2].u.s;
  }

  if (!session_id || !socket_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.create.listen'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  socket_t *sock = create_listen_socket(s, socket_id);
  if (!sock) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR failed to create socket");
    return err;
  }

  resp_object *res = resp_array_init();
  resp_array_append_int(res, sock->local_port);
  resp_object *udphole   = get_udphole_cfg();
  const char  *advertise = udphole ? resp_map_get_string(udphole, "advertise") : NULL;
  resp_array_append_bulk(res, advertise ? advertise : "");
  return res;
}

resp_object *domain_socket_create_connect(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 5) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.create.connect'");
    return err;
  }

  const char *session_id = NULL;
  const char *socket_id  = NULL;
  const char *ip         = NULL;
  const char *port_str   = NULL;

  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }
  if (args->u.arr.elem[2].type == RESPT_BULK) {
    socket_id = args->u.arr.elem[2].u.s;
  }
  if (args->u.arr.elem[3].type == RESPT_BULK) {
    ip = args->u.arr.elem[3].u.s;
  }
  if (args->u.arr.elem[4].type == RESPT_BULK) {
    port_str = args->u.arr.elem[4].u.s;
  }

  if (!session_id || !socket_id || !ip || !port_str) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.create.connect'");
    return err;
  }

  int port = atoi(port_str);

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  socket_t *sock = create_connect_socket(s, socket_id, ip, port);
  if (!sock) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR failed to create socket");
    return err;
  }

  resp_object *res = resp_array_init();
  resp_array_append_int(res, sock->local_port);
  resp_object *udphole   = get_udphole_cfg();
  const char  *advertise = udphole ? resp_map_get_string(udphole, "advertise") : NULL;
  resp_array_append_bulk(res, advertise ? advertise : "");
  return res;
}

resp_object *domain_socket_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 3) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.destroy'");
    return err;
  }

  const char *session_id = NULL;
  const char *socket_id  = NULL;

  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }
  if (args->u.arr.elem[2].type == RESPT_BULK) {
    socket_id = args->u.arr.elem[2].u.s;
  }

  if (!session_id || !socket_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.socket.destroy'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  if (destroy_socket(s, socket_id) != 0) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR socket not found");
    return err;
  }

  resp_object *res = resp_simple_init("OK");
  return res;
}

resp_object *domain_forward_list(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 2) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.list'");
    return err;
  }

  const char *session_id = NULL;
  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }

  if (!session_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.list'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  resp_object *res = resp_array_init();
  for (size_t i = 0; i < s->forwards_count; i++) {
    resp_array_append_bulk(res, s->forwards[i].src_socket_id);
    resp_array_append_bulk(res, s->forwards[i].dst_socket_id);
  }
  return res;
}

resp_object *domain_forward_create(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 4) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.create'");
    return err;
  }

  const char *session_id    = NULL;
  const char *src_socket_id = NULL;
  const char *dst_socket_id = NULL;

  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }
  if (args->u.arr.elem[2].type == RESPT_BULK) {
    src_socket_id = args->u.arr.elem[2].u.s;
  }
  if (args->u.arr.elem[3].type == RESPT_BULK) {
    dst_socket_id = args->u.arr.elem[3].u.s;
  }

  if (!session_id || !src_socket_id || !dst_socket_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.create'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  socket_t *src = find_socket(s, src_socket_id);
  if (!src) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR source socket not found");
    return err;
  }

  socket_t *dst = find_socket(s, dst_socket_id);
  if (!dst) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR destination socket not found");
    return err;
  }

  if (add_forward(s, src_socket_id, dst_socket_id) != 0) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR failed to add forward");
    return err;
  }

  log_debug("udphole: created forward %s -> %s in session %s", src_socket_id, dst_socket_id, session_id);

  resp_object *res = resp_simple_init("OK");
  return res;
}

resp_object *domain_forward_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!args || args->type != RESPT_ARRAY || args->u.arr.n < 4) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.destroy'");
    return err;
  }

  const char *session_id    = NULL;
  const char *src_socket_id = NULL;
  const char *dst_socket_id = NULL;

  if (args->u.arr.elem[1].type == RESPT_BULK) {
    session_id = args->u.arr.elem[1].u.s;
  }
  if (args->u.arr.elem[2].type == RESPT_BULK) {
    src_socket_id = args->u.arr.elem[2].u.s;
  }
  if (args->u.arr.elem[3].type == RESPT_BULK) {
    dst_socket_id = args->u.arr.elem[3].u.s;
  }

  if (!session_id || !src_socket_id || !dst_socket_id) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR wrong number of arguments for 'session.forward.destroy'");
    return err;
  }

  session_t *s = find_session(session_id);
  if (!s) {
    resp_object *err = resp_error_init("ERR session not found");
    return err;
  }

  if (remove_forward(s, src_socket_id, dst_socket_id) != 0) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR forward not found");
    return err;
  }

  resp_object *res = resp_simple_init("OK");
  return res;
}

resp_object *domain_system_load(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;

  double loadavg[3];
  if (getloadavg(loadavg, 3) != 3) {
    resp_object *err = resp_array_init();
    resp_array_append_simple(err, "ERR failed to get load average");
    return err;
  }

  resp_object *res = resp_array_init();
  char         buf[64];

  resp_array_append_bulk(res, "1min");
  snprintf(buf, sizeof(buf), "%.2f", loadavg[0]);
  resp_array_append_bulk(res, buf);

  resp_array_append_bulk(res, "5min");
  snprintf(buf, sizeof(buf), "%.2f", loadavg[1]);
  resp_array_append_bulk(res, buf);

  resp_array_append_bulk(res, "15min");
  snprintf(buf, sizeof(buf), "%.2f", loadavg[2]);
  resp_array_append_bulk(res, buf);

  return res;
}

resp_object *domain_session_count(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;

  size_t count = 0;
  for (size_t i = 0; i < sessions_count; i++) {
    if (sessions[i] != NULL) {
      count++;
    }
  }

  resp_object *res = malloc(sizeof(resp_object));
  if (!res) return NULL;
  res->type = RESPT_INT;
  res->u.i  = (long long)count;
  return res;
}

int session_manager_pt(int64_t timestamp, struct pt_task *task) {
  session_manager_udata_t *udata = task->udata;

  resp_object *udphole = get_udphole_cfg();
  if (!udphole) {
    return SCHED_RUNNING;
  }

  if (!udata->initialized) {
    const char *ports_str = resp_map_get_string(udphole, "ports");
    int         port_low = 7000, port_high = 7999;
    if (ports_str) sscanf(ports_str, "%d-%d", &port_low, &port_high);
    log_info("udphole: manager started with port range %d-%d", port_low, port_high);
    udata->initialized = 1;
  }

  if (timestamp - udata->last_cleanup >= 1000) {
    cleanup_expired_sessions();
    udata->last_cleanup = timestamp;
  }

  return SCHED_RUNNING;
}
