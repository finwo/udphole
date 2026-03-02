#ifndef UDPHOLE_CLUSTER_H
#define UDPHOLE_CLUSTER_H

#include "common/resp.h"
#include "node.h"

typedef struct {
  cluster_nodes_t *nodes;
  int initialized;
} cluster_state_t;

extern cluster_state_t *cluster_state;

void cluster_init(void);

void cluster_reload(void);

void cluster_shutdown(void);

resp_object *cluster_session_create(const char *cmd, resp_object *args);
resp_object *cluster_session_list(const char *cmd, resp_object *args);
resp_object *cluster_session_info(const char *cmd, resp_object *args);
resp_object *cluster_session_destroy(const char *cmd, resp_object *args);
resp_object *cluster_socket_create_listen(const char *cmd, resp_object *args);
resp_object *cluster_socket_create_connect(const char *cmd, resp_object *args);
resp_object *cluster_socket_destroy(const char *cmd, resp_object *args);
resp_object *cluster_forward_list(const char *cmd, resp_object *args);
resp_object *cluster_forward_create(const char *cmd, resp_object *args);
resp_object *cluster_forward_destroy(const char *cmd, resp_object *args);
resp_object *cluster_session_count(const char *cmd, resp_object *args);
resp_object *cluster_system_load(const char *cmd, resp_object *args);

#endif
