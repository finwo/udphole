#include "node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/resp.h"
#include "common/socket_util.h"
#include "rxi/log.h"

#define QUERY_TIMEOUT_MS        500
#define HEALTHCHECK_INTERVAL_MS 5000

static int parse_address(const char *address, char **host, int *port, char **unix_path) {
  if (!address) return -1;

  if (strncmp(address, "tcp://", 6) == 0) {
    const char *hp    = address + 6;
    const char *colon = strchr(hp, ':');
    if (!colon) {
      log_error("cluster: invalid tcp address '%s' (missing port)", address);
      return -1;
    }

    size_t host_len = colon - hp;
    *host           = malloc(host_len + 1);
    if (!*host) return -1;
    memcpy(*host, hp, host_len);
    (*host)[host_len] = '\0';

    *port = atoi(colon + 1);
    if (*port <= 0) {
      log_error("cluster: invalid port in tcp address '%s'", address);
      free(*host);
      *host = NULL;
      return -1;
    }
    *unix_path = NULL;
    return 0;

  } else if (strncmp(address, "unix://", 7) == 0) {
    *unix_path = strdup(address + 7);
    *host      = NULL;
    *port      = 0;
    return 0;
  }

  log_error("cluster: unknown address scheme in '%s' (expected tcp:// or unix://)", address);
  return -1;
}

int cluster_node_init(cluster_node_t *node, const char *name, const char *address, const char *username,
                      const char *password) {
  memset(node, 0, sizeof(*node));

  node->name     = strdup(name);
  node->address  = address ? strdup(address) : NULL;
  node->username = username ? strdup(username) : NULL;
  node->password = password ? strdup(password) : NULL;

  if (!node->address) {
    log_error("cluster: node '%s' has no address configured", name);
    return -1;
  }

  if (parse_address(node->address, &node->host, &node->port, &node->unix_path) != 0) {
    return -1;
  }

  node->fd         = -1;
  node->available  = 0;
  node->last_ping  = 0;
  node->last_check = 0;
  node->weight     = 1;

  return 0;
}

void cluster_node_free(cluster_node_t *node) {
  if (!node) return;
  cluster_node_disconnect(node);
  free(node->name);
  free(node->address);
  free(node->host);
  free(node->unix_path);
  free(node->username);
  free(node->password);
}

static int connect_tcp(const char *host, int port) {
  struct addrinfo hints, *res, *res0;
  int             sockfd, error;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  error = getaddrinfo(host, port_str, &hints, &res0);
  if (error) {
    log_error("cluster: getaddrinfo(%s:%s): %s", host, port_str, gai_strerror(error));
    return -1;
  }

  sockfd = -1;
  for (res = res0; res; res = res->ai_next) {
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) continue;

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0) break;

    close(sockfd);
    sockfd = -1;
  }

  freeaddrinfo(res0);

  if (sockfd >= 0) {
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  }

  return sockfd;
}

static int connect_unix(const char *path) {
  struct sockaddr_un addr;
  int                sockfd;

  if (strlen(path) >= sizeof(addr.sun_path)) {
    log_error("cluster: unix socket path too long: %s", path);
    return -1;
  }

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sockfd);
    return -1;
  }

  return sockfd;
}

int cluster_node_connect(cluster_node_t *node) {
  if (node->fd >= 0) {
    return 0;
  }

  if (node->unix_path) {
    node->fd = connect_unix(node->unix_path);
  } else {
    node->fd = connect_tcp(node->host, node->port);
  }

  if (node->fd < 0) {
    log_debug("cluster: failed to connect to node '%s' at %s", node->name, node->address);
    return -1;
  }

  log_info("cluster: connected to node '%s' at %s", node->name, node->address);

  if (node->username && node->password) {
    char        *cmd_str = NULL;
    size_t       cmd_len = 0;
    resp_object *cmd     = resp_array_init();
    resp_array_append_bulk(cmd, "auth");
    resp_array_append_bulk(cmd, node->username);
    resp_array_append_bulk(cmd, node->password);
    resp_serialize(cmd, &cmd_str, &cmd_len);
    resp_free(cmd);

    if (cmd_str) {
      ssize_t n = send(node->fd, cmd_str, cmd_len, 0);
      if (n > 0) {
        resp_object *resp = resp_read(node->fd);
        if (!resp || resp->type == RESPT_ERROR) {
          log_warn("cluster: authentication failed for node '%s'", node->name);
          close(node->fd);
          node->fd = -1;
          free(cmd_str);
          return -1;
        }
        resp_free(resp);
      }
      free(cmd_str);
    }
  }

  return 0;
}

void cluster_node_disconnect(cluster_node_t *node) {
  if (node->fd >= 0) {
    close(node->fd);
    node->fd = -1;
  }
}

