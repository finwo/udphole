#include "cluster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/resp.h"
#include "common/scheduler.h"
#include "common/url_utils.h"
#include "domain/config.h"
#include "finwo/url-parser.h"
#include "rxi/log.h"

cluster_state_t *cluster_state = NULL;

static int is_session_not_found(resp_object *resp) {
  if (!resp) return 0;
  if (resp->type == RESPT_ERROR && resp->u.s) {
    return (strstr(resp->u.s, "session") != NULL && strstr(resp->u.s, "not found") != NULL) ||
           (strstr(resp->u.s, "not found") != NULL);
  }
  return 0;
}

static resp_object *cluster_forward_to_nodes(const char *cmd_str) {
  size_t           available_count = 0;
  cluster_node_t **available       = cluster_nodes_get_available(cluster_state->nodes, &available_count);

  if (!available || available_count == 0) {
    free(available);
    return resp_error_init("ERR no available nodes");
  }

  for (size_t i = 0; i < available_count; i++) {
    cluster_node_t *node = available[i];

    resp_object *resp = NULL;
    cluster_node_send_command(node, cmd_str, &resp);

    if (!resp) {
      node->available = 0;
      continue;
    }

    if (is_session_not_found(resp)) {
      resp_free(resp);
      continue;
    }

    if (resp->type == RESPT_ERROR) {
      node->available = 0;
      resp_free(resp);
      continue;
    }

    free(available);
    return resp;
  }

  free(available);
  return resp_error_init("ERR session not found");
}

void cluster_init(void) {
  if (cluster_state) {
    cluster_shutdown();
  }

  cluster_state        = calloc(1, sizeof(cluster_state_t));
  cluster_state->nodes = cluster_nodes_create();

  if (!domain_cfg) return;

  resp_object *cluster_nodes = domain_config_get_cluster_nodes();
  if (!cluster_nodes || cluster_nodes->type != RESPT_ARRAY) {
    log_error("cluster: no cluster nodes configured");
    return;
  }

  log_info("cluster: found %zu cluster nodes", cluster_nodes->u.arr.n);
  for (size_t i = 0; i < cluster_nodes->u.arr.n; i++) {
    resp_object *elem = &cluster_nodes->u.arr.elem[i];
    if (elem->type != RESPT_BULK || !elem->u.s) continue;

    const char *address = elem->u.s;

    struct parsed_url *purl = NULL;
    if (parse_address_url(address, &purl) != 0) {
      log_error("cluster: failed to parse address '%s'", address);
      continue;
    }

    char *node_name = NULL;
    if (purl->host && purl->port) {
      asprintf(&node_name, "%s-%s", purl->host, purl->port);
    } else if (purl->scheme && strcmp(purl->scheme, "unix") == 0 && purl->path) {
      const char *basename = strrchr(purl->path, '/');
      basename             = basename ? basename + 1 : purl->path;
      asprintf(&node_name, "unix-%s", basename);
    } else {
      log_error("cluster: cannot generate node name for address '%s'", address);
      parsed_url_free(purl);
      continue;
    }

    cluster_node_t *node = calloc(1, sizeof(cluster_node_t));
    if (cluster_node_init(node, node_name, address, purl->username, purl->password) == 0) {
      cluster_nodes_add(cluster_state->nodes, node);
      sched_create(cluster_node_healthcheck_pt, node);
      log_info("cluster: added node '%s' at %s", node->name, node->address);
    } else {
      cluster_node_free(node);
      free(node);
    }
    free(node_name);
    parsed_url_free(purl);
  }

  cluster_state->initialized = 1;
}

void cluster_reload(void) {
  cluster_shutdown();
  cluster_init();
}

void cluster_shutdown(void) {
  if (!cluster_state) return;
  if (cluster_state->nodes) {
    cluster_nodes_free(cluster_state->nodes);
  }
  free(cluster_state);
  cluster_state = NULL;
}

static char *serialize_args(resp_object *args) {
  char  *cmd_str = NULL;
  size_t cmd_len = 0;
  resp_serialize(args, &cmd_str, &cmd_len);
  return cmd_str;
}

resp_object *cluster_session_create(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  char *cmd_str = serialize_args(args);
  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  cluster_node_t *selected_node  = NULL;
  double          selected_ratio = -1.0;
  resp_object    *resp           = NULL;

  for (int attempt = 0; attempt < 10; attempt++) {
    size_t           available_count = 0;
    cluster_node_t **available       = cluster_nodes_get_available(cluster_state->nodes, &available_count);

    if (!available || available_count == 0) {
      free(available);
      break;
    }

    selected_node  = NULL;
    selected_ratio = -1.0;

    for (size_t i = 0; i < available_count; i++) {
      cluster_node_t *node = available[i];

      resp_object *count_args = resp_array_init();
      resp_array_append_bulk(count_args, "session.count");
      char *count_str = serialize_args(count_args);
      resp_free(count_args);

      size_t node_session_count = 0;
      if (count_str) {
        resp_object *count_resp = NULL;
        if (cluster_node_send_command(node, count_str, &count_resp) == 0 && count_resp &&
            count_resp->type == RESPT_INT) {
          node_session_count = (size_t)count_resp->u.i;
        }
        if (count_resp) resp_free(count_resp);
        free(count_str);
      }

      double ratio = (double)node_session_count / (double)node->weight;
      if (selected_node == NULL || ratio < selected_ratio) {
        selected_node  = node;
        selected_ratio = ratio;
      }
    }

    free(available);

    if (!selected_node) {
      break;
    }

    if (cluster_node_send_command(selected_node, cmd_str, &resp) == 0) {
      free(cmd_str);
      return resp;
    }

    log_debug("cluster: session.create failed on node '%s', marking unavailable", selected_node->name);
    selected_node->available = 0;
    if (resp) {
      resp_free(resp);
      resp = NULL;
    }
  }

  free(cmd_str);
  if (resp) resp_free(resp);
  return resp_error_init("ERR all nodes failed");
}

