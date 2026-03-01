#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>

#include "rxi/log.h"
#include "domain/cluster/node.h"
#include "common/resp.h"

#define MAX_HOST_LEN 256
#define MAX_PORT_LEN 16
#define MAX_PATH_LEN 256

static int parse_url(const char *url, char *host, size_t host_len, int *port, char *unix_path, size_t path_len) {
  if (!url) return -1;

  if (strncmp(url, "unix://", 7) == 0) {
    const char *p = url + 7;
    size_t path_len_calc = strlen(p);
    if (path_len_calc >= path_len) path_len_calc = path_len - 1;
    strncpy(unix_path, p, path_len_calc);
    unix_path[path_len_calc] = '\0';
    return 1;
  }

  if (strncmp(url, "tcp://", 6) != 0) {
    return -1;
  }

  const char *p = url + 6;
  const char *colon = strchr(p, ':');
  if (!colon) return -1;

  size_t host_len_calc = colon - p;
  if (host_len_calc >= host_len) host_len_calc = host_len - 1;
  strncpy(host, p, host_len_calc);
  host[host_len_calc] = '\0';

  *port = atoi(colon + 1);
  if (*port <= 0) return -1;

  return 0;
}

cluster_node_t *cluster_node_create(const char *name, const char *url, int weight, const char *user, const char *secret) {
  cluster_node_t *node = calloc(1, sizeof(*node));
  if (!node) return NULL;

  node->name = strdup(name);
  node->url = strdup(url);
  node->weight = weight > 0 ? weight : 1;
  node->user = user ? strdup(user) : NULL;
  node->secret = secret ? strdup(secret) : NULL;
  node->fd = -1;
  node->session_count = 0;
  node->available = false;
  node->consecutive_failures = 0;
  node->last_check = 0;
  node->next = NULL;

  char host[MAX_HOST_LEN];
  int port;
  char unix_path[MAX_PATH_LEN];
  if (parse_url(url, host, sizeof(host), &port, unix_path, sizeof(unix_path)) < 0) {
    log_error("cluster: failed to parse URL %s", url);
    cluster_node_free(node);
    return NULL;
  }

  return node;
}

void cluster_node_free(cluster_node_t *node) {
  if (!node) return;
  cluster_node_disconnect(node);
  free(node->name);
  free(node->url);
  free(node->user);
  free(node->secret);
  free(node);
}

int cluster_node_connect(cluster_node_t *node) {
  if (!node) return -1;

  cluster_node_disconnect(node);

  char host[MAX_HOST_LEN];
  int port;
  char unix_path[MAX_PATH_LEN];
  int url_type = parse_url(node->url, host, sizeof(host), &port, unix_path, sizeof(unix_path));
  if (url_type < 0) {
    return -1;
  }

  int fd;
  if (url_type == 1) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, unix_path, sizeof(addr.sun_path) - 1);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
      close(fd);
      return -1;
    }

    node->fd = fd;
    return 0;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }

  node->fd = fd;
  return 0;
}

void cluster_node_disconnect(cluster_node_t *node) {
  if (!node || node->fd < 0) return;
  close(node->fd);
  node->fd = -1;
}

int cluster_node_ping(cluster_node_t *node) {
  if (!node || node->fd < 0) {
    if (node && cluster_node_connect(node) != 0) {
      return -1;
    }
  }

  resp_object *resp = cluster_node_send_command(node, "ping");
  if (!resp) return -1;

  int ok = (resp->type == RESPT_SIMPLE && resp->u.s && strcmp(resp->u.s, "PONG") == 0);
  resp_free(resp);

  return ok ? 0 : -1;
}

int cluster_node_get_session_count(cluster_node_t *node) {
  if (!node || node->fd < 0) return -1;

  resp_object *resp = cluster_node_send_command(node, "session.count");
  if (!resp) return -1;

  int count = -1;
  if (resp->type == RESPT_INT) {
    count = (int)resp->u.i;
  }
  resp_free(resp);

  return count;
}

resp_object *cluster_node_send_command(cluster_node_t *node, const char *cmd, ...) {
  if (!node || node->fd < 0 || !cmd) return NULL;

  char *buf = NULL;
  size_t len = 0;

  va_list args;
  va_start(args, cmd);

  int argc = 1;
  const char *arg = cmd;
  while (va_arg(args, const char *) != NULL) {
    argc++;
  }
  va_end(args);

  resp_object **argv = malloc(sizeof(resp_object *) * argc);
  if (!argv) return NULL;

  argv[0] = resp_array_init();
  resp_array_append_bulk(argv[0], cmd);

  va_start(args, cmd);
  int i = 1;
  while ((arg = va_arg(args, const char *)) != NULL) {
    argv[i] = resp_array_init();
    resp_array_append_bulk(argv[i], arg);
    i++;
  }
  va_end(args);

  resp_object *cmd_arr = resp_array_init();
  for (i = 0; i < argc; i++) {
    resp_array_append_obj(cmd_arr, argv[i]);
  }

  resp_encode_array(argc, (const resp_object *const *)argv, &buf, &len);

  for (i = 0; i < argc; i++) {
    resp_free(argv[i]);
  }
  free(argv);
  resp_free(cmd_arr);

  if (!buf) return NULL;

  ssize_t written = send(node->fd, buf, len, 0);
  free(buf);

  if (written != (ssize_t)len) {
    return NULL;
  }

  return resp_read(node->fd);
}

void cluster_node_set_available(cluster_node_t *node, bool available) {
  if (!node) return;
  node->available = available;
  if (!available) {
    node->consecutive_failures++;
  } else {
    node->consecutive_failures = 0;
  }
}

bool cluster_node_select_weighted_lowest(cluster_node_t *node, cluster_node_t *best) {
  if (!node || !node->available) return false;

  if (!best) return true;

  double node_ratio = (double)node->session_count / node->weight;
  double best_ratio = (double)best->session_count / best->weight;

  if (node_ratio < best_ratio) return true;
  if (node_ratio > best_ratio) return false;

  if (node->weight > best->weight) return true;
  if (node->weight < best->weight) return false;

  return false;
}
