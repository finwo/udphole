#ifndef UDPHOLE_CLUSTER_NODE_H
#define UDPHOLE_CLUSTER_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/resp.h"

typedef struct cluster_node cluster_node_t;

struct cluster_node {
  char *name;
  char *url;
  int weight;
  char *user;
  char *secret;
  int fd;
  int session_count;
  bool available;
  int consecutive_failures;
  int64_t last_check;
  cluster_node_t *next;
};

cluster_node_t *cluster_node_create(const char *name, const char *url, int weight, const char *user, const char *secret);

void cluster_node_free(cluster_node_t *node);

int cluster_node_connect(cluster_node_t *node);

void cluster_node_disconnect(cluster_node_t *node);

resp_object *cluster_node_send_command(cluster_node_t *node, const char *cmd, ...);

int cluster_node_ping(cluster_node_t *node);

int cluster_node_get_session_count(cluster_node_t *node);

void cluster_node_set_available(cluster_node_t *node, bool available);

bool cluster_node_select_weighted_lowest(cluster_node_t *node, cluster_node_t *best);

#endif
