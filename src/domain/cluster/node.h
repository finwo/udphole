#ifndef UDPHOLE_CLUSTER_NODE_H
#define UDPHOLE_CLUSTER_NODE_H

#include <stdint.h>
#include "common/resp.h"
#include "common/scheduler.h"

typedef struct cluster_node {
  char *name;
  char *address;
  char *host;
  int port;
  char *unix_path;
  char *username;
  char *password;
  int weight;

  int fd;
  int available;
  int64_t last_ping;
  int64_t last_check;
} cluster_node_t;

typedef struct {
  cluster_node_t **nodes;
  size_t nodes_count;
} cluster_nodes_t;

int cluster_node_init(cluster_node_t *node, const char *name, const char *address, const char *username, const char *password);

void cluster_node_free(cluster_node_t *node);

int cluster_node_connect(cluster_node_t *node);

void cluster_node_disconnect(cluster_node_t *node);

int cluster_node_send_command(cluster_node_t *node, const char *cmd, resp_object **out_response);

int cluster_node_healthcheck_pt(int64_t timestamp, struct pt_task *task);

cluster_nodes_t *cluster_nodes_create(void);

void cluster_nodes_free(cluster_nodes_t *cnodes);

int cluster_nodes_add(cluster_nodes_t *cnodes, cluster_node_t *node);

cluster_node_t *cluster_nodes_get(cluster_nodes_t *cnodes, const char *name);

cluster_node_t **cluster_nodes_get_available(cluster_nodes_t *cnodes, size_t *out_count);

#endif