static int send_resp_command(int fd, const char *cmd) {
  size_t  cmd_len = strlen(cmd);
  ssize_t n       = send(fd, cmd, cmd_len, 0);
  return (n == (ssize_t)cmd_len) ? 0 : -1;
}

int cluster_node_send_command(cluster_node_t *node, const char *cmd, resp_object **out_response) {
  if (node->fd < 0) {
    if (cluster_node_connect(node) != 0) {
      return -1;
    }
  }

  if (send_resp_command(node->fd, cmd) != 0) {
    log_debug("cluster: failed to send command to node '%s'", node->name);
    cluster_node_disconnect(node);
    return -1;
  }

  resp_object *resp = resp_read(node->fd);
  if (!resp) {
    log_debug("cluster: no response from node '%s'", node->name);
    cluster_node_disconnect(node);
    return -1;
  }

  *out_response = resp;
  return 0;
}

int cluster_node_healthcheck_pt(int64_t timestamp, struct pt_task *task) {
  cluster_node_t *node = task->udata;

  if (!node) {
    return SCHED_DONE;
  }

  if (node->last_check > 0 && timestamp - node->last_check < HEALTHCHECK_INTERVAL_MS) {
    return SCHED_RUNNING;
  }
  node->last_check = timestamp;

  if (node->fd < 0) {
    if (cluster_node_connect(node) != 0) {
      node->available = 0;
      return SCHED_RUNNING;
    }
  }

  char        *cmd_str = NULL;
  size_t       cmd_len = 0;
  resp_object *cmd     = resp_array_init();
  resp_array_append_bulk(cmd, "ping");
  resp_serialize(cmd, &cmd_str, &cmd_len);
  resp_free(cmd);

  if (!cmd_str) {
    node->available = 0;
    cluster_node_disconnect(node);
    return SCHED_RUNNING;
  }

  ssize_t n = send(node->fd, cmd_str, cmd_len, 0);
  free(cmd_str);

  if (n <= 0) {
    node->available = 0;
    cluster_node_disconnect(node);
    return SCHED_RUNNING;
  }

  resp_object *resp = resp_read(node->fd);
  if (!resp || (resp->type != RESPT_SIMPLE && resp->type != RESPT_BULK)) {
    node->available = 0;
    if (resp) resp_free(resp);
    cluster_node_disconnect(node);
    return SCHED_RUNNING;
  }

  node->available = 1;
  node->last_ping = timestamp;
  log_trace("cluster: node '%s' healthcheck OK", node->name);
  resp_free(resp);

  return SCHED_RUNNING;
}

cluster_nodes_t *cluster_nodes_create(void) {
  cluster_nodes_t *cnodes = calloc(1, sizeof(cluster_nodes_t));
  return cnodes;
}

void cluster_nodes_free(cluster_nodes_t *cnodes) {
  if (!cnodes) return;
  for (size_t i = 0; i < cnodes->nodes_count; i++) {
    if (cnodes->nodes[i]) {
      cluster_node_free(cnodes->nodes[i]);
      free(cnodes->nodes[i]);
    }
  }
  free(cnodes->nodes);
  free(cnodes);
}

int cluster_nodes_add(cluster_nodes_t *cnodes, cluster_node_t *node) {
  cluster_node_t **new_nodes = realloc(cnodes->nodes, sizeof(cluster_node_t *) * (cnodes->nodes_count + 1));
  if (!new_nodes) return -1;
  cnodes->nodes                        = new_nodes;
  cnodes->nodes[cnodes->nodes_count++] = node;
  return 0;
}

cluster_node_t *cluster_nodes_get(cluster_nodes_t *cnodes, const char *name) {
  for (size_t i = 0; i < cnodes->nodes_count; i++) {
    if (cnodes->nodes[i] && strcmp(cnodes->nodes[i]->name, name) == 0) {
      return cnodes->nodes[i];
    }
  }
  return NULL;
}

cluster_node_t **cluster_nodes_get_available(cluster_nodes_t *cnodes, size_t *out_count) {
  size_t count = 0;
  for (size_t i = 0; i < cnodes->nodes_count; i++) {
    if (cnodes->nodes[i] && cnodes->nodes[i]->available) {
      count++;
    }
  }

  if (count == 0) {
    *out_count = 0;
    return NULL;
  }

  cluster_node_t **available = malloc(sizeof(cluster_node_t *) * count);
  if (!available) {
    *out_count = 0;
    return NULL;
  }

  count = 0;
  for (size_t i = 0; i < cnodes->nodes_count; i++) {
    if (cnodes->nodes[i] && cnodes->nodes[i]->available) {
      available[count++] = cnodes->nodes[i];
    }
  }

  *out_count = count;
  return available;
}