resp_object *cluster_session_list(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  size_t           available_count = 0;
  cluster_node_t **available       = cluster_nodes_get_available(cluster_state->nodes, &available_count);

  if (!available || available_count == 0) {
    free(available);
    resp_object *res = resp_array_init();
    return res;
  }

  resp_object *result = resp_array_init();

  for (size_t i = 0; i < available_count; i++) {
    cluster_node_t *node = available[i];

    resp_object *cmd_args = resp_array_init();
    resp_array_append_bulk(cmd_args, "session.list");
    char *cmd_str = serialize_args(cmd_args);
    resp_free(cmd_args);

    if (!cmd_str) continue;

    resp_object *resp = NULL;
    if (cluster_node_send_command(node, cmd_str, &resp) == 0 && resp && resp->type == RESPT_ARRAY) {
      for (size_t j = 0; j < resp->u.arr.n; j++) {
        resp_object *elem = &resp->u.arr.elem[j];
        resp_object *copy = resp_deep_copy(elem);
        if (copy) resp_array_append_obj(result, copy);
      }
    }
    if (resp) resp_free(resp);
    free(cmd_str);
  }

  free(available);
  return result;
}

resp_object *cluster_session_info(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  size_t           available_count = 0;
  cluster_node_t **available       = cluster_nodes_get_available(cluster_state->nodes, &available_count);

  if (!available || available_count == 0) {
    free(available);
    return resp_error_init("ERR no available nodes");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 4; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    free(available);
    return resp_error_init("ERR failed to serialize command");
  }

  for (size_t i = 0; i < available_count; i++) {
    cluster_node_t *node = available[i];

    resp_object *resp = NULL;
    if (cluster_node_send_command(node, cmd_str, &resp) == 0) {
      if (resp && resp->type != RESPT_ERROR) {
        free(cmd_str);
        free(available);
        return resp;
      }
      if (resp && !is_session_not_found(resp)) {
        free(cmd_str);
        free(available);
        return resp;
      }
      if (resp) resp_free(resp);
    }
  }

  free(cmd_str);
  free(available);
  return resp_error_init("ERR session not found");
}

resp_object *cluster_session_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 4; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_socket_create_listen(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 8; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_socket_create_connect(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 8; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_socket_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 8; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_forward_list(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 4; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_forward_create(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 8; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_forward_destroy(const char *cmd, resp_object *args) {
  (void)cmd;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  resp_object *cmd_args = resp_array_init();
  for (size_t i = 0; i < args->u.arr.n && i < 8; i++) {
    resp_array_append_obj(cmd_args, resp_deep_copy(&args->u.arr.elem[i]));
  }
  char *cmd_str = serialize_args(cmd_args);
  resp_free(cmd_args);

  if (!cmd_str) {
    return resp_error_init("ERR failed to serialize command");
  }

  return cluster_forward_to_nodes(cmd_str);
}

resp_object *cluster_session_count(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;
  if (!cluster_state || !cluster_state->nodes) {
    return resp_error_init("ERR cluster not initialized");
  }

  size_t           available_count = 0;
  cluster_node_t **available       = cluster_nodes_get_available(cluster_state->nodes, &available_count);

  if (!available || available_count == 0) {
    free(available);
    resp_object *res = malloc(sizeof(resp_object));
    if (!res) return NULL;
    res->type = RESPT_INT;
    res->u.i  = 0;
    return res;
  }

  size_t total_count = 0;

  for (size_t i = 0; i < available_count; i++) {
    cluster_node_t *node = available[i];

    resp_object *cmd_args = resp_array_init();
    resp_array_append_bulk(cmd_args, "session.count");
    char *cmd_str = serialize_args(cmd_args);
    resp_free(cmd_args);

    if (!cmd_str) continue;

    resp_object *resp = NULL;
    if (cluster_node_send_command(node, cmd_str, &resp) == 0 && resp && resp->type == RESPT_INT) {
      total_count += (size_t)resp->u.i;
    }
    if (resp) resp_free(resp);
    free(cmd_str);
  }

  free(available);

  resp_object *res = malloc(sizeof(resp_object));
  if (!res) return NULL;
  res->type = RESPT_INT;
  res->u.i  = (long long)total_count;
  return res;
}

resp_object *cluster_system_load(const char *cmd, resp_object *args) {
  (void)cmd;
  (void)args;

  double loadavg[3];
  if (getloadavg(loadavg, 3) != 3) {
    return resp_error_init("ERR failed to get load average");
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
